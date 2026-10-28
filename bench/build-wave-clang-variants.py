#!/usr/bin/env python3
"""Build isolated Clang runtime variants; train LLVM PGO, never benchmark.

Each variant cleans the source tree and snapshots its matching bender/static
archive before the next clean. Instrumentation covers both shell and runtime;
this is LLVM PGO, not Janet's --aot --native --pgo type-profile pass.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

PIN = "a69c09843714e1733660e5335b285dd383680d23"
WAVE = Path("bench/cross-runtime/wave.janet")
VARIANTS = ("baseline", "instrumented", "profile-use", "lto")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def command(argv, *, cwd, env, timeout, stdout_path, stderr_path):
    with stdout_path.open("wb") as out, stderr_path.open("wb") as err:
        try:
            result = subprocess.run(argv, cwd=cwd, env=env, stdout=out,
                                    stderr=err, timeout=timeout, check=False)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"timed out after {timeout}s: {argv}; see {stderr_path}") from exc
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {argv}; see {stderr_path}")


def capture(argv, *, cwd, timeout=20):
    return subprocess.run(argv, cwd=cwd, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=timeout, check=True).stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--evidence-root", required=True, type=Path)
    parser.add_argument("--clang", help="Clang executable (default: xcrun --find clang)")
    parser.add_argument("--profdata", help="llvm-profdata executable (default: xcrun --find llvm-profdata)")
    parser.add_argument("--make", default="make", dest="make_cmd")
    parser.add_argument("--ar", default="/usr/bin/ar")
    parser.add_argument("--build-timeout", type=int, default=1800)
    parser.add_argument("--train-timeout", type=int, default=180)
    args = parser.parse_args()
    if args.build_timeout <= 0 or args.train_timeout <= 0:
        parser.error("timeouts must be positive")
    root = args.source_root.resolve(strict=True)
    evidence = args.evidence_root.resolve()
    if evidence == root or root in evidence.parents:
        parser.error("evidence root must be outside the source tree")
    if evidence.exists():
        parser.error("evidence root already exists; choose a fresh path")
    if capture(["git", "rev-parse", "HEAD"], cwd=root) != PIN:
        parser.error(f"source root must be pinned at {PIN}")
    if capture(["git", "status", "--porcelain"], cwd=root):
        parser.error("source root has tracked/untracked changes")
    wave = root / WAVE
    if not wave.is_file():
        parser.error(f"missing benchmark: {wave}")
    clang = args.clang or capture(["xcrun", "--find", "clang"], cwd=root)
    profdata = args.profdata or capture(["xcrun", "--find", "llvm-profdata"], cwd=root)
    clang = str(Path(clang).resolve(strict=True))
    profdata = str(Path(profdata).resolve(strict=True))
    ar = str(Path(args.ar).resolve(strict=True))
    make_cmd = shutil.which(args.make_cmd)
    if not make_cmd:
        parser.error(f"make executable not found: {args.make_cmd}")
    make_cmd = str(Path(make_cmd).resolve(strict=True))
    evidence.mkdir(parents=True)
    logs = evidence / "logs"
    logs.mkdir()
    raw = evidence / "raw"
    raw.mkdir()
    env = os.environ.copy()
    # A directly invoked Xcode toolchain clang needs the selected SDK to find
    # system headers; keep the same SDK for every build and training run.
    sdkroot = env.get("SDKROOT") or capture(["xcrun", "--show-sdk-path"], cwd=root)
    if not Path(sdkroot).is_dir():
        parser.error(f"SDKROOT does not name a directory: {sdkroot}")
    env["SDKROOT"] = str(Path(sdkroot).resolve())
    # Neither Janet's type profiler nor an inherited compiler/profile path may
    # contaminate LLVM training. Explicit Make arguments control build flags.
    for key in ("LLVM_PROFILE_FILE", "JANET_PROFILE", "JANET_PROFILE_OUT",
                "NATIVE_COMMIT_PROBE", "CPATH", "CFLAGS", "LDFLAGS"):
        env.pop(key, None)
    manifest = {"source_commit": PIN, "wave_sha256": digest(wave),
                "sdkroot": env["SDKROOT"],
                "clang": clang, "clang_version": capture([clang, "--version"], cwd=root).splitlines()[0],
                "profdata": profdata,
                "profdata_version": capture([profdata, "--version"], cwd=root).splitlines()[0],
                "ar": ar, "make": make_cmd, "variants": {}, "training": []}
    common = [f"CC={clang}", f"HOSTCC={clang}", f"AR={ar}",
              f"HOSTAR={ar}", "NATIVE_EXECUTORS=1"]

    def step(name, argv, timeout, step_env=env):
        command(argv, cwd=root, env=step_env, timeout=timeout,
                stdout_path=logs / f"{name}.stdout", stderr_path=logs / f"{name}.stderr")

    def build(name, cflags, ldflags="-rdynamic"):
        step(f"{name}-clean", [make_cmd, "clean"], args.build_timeout)
        argv = [make_cmd, "-j1", *common, f"CFLAGS={cflags}",
                f"LDFLAGS={ldflags}", "build/bender", "build/libjanet.a"]
        step(f"{name}-build", argv, args.build_timeout)
        stamp = (root / "build/.native-executors-mode").read_text().strip()
        if stamp != "enabled":
            raise RuntimeError(f"{name}: native mode stamp is {stamp!r}, expected enabled")
        dest = evidence / name
        dest.mkdir()
        files = {}
        for filename in ("bender", "libjanet.a"):
            src = root / "build" / filename
            if not src.is_file() or not src.stat().st_size:
                raise RuntimeError(f"{name}: missing/empty {src}")
            target = dest / filename
            shutil.copy2(src, target)
            files[filename] = {"sha256": digest(target), "bytes": target.stat().st_size}
        manifest["variants"][name] = {"make_argv": argv, "files": files,
                                      "native_mode": stamp}
        save_manifest()

    def save_manifest():
        (evidence / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    save_manifest()
    build("baseline", "-O2 -g")
    build("instrumented", "-O2 -g -fprofile-instr-generate",
          "-rdynamic -fprofile-instr-generate")
    # Exactly four non-overlapping runs. The stdout is a semantic gate, not
    # timing evidence; stderr carries inherently variable clock readings.
    for lanes in (0, 2):
        for n in (1000, 2000):
            tag = f"train-lanes{lanes}-n{n}"
            profile = raw / f"{tag}-%p.profraw"
            train_env = env.copy()
            train_env.update(LLVM_PROFILE_FILE=str(profile), JANET_NO_PROFILE="1",
                             JANET_LANES=str(lanes))
            step(tag, [str(evidence / "instrumented/bender"), str(wave), str(n)],
                 args.train_timeout, train_env)
            actual = (logs / f"{tag}.stdout").read_text().strip()
            if actual != f"procs={n} result={n}":
                raise RuntimeError(f"{tag}: incorrect stdout {actual!r}")
            matched = sorted(raw.glob(f"{tag}-*.profraw"))
            if not matched or any(not path.stat().st_size for path in matched):
                raise RuntimeError(f"{tag}: no nonempty LLVM raw profiles")
            manifest["training"].append({"lanes": lanes, "n": n,
                                         "profiles": [{"file": p.name, "sha256": digest(p)} for p in matched]})
            save_manifest()
    profiles = sorted(raw.glob("*.profraw"))
    merged = evidence / "wave.profdata"
    step("profdata-merge", [profdata, "merge", f"-output={merged}",
                            *map(str, profiles)], args.build_timeout)
    if not merged.is_file() or not merged.stat().st_size:
        raise RuntimeError("merged LLVM profile missing/empty")
    manifest["profile_sha256"] = digest(merged)
    save_manifest()
    build("profile-use", f"-O2 -g -fprofile-instr-use={merged}")
    warnings = (logs / "profile-use-build.stderr").read_text()
    if re.search(r"profile.*(?:mismatch|out.of.date|invalid|missing)|(?:mismatch|out.of.date).*profile", warnings, re.I):
        raise RuntimeError("profile-use build reported incompatible/missing profile; inspect log")
    # Independent LTO-only arm: no instrumentation or profile-use flags.
    build("lto", "-O2 -g -flto", "-rdynamic -flto")
    if capture(["git", "rev-parse", "HEAD"], cwd=root) != PIN or capture(["git", "status", "--porcelain"], cwd=root):
        raise RuntimeError("source revision or tracked tree changed during build")
    manifest["complete"] = True
    save_manifest()
    print(f"completed four matched pairs; evidence: {evidence}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired, RuntimeError) as exc:
        print(f"wave Clang build stopped: {exc}", file=sys.stderr)
        sys.exit(1)
