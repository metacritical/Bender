#!/usr/bin/env python3
"""Controlled, evidence-producing runner for Phase 4 trampoline-wave trials.

Why this exists
---------------
``bench/cross-runtime/run.sh`` prints a table for a human. That is fine for a
smoke test, but it is not evidence: it keeps one number per runtime, discards
repeats, and can render a FAILED run as an empty or misleading row. This runner
is the controlled instrument instead.

Guarantees
----------
* Sequential trials only. One child at a time, so trials never contend.
* ``argv`` is executed literally -- no shell -- and the manifest may only
  interpolate ``{n}``, ``{repeat}`` and ``{variant}``.
* Exact outcome per trial: exit status, terminating signal, timeout flag and
  wall seconds. Only on timeout does the runner kill the child, and it kills
  the child's own process group (the child is started with
  ``start_new_session=True``), never an unrelated process.
* Every trial keeps its stdout and stderr, plus a JSON record, and the run
  keeps an append-only ``trials.jsonl``.
* Waves are checked for correctness, not just timing: ``result`` must equal the
  requested N and, when the program reports ``procs``, that must equal N too.
  A nonzero exit, a signal, a timeout, a wrong result or a missing result all
  mark the trial failed, and any failed trial makes the process exit 1.
* Repetitions are retained individually and summarized with min/median/mean/
  max/stdev. A minimum alone is never the result.
* Output precision is preserved and reported, never silently coerced. The
  legacy harnesses print **integer milliseconds**, so a sub-millisecond phase
  collapses to ``0`` before this runner sees it. Each trial records
  ``metric_text`` (exactly what was printed) and ``metric_resolution`` (the
  step that text can express); the summary takes the coarsest step per metric
  and emits ``precision_notes`` stating the value is quantised at the source.
  A harness printing real decimals, or a JSON object record, keeps every
  printed digit.
* Provenance is recorded: git revision and dirty state, machine identity,
  Python version, the manifest hash, and the SHA-256 of every executable.

Output layout
-------------
``<output_dir>/run.json``            run-level provenance and manifest snapshot
``<output_dir>/trials.jsonl``        one flushed JSON line per trial
``<output_dir>/summary.json``        stats + every retained sample
``<output_dir>/<variant>/n<N>/rep<k>/stdout.log``
``<output_dir>/<variant>/n<N>/rep<k>/stderr.log``
``<output_dir>/<variant>/n<N>/rep<k>/trial.json``
``<output_dir>/<variant>/n<N>/rep<k>/sample.txt``   (macOS, opt-in only)

Manifest
--------
::

    {
      "repo": ".",
      "output_dir": "build/wave-evidence",
      "sizes": [100000],
      "repeats": 3,
      "timeout_s": 600,
      "env": {"JANET_NO_PROFILE": "1"},
      "variants": [
        {
          "name": "janet-lanes0",
          "argv": ["./build/bender", "bench/cross-runtime/wave.janet", "{n}"],
          "env": {"JANET_LANES": "0"},
          "env_unset": ["JANET_PROFILE"]
        },
        {
          "name": "go",
          "argv": ["./bench/cross-runtime/wave-go", "{n}"],
          "cwd": "."
        }
      ]
    }

Per-variant ``sizes``, ``repeats``, ``timeout_s``, ``cwd`` and ``env`` override
the top-level values. ``env`` is applied on top of the inherited environment
and recorded as an explicit override; ``env_unset`` removes variables.
``repo``, ``output_dir`` and ``cwd`` are resolved relative to the directory
holding the manifest, so a checked-in manifest is location-independent.

``sample`` (macOS only, opt-in per variant) attaches ``/usr/bin/sample`` to the
exact child pid for ``sample_seconds``. It is diagnostic evidence only: it is
recorded, its absence is recorded as a reason, and it never changes a trial's
verdict. Wall-clock profiling integrated into the trial was deliberately left
out -- an in-process profiler changes the thing being measured, and a wrapper
that fails would silently corrupt the numbers.

Exit codes: 0 all trials passed, 1 at least one trial failed, 2 manifest or
invocation error.

Usage
-----
    python3 bench/wave-controlled-run.py manifest.json
    python3 bench/wave-controlled-run.py manifest.json --output-dir /tmp/run1
    python3 bench/wave-controlled-run.py manifest.json --only janet-lanes0
    python3 bench/wave-controlled-run.py --print-example-manifest
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone

SCHEMA = "wave-controlled-run/1"
PLACEHOLDER_RE = re.compile(r"\{([^{}]*)\}")
# Only a bare identifier is a placeholder. Anything else inside braces is left
# literal, so an argv template may carry shell-free f-strings or JSON.
IDENTIFIER_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ALLOWED_PLACEHOLDERS = ("n", "repeat", "variant")
# Bare keys emitted by bench/wave.go and bench/wave.erl mean the same thing as
# the explicit *_ms keys emitted by bench/cross-runtime/wave.go.
BARE_MS_KEYS = ("spawn", "wave", "total")

# Failure kinds, in the order they are reported.
FAILURE_TIMEOUT = "timeout"
FAILURE_SIGNAL = "signal"
FAILURE_NONZERO = "nonzero_exit"
FAILURE_MISSING_RESULT = "missing_result"
FAILURE_RESULT_MISMATCH = "result_mismatch"
FAILURE_PROCS_MISMATCH = "procs_mismatch"
FAILURE_METRIC_CONFLICT = "metric_conflict"
FAILURE_SPAWN_ERROR = "spawn_error"

EXAMPLE_MANIFEST = {
    "repo": ".",
    "output_dir": "build/wave-evidence",
    "sizes": [10000, 100000],
    "repeats": 3,
    "timeout_s": 600,
    "env": {"JANET_NO_PROFILE": "1"},
    "variants": [
        {
            "name": "janet-script-lanes0",
            "argv": ["./build/bender", "bench/cross-runtime/wave.janet", "{n}"],
            "env": {"JANET_LANES": "0"},
        },
        {
            "name": "janet-aot-lanes0",
            "argv": ["./build/wave-pgo-evidence/wave", "{n}"],
            "env": {"JANET_LANES": "0"},
        },
        {
            "name": "go",
            "argv": ["./bench/cross-runtime/wave-go", "{n}"],
        },
        {
            "name": "erlang",
            "argv": [
                "erl",
                "-noshell",
                "+P",
                "8000000",
                "-pa",
                "build/beam",
                "-eval",
                'wave:main(["{n}"]),halt().',
            ],
        },
    ],
}


class ManifestError(Exception):
    """A manifest (or invocation) problem, not a trial failure."""


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------
def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def sha256_file(path: str):
    """Return the hex digest of path, or None when it cannot be read."""
    try:
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def signal_name(signum):
    try:
        return signal.Signals(signum).name
    except (ValueError, TypeError):
        return None


def git_info(repo: str):
    """Revision and dirty state, or a recorded reason when git is unusable."""

    def run(*args):
        try:
            proc = subprocess.run(
                ["git", "-C", repo, *args],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=20,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            return None, str(exc)
        return proc, proc.stderr.strip()

    info = {"repo": repo, "rev": None, "branch": None, "dirty": None, "error": None}
    proc, err = run("rev-parse", "HEAD")
    if proc is None or proc.returncode != 0:
        info["error"] = err or (proc.stdout or "").strip() or "git rev-parse failed"
        return info
    info["rev"] = proc.stdout.strip()

    proc, _ = run("rev-parse", "--abbrev-ref", "HEAD")
    if proc is not None and proc.returncode == 0:
        info["branch"] = proc.stdout.strip()

    proc, err = run("status", "--porcelain")
    if proc is None or proc.returncode != 0:
        info["error"] = err or "git status failed"
        return info
    info["dirty"] = bool(proc.stdout.strip())
    return info


def machine_info():
    return {
        "system": platform.system(),
        "release": platform.release(),
        "version": platform.version(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "cpu_count": os.cpu_count(),
        "hostname": platform.node(),
        "python": {
            "version": platform.python_version(),
            "implementation": platform.python_implementation(),
            "executable": sys.executable,
        },
    }


# --------------------------------------------------------------------------
# manifest handling
# --------------------------------------------------------------------------
def _require_number(value, field):
    try:
        number = float(value)
    except (TypeError, ValueError):
        raise ManifestError(f"{field} must be a number, got {value!r}")
    if number <= 0:
        raise ManifestError(f"{field} must be positive, got {value!r}")
    return number


def _require_positive_int(value, field):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ManifestError(f"{field} must be a positive integer, got {value!r}")
    if value < 1:
        raise ManifestError(f"{field} must be >= 1, got {value!r}")
    return value


def _env_map(value, field):
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ManifestError(f"{field} must be an object of strings")
    out = {}
    for key, item in value.items():
        if not isinstance(key, str) or not isinstance(item, str):
            raise ManifestError(f"{field} entries must be string -> string")
        out[key] = item
    return out


def _env_unset(value, field):
    if value is None:
        return []
    if not isinstance(value, list) or any(not isinstance(v, str) for v in value):
        raise ManifestError(f"{field} must be a list of strings")
    return list(value)


def validate_placeholders(argv, where):
    for token in argv:
        for name in PLACEHOLDER_RE.findall(token):
            if not IDENTIFIER_RE.match(name):
                # Braces that do not contain a bare identifier are literal
                # text (an f-string, JSON, a shell-free format specifier).
                continue
            if name not in ALLOWED_PLACEHOLDERS:
                raise ManifestError(
                    f"{where}: unknown placeholder '{{{name}}}' in argv entry "
                    f"{token!r}; allowed: "
                    + ", ".join("{" + p + "}" for p in ALLOWED_PLACEHOLDERS)
                )


def _resolve_paths(raw, base_dir):
    """Resolve repo and output_dir. output_dir is repo-relative (build/ is ignored)."""
    repo = raw.get("repo", ".")
    if not isinstance(repo, str):
        raise ManifestError("repo must be a string path")
    repo = os.path.abspath(os.path.join(base_dir, repo))

    output_dir = raw.get("output_dir", "build/wave-evidence")
    if not isinstance(output_dir, str) or not output_dir:
        raise ManifestError("output_dir must be a non-empty string")
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(repo, output_dir)
    return repo, os.path.abspath(output_dir)


def _variant_name(entry, where, seen):
    name = entry.get("name")
    if not isinstance(name, str) or not name:
        raise ManifestError(f"{where}.name must be a non-empty string")
    if os.sep in name or name in (".", ".."):
        raise ManifestError(f"{where}.name must not contain a path separator")
    if name in seen:
        raise ManifestError(f"duplicate variant name {name!r}")
    seen.add(name)
    return name


def _variant_argv(entry, where):
    argv = entry.get("argv")
    if (not isinstance(argv, list) or not argv
            or any(not isinstance(tok, str) for tok in argv)):
        raise ManifestError(f"{where}.argv must be a non-empty list of strings")
    validate_placeholders(argv, where)
    return list(argv)


def _variant_sizes(entry, where, defaults):
    sizes = entry.get("sizes", defaults["sizes"])
    if not isinstance(sizes, list) or not sizes:
        raise ManifestError(
            f"{where}: sizes must be a non-empty list (set it on the variant "
            "or at the top level)"
        )
    return [_require_positive_int(size, f"{where}.sizes[]") for size in sizes]


def _variant_env(entry, where, defaults):
    env = dict(_env_map(defaults["env"], "env"))
    env.update(_env_map(entry.get("env"), f"{where}.env"))
    env_unset = list(_env_unset(defaults["env_unset"], "env_unset"))
    env_unset.extend(_env_unset(entry.get("env_unset"), f"{where}.env_unset"))
    return env, env_unset


def _variant_cwd(entry, where, defaults, repo):
    cwd_raw = entry.get("cwd", defaults["cwd"])
    cwd = repo if cwd_raw is None else os.path.abspath(os.path.join(repo, cwd_raw))
    if not os.path.isdir(cwd):
        raise ManifestError(f"{where}.cwd does not exist: {cwd}")
    return cwd


def _normalize_variant(entry, index, defaults, repo, seen):
    where = f"variants[{index}]"
    if not isinstance(entry, dict):
        raise ManifestError(f"{where} must be an object")

    sample = entry.get("sample", False)
    if not isinstance(sample, bool):
        raise ManifestError(f"{where}.sample must be a boolean")

    env, env_unset = _variant_env(entry, where, defaults)
    return {
        "name": _variant_name(entry, where, seen),
        "argv": _variant_argv(entry, where),
        "sizes": _variant_sizes(entry, where, defaults),
        "repeats": _require_positive_int(
            entry.get("repeats", defaults["repeats"]), f"{where}.repeats"
        ),
        "timeout_s": _require_number(
            entry.get("timeout_s", defaults["timeout_s"]), f"{where}.timeout_s"
        ),
        "env": env,
        "env_unset": env_unset,
        "cwd": _variant_cwd(entry, where, defaults, repo),
        "sample": sample,
        "sample_seconds": _require_number(
            entry.get("sample_seconds", 10), f"{where}.sample_seconds"
        ),
    }


def normalize_manifest(raw, base_dir):
    if not isinstance(raw, dict):
        raise ManifestError("manifest must be a JSON object")

    repo, output_dir = _resolve_paths(raw, base_dir)

    defaults = {
        "sizes": raw.get("sizes"),
        "repeats": raw.get("repeats", 1),
        "timeout_s": raw.get("timeout_s", 600),
        "env": raw.get("env"),
        "env_unset": raw.get("env_unset"),
        "cwd": raw.get("cwd"),
    }

    variants_raw = raw.get("variants")
    if not isinstance(variants_raw, list) or not variants_raw:
        raise ManifestError("variants must be a non-empty list")

    variants = []
    seen = set()
    for index, entry in enumerate(variants_raw):
        variants.append(_normalize_variant(entry, index, defaults, repo, seen))

    return {
        "repo": repo,
        "output_dir": output_dir,
        "variants": variants,
        "raw": raw,
    }


def load_manifest(path):
    try:
        with open(path, "rb") as handle:
            payload = handle.read()
    except OSError as exc:
        raise ManifestError(f"cannot read manifest {path}: {exc}")
    try:
        raw = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ManifestError(f"manifest {path} is not valid JSON: {exc}")
    manifest = normalize_manifest(raw, os.path.dirname(os.path.abspath(path)))
    manifest["path"] = os.path.abspath(path)
    manifest["sha256"] = sha256_bytes(payload)
    return manifest


def expand_argv(template, n, repeat, variant):
    values = {"n": str(n), "repeat": str(repeat), "variant": variant}
    out = []
    for token in template:
        for key, value in values.items():
            token = token.replace("{" + key + "}", value)
        out.append(token)
    return out


def build_env(manifest_env, env_unset):
    env = dict(os.environ)
    for key in env_unset:
        env.pop(key, None)
    env.update(manifest_env)
    return env


# --------------------------------------------------------------------------
# output parsing
# --------------------------------------------------------------------------
def _normalize_key(key):
    key = key.strip()
    if key.endswith("_ms"):
        return key, "ms"
    if key.endswith("_mb"):
        return key, "mb"
    if key in BARE_MS_KEYS:
        # bench/wave.erl and bench/wave-go.go emit `spawn=12ms total=25ms`.
        return key + "_ms", "ms"
    return key, None


def _parse_value(text):
    """Parse a metric value, tolerating the legacy ``12ms`` suffix.

    Returns ``(value, resolution)`` where resolution is the smallest step the
    emitted text can represent: 1 for an integer literal such as ``spawn=1ms``,
    or 10**-decimals for ``spawn_ms=1.437``. The value itself keeps every digit
    the harness printed; nothing is rounded or int-truncated here.
    """
    text = text.strip()
    if not text:
        return None, None
    if text.endswith("ms") or text.endswith("MS"):
        text = text[:-2]
    try:
        number = float(text)
    except ValueError:
        return None, None
    lowered = text.lower()
    if "." in text and "e" not in lowered:
        decimals = len(text.split(".", 1)[1])
        resolution = 10.0 ** -decimals
        return number, resolution
    if "e" in lowered:
        # Scientific notation: the mantissa decides the step.
        mantissa = lowered.split("e", 1)[0]
        decimals = len(mantissa.split(".", 1)[1]) if "." in mantissa else 0
        return number, 10.0 ** -decimals
    return int(number), 1.0


def _json_precision(value):
    """The representable step of a JSON number: ints are unit-grid, floats decimal."""
    if isinstance(value, int):
        return 1.0
    text = repr(value)  # shortest round-tripping decimal for a float
    return 10.0 ** -(len(text.split(".", 1)[1])) if "." in text else 1.0


def _json_metrics(line):
    """Metrics from a JSON object record, or None when the line is not one.

    A harness may emit ``{"procs": 1000, "spawn_ms": 1.437, "wave_ms": 109.2}``
    instead of the key=value line. That form carries real decimals, so it is
    accepted and its values keep full precision.
    """
    stripped = line.strip()
    if not stripped.startswith("{") or not stripped.endswith("}"):
        return None
    try:
        payload = json.loads(stripped)
    except json.JSONDecodeError:
        return None
    if not isinstance(payload, dict):
        return None
    found = {}
    for key, value in payload.items():
        if isinstance(key, str) and isinstance(value, (int, float)) and not isinstance(value, bool):
            found[key] = (value, _json_precision(value))
    return found or None


def _record(store, key, value, step, printed):
    """Fold one parsed record into the metric store, tracking printed grid."""
    # Widen the step first: a disagreeing record still proves the output cannot
    # resolve finer than its own printed grid, and a conflict must not hide it.
    if step is not None:
        if key not in store["resolution"] or step > store["resolution"][key]:
            # Keep the coarsest step seen: the weakest record bounds what an
            # aggregate over these samples can honestly claim.
            store["resolution"][key] = step
    if key in store["metrics"] and store["metrics"][key] != value:
        store["conflicts"].append(
            {"key": key, "first": store["metrics"][key], "second": value}
        )
        return
    store["metrics"][key] = value
    store["text"][key] = printed


def _parse_line(line, store):
    """Parse one output line: either a JSON object record or key=value tokens."""
    as_json = _json_metrics(line)
    if as_json is not None:
        for key, (value, step) in as_json.items():
            key, _unit = _normalize_key(key)
            _record(store, key, value, step, repr(value))
        return
    for token in line.replace("\t", " ").split():
        if "=" not in token:
            continue
        key, _, raw_value = token.partition("=")
        key, unit = _normalize_key(key)
        value, step = _parse_value(raw_value)
        if value is None:
            continue
        printed = raw_value.strip()
        if printed.endswith("ms") or printed.endswith("MS"):
            printed = printed[:-2]
        # A count has no unit grid, so it claims no resolution.
        _record(store, key, value, step if unit is not None else None, printed)


def parse_output(text):
    """Parse every metric a wave harness emitted, with its printed precision.

    Handles all three shapes used in this repo:

    * ``procs=N result=N spawn=12ms wave=13ms total=25ms`` -- Go
      (``bench/cross-runtime/wave.go``, ``bench/wave-go.go``) and Erlang
      (``wave.erl``), which print **integer milliseconds**.
    * ``procs=N result=N`` plus ``spawn_ms=.. wave_ms=.. total_ms=..`` on
      another stream -- Janet, whose ``eprintf`` line carries the timings.
    * a JSON object line, for any harness that emits real decimals.

    Keys are canonicalised so ``spawn`` and ``spawn_ms`` are one metric.

    Returns a dict with ``metrics`` (numbers), ``conflicts`` (keys whose value
    differed between records), ``text`` (the exact printed token per key) and
    ``resolution`` (the representable step per key). A key whose resolution is
    1 ms was quantised by the harness before the runner saw it -- that is a
    floor, not a measurement, and downstream statistics must say so.
    """
    store = {"metrics": {}, "conflicts": [], "text": {}, "resolution": {}}
    for line in text.splitlines():
        _parse_line(line, store)
    return store


def trial_failures(metrics, conflicts, n, outcome):
    """Classify one trial's outcome. ``outcome`` carries exit/signal/timeout."""
    failures = []
    if outcome["timed_out"]:
        # We killed it; the signal is our own kill, so it is not a second cause.
        return [FAILURE_TIMEOUT]
    if outcome["term_signal"] is not None:
        failures.append(FAILURE_SIGNAL)
    if outcome["exit_status"] is not None and outcome["exit_status"] != 0:
        failures.append(FAILURE_NONZERO)
    if "result" not in metrics:
        failures.append(FAILURE_MISSING_RESULT)
    elif metrics["result"] != n:
        failures.append(FAILURE_RESULT_MISMATCH)
    if "procs" in metrics and metrics["procs"] != n:
        failures.append(FAILURE_PROCS_MISMATCH)
    for conflict in conflicts:
        if conflict["key"] in ("result", "procs"):
            failures.append(FAILURE_METRIC_CONFLICT)
            break
    return failures


# --------------------------------------------------------------------------
# trial execution
# --------------------------------------------------------------------------
def _kill_group(pid):
    """Kill the child's own process group. Returns None or a reason string.

    ESRCH (already exited) is expected and is not an error. Anything else is
    reported to the caller so a failed kill is never silently swallowed.
    """
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        return None
    except OSError as exc:
        return f"killpg({pid}) failed: {type(exc).__name__}: {exc}"
    return None


def _start_sampler(trial_dir, pid, seconds):
    """Attach macOS ``sample`` to the exact child pid; never fatal."""
    if sys.platform != "darwin":
        return None, "sample: not macOS"
    binary = shutil.which("sample")
    if not binary:
        return None, "sample: binary not found"
    log_path = os.path.join(trial_dir, "sample.txt")
    argv = [binary, "-file", log_path, str(pid), str(int(seconds))]
    try:
        proc = subprocess.Popen(
            argv,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
    except OSError as exc:
        return None, f"sample: failed to start: {exc}"
    return proc, None


def _finish_sampler(sampler, timeout_s):
    """Wait (bounded) for the sampler and report how it ended, errors included."""
    if sampler is None:
        return None
    try:
        _, stderr = sampler.communicate(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        kill_error = _kill_group(sampler.pid)
        try:
            sampler.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            return {
                "returncode": None,
                "error": "sample did not exit even after SIGKILL to its group",
            }
        return {
            "returncode": None,
            "error": "sample exceeded its bounded run time; killed"
            + (f" ({kill_error})" if kill_error else ""),
        }
    return {"returncode": sampler.returncode, "error": (stderr or "").strip() or None}


def executable_digest(argv, cwd):
    if not argv:
        return {"path": None, "sha256": None, "error": "empty argv"}
    candidate = argv[0]
    if os.sep not in candidate and not os.path.isabs(candidate):
        resolved = shutil.which(candidate) or candidate
    else:
        resolved = candidate if os.path.isabs(candidate) else os.path.join(cwd, candidate)
    resolved = os.path.abspath(resolved)
    if not os.path.isfile(resolved):
        return {"path": resolved, "sha256": None, "error": "not a regular file"}
    digest = sha256_file(resolved)
    return {
        "path": resolved,
        "sha256": digest,
        "error": None if digest else "unreadable",
    }


def _execute_with_sample(argv, cwd, env, variant, trial_dir):
    """Execute one trial, optionally attaching ``sample`` to the exact child."""
    started = time.monotonic()
    try:
        proc = subprocess.Popen(
            argv,
            cwd=cwd,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
            start_new_session=True,
        )
    except OSError as exc:
        return {
            "proc": None,
            "stdout": "",
            "stderr": "",
            "wall_s": time.monotonic() - started,
            "timed_out": False,
            "kill_error": None,
            "spawn_error": f"{type(exc).__name__}: {exc}",
            "sample_note": None,
            "sample_info": None,
        }

    sampler = None
    sample_note = None
    if variant["sample"]:
        sampler, sample_note = _start_sampler(
            trial_dir, proc.pid, variant["sample_seconds"]
        )

    timed_out = False
    kill_error = None
    try:
        stdout, stderr = proc.communicate(timeout=variant["timeout_s"])
    except subprocess.TimeoutExpired:
        timed_out = True
        kill_error = _kill_group(proc.pid)
        stdout, stderr = proc.communicate()

    sample_info = None
    if sampler is not None:
        sample_info = _finish_sampler(sampler, variant["sample_seconds"] + 30)

    return {
        "proc": proc,
        "stdout": stdout or "",
        "stderr": stderr or "",
        "wall_s": time.monotonic() - started,
        "timed_out": timed_out,
        "kill_error": kill_error,
        "spawn_error": None,
        "sample_note": sample_note,
        "sample_info": sample_info,
    }


def _write_logs(trial_dir, stdout, stderr):
    stdout_path = os.path.join(trial_dir, "stdout.log")
    stderr_path = os.path.join(trial_dir, "stderr.log")
    with open(stdout_path, "w", encoding="utf-8") as handle:
        handle.write(stdout)
    with open(stderr_path, "w", encoding="utf-8") as handle:
        handle.write(stderr)
    return stdout_path, stderr_path


def _outcome_fields(outcome):
    """Split a raw process result into the exact fields the record needs."""
    returncode = None if outcome["proc"] is None else outcome["proc"].returncode
    return {
        "exit_status": returncode if returncode is not None and returncode >= 0 else None,
        "term_signal": -returncode if returncode is not None and returncode < 0 else None,
    }


def _sample_record(variant, trial_dir, outcome):
    return {
        "enabled": variant["sample"],
        "seconds": variant["sample_seconds"] if variant["sample"] else None,
        "note": outcome["sample_note"],
        "result": outcome["sample_info"],
        "log_path": (
            os.path.join(trial_dir, "sample.txt") if outcome["sample_info"] else None
        ),
    }


def _classify(outcome, metrics, conflicts, n):
    """Decide the failure list for one trial from its measured outcome."""
    if outcome["spawn_error"]:
        # The program never ran, so correctness checks are meaningless; report
        # exactly one cause rather than a cascade of missing_result noise.
        return [FAILURE_SPAWN_ERROR]
    fields = _outcome_fields(outcome)
    return trial_failures(
        metrics,
        conflicts,
        n,
        {
            "exit_status": fields["exit_status"],
            "term_signal": fields["term_signal"],
            "timed_out": outcome["timed_out"],
        },
    )


def run_trial(variant, n, repeat, output_dir, executables):
    """Execute one trial and write its evidence. Returns the trial record."""
    argv = expand_argv(variant["argv"], n, repeat, variant["name"])
    cwd = variant["cwd"]
    trial_dir = os.path.join(output_dir, variant["name"], f"n{n}", f"rep{repeat}")
    os.makedirs(trial_dir, exist_ok=True)

    digest = executable_digest(argv, cwd)
    executables.setdefault(digest["path"], digest["sha256"])

    started_at = utc_now()
    outcome = _execute_with_sample(
        argv, cwd, build_env(variant["env"], variant["env_unset"]), variant, trial_dir
    )
    stdout, stderr = outcome["stdout"], outcome["stderr"]
    stdout_path, stderr_path = _write_logs(trial_dir, stdout, stderr)

    parsed = parse_output(stdout + "\n" + stderr)
    metrics, conflicts = parsed["metrics"], parsed["conflicts"]
    failures = _classify(outcome, metrics, conflicts, n)
    fields = _outcome_fields(outcome)

    record = {
        "schema": SCHEMA,
        "variant": variant["name"],
        "n": n,
        "repeat": repeat,
        "argv": argv,
        "cwd": cwd,
        "env_overrides": variant["env"],
        "env_unset": variant["env_unset"],
        "started_at": started_at,
        "wall_s": round(outcome["wall_s"], 6),
        "exit_status": fields["exit_status"],
        "term_signal": fields["term_signal"],
        "term_signal_name": (
            signal_name(fields["term_signal"]) if fields["term_signal"] else None
        ),
        "timed_out": outcome["timed_out"],
        "timeout_s": variant["timeout_s"],
        "stdout_path": stdout_path,
        "stderr_path": stderr_path,
        "stdout_sha256": sha256_bytes(stdout.encode("utf-8")),
        "stderr_sha256": sha256_bytes(stderr.encode("utf-8")),
        "metrics": metrics,
        # Exactly what the harness printed, and the step its text can express.
        # resolution_ms == 1 means the harness quantised before reporting.
        "metric_text": parsed["text"],
        "metric_resolution": parsed["resolution"],
        "metric_conflicts": conflicts,
        "failures": failures,
        "ok": not failures,
        "spawn_error": outcome["spawn_error"],
        "kill_error": outcome["kill_error"],
        "executable": digest,
        "sample": _sample_record(variant, trial_dir, outcome),
        "trial_dir": trial_dir,
    }

    _write_json(os.path.join(trial_dir, "trial.json"), record)
    return record


# --------------------------------------------------------------------------
# summary
# --------------------------------------------------------------------------
def _stats(values):
    if not values:
        return None
    return {
        "count": len(values),
        "min": min(values),
        "max": max(values),
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else None,
    }


def summarize(trials):
    groups = {}
    for trial in trials:
        key = (trial["variant"], trial["n"])
        groups.setdefault(key, []).append(trial)

    summary = {"schema": SCHEMA, "ok": all(t["ok"] for t in trials), "groups": []}
    for (variant, n), rows in sorted(groups.items()):
        metric_names = sorted({name for row in rows for name in row["metrics"]})
        metrics = {
            name: _stats([row["metrics"][name] for row in rows if name in row["metrics"]])
            for name in metric_names
        }
        resolution = _group_resolution(rows, metric_names)
        summary["groups"].append(
            {
                "variant": variant,
                "n": n,
                "trials": len(rows),
                "ok_count": sum(1 for row in rows if row["ok"]),
                "failed_count": sum(1 for row in rows if not row["ok"]),
                "ok": all(row["ok"] for row in rows),
                "failures": sorted({k for row in rows for k in row["failures"]}),
                "wall_s": _stats([row["wall_s"] for row in rows]),
                "metrics": metrics,
                "metric_resolution": resolution,
                "precision_notes": _precision_notes(resolution),
                "samples": [
                    {
                        "repeat": row["repeat"],
                        "ok": row["ok"],
                        "wall_s": row["wall_s"],
                        "exit_status": row["exit_status"],
                        "term_signal": row["term_signal"],
                        "timed_out": row["timed_out"],
                        "metrics": row["metrics"],
                        "metric_text": row.get("metric_text", {}),
                        "failures": row["failures"],
                        "trial_dir": row["trial_dir"],
                    }
                    for row in sorted(rows, key=lambda row: row["repeat"])
                ],
            }
        )
    return summary


def _group_resolution(rows, metric_names):
    """Coarsest printed step per metric: the weakest record bounds the group."""
    out = {}
    for name in metric_names:
        steps = [
            row["metric_resolution"][name]
            for row in rows
            if name in row.get("metric_resolution", {})
            and row["metric_resolution"][name] is not None
        ]
        if steps:
            out[name] = max(steps)
    return out


def _precision_notes(resolution):
    """Spell out which metrics the harness quantised before reporting."""
    notes = []
    for name, step in sorted(resolution.items()):
        if step >= 1.0:
            notes.append(
                f"{name} is printed as whole {int(step)} ms by the harness; "
                "values are quantised at the source and stdev/median are "
                "resolved to that grid, not to a finer measurement."
            )
    return notes


def total_trials(manifest, only=None):
    count = 0
    for variant in manifest["variants"]:
        if only and variant["name"] not in only:
            continue
        count += len(variant["sizes"]) * variant["repeats"]
    return count


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def _run_meta(manifest, variants, out_dir):
    expected = sum(len(v["sizes"]) * v["repeats"] for v in variants)
    return {
        "schema": SCHEMA,
        "started_at": utc_now(),
        "finished_at": None,
        "manifest_path": manifest["path"],
        "manifest_sha256": manifest["sha256"],
        "manifest": manifest["raw"],
        "repo": manifest["repo"],
        "git": git_info(manifest["repo"]),
        "machine": machine_info(),
        "output_dir": out_dir,
        "selected_variants": [v["name"] for v in variants],
        "expected_trials": expected,
        # Filled in as trials start; one entry per distinct executable.
        "executables": {},
        "notes": [
            "Sequential trials; argv executed literally without a shell.",
            "Children run in their own session; only a timed-out child's "
            "process group is SIGKILLed.",
            "A trial fails on timeout, signal, nonzero exit, missing result, "
            "result != N, procs != N, or conflicting result/procs records.",
            "sample is macOS-only, opt-in, attached to the exact child pid, "
            "and diagnostic only.",
            "Wall-clock profiling is intentionally not integrated into trials.",
        ],
    }


def _write_json(path, payload):
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")


def _execute_variant(variant, out_dir, state):
    """Run every (size, repeat) of one variant sequentially."""
    trials = []
    for n in variant["sizes"]:
        for repeat in range(1, variant["repeats"] + 1):
            state["index"] += 1
            state["log"](
                f"[{state['index']}/{state['expected']}] "
                f"{variant['name']} n={n} rep={repeat}"
            )
            trial = run_trial(variant, n, repeat, out_dir, state["executables"])
            trials.append(trial)
            # Append-only: a crash mid-run still leaves every finished trial.
            state["journal"].write(json.dumps(trial, sort_keys=True) + "\n")
            state["journal"].flush()
            os.fsync(state["journal"].fileno())
            verdict = "ok" if trial["ok"] else "FAIL:" + ",".join(trial["failures"])
            state["log"](
                f"    {verdict} wall={trial['wall_s']:.3f}s "
                f"result={trial['metrics'].get('result')}"
                f" procs={trial['metrics'].get('procs')}"
            )
    return trials


def run(manifest, only=None, output_dir=None, log=None):
    log = log or (lambda message: print(message, file=sys.stderr))
    out_dir = os.path.abspath(output_dir) if output_dir else manifest["output_dir"]
    os.makedirs(out_dir, exist_ok=True)

    variants = [
        variant
        for variant in manifest["variants"]
        if not only or variant["name"] in only
    ]
    if not variants:
        raise ManifestError("no variants selected")

    run_meta = _run_meta(manifest, variants, out_dir)
    run_meta_path = os.path.join(out_dir, "run.json")
    _write_json(run_meta_path, run_meta)

    trials = []
    # Rewound, not appended: a re-run must not look like extra samples of the
    # previous one.
    with open(os.path.join(out_dir, "trials.jsonl"), "w", encoding="utf-8") as journal:
        state = {
            "index": 0,
            "expected": run_meta["expected_trials"],
            "log": log,
            "journal": journal,
            "executables": run_meta["executables"],
        }
        for variant in variants:
            trials.extend(_execute_variant(variant, out_dir, state))

    summary = summarize(trials)
    summary["started_at"] = run_meta["started_at"]
    summary["manifest_sha256"] = manifest["sha256"]
    summary["output_dir"] = out_dir
    summary["trials"] = len(trials)
    _write_json(os.path.join(out_dir, "summary.json"), summary)

    run_meta["finished_at"] = utc_now()
    run_meta["trials"] = len(trials)
    run_meta["ok"] = summary["ok"]
    _write_json(run_meta_path, run_meta)

    return summary, out_dir


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run controlled Phase 4 wave trials from a JSON manifest.",
    )
    parser.add_argument("manifest", nargs="?", help="path to the manifest JSON")
    parser.add_argument(
        "--output-dir",
        default=None,
        help="override the manifest's output_dir",
    )
    parser.add_argument(
        "--only",
        action="append",
        default=None,
        metavar="VARIANT",
        help="run only this variant (repeatable)",
    )
    parser.add_argument(
        "--print-example-manifest",
        action="store_true",
        help="print an example manifest and exit",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="suppress per-trial progress on stderr",
    )
    args = parser.parse_args(argv)

    if args.print_example_manifest:
        print(json.dumps(EXAMPLE_MANIFEST, indent=2))
        return 0

    if not args.manifest:
        parser.error("a manifest path is required")

    try:
        manifest = load_manifest(args.manifest)
        log = (lambda message: None) if args.quiet else None
        summary, out_dir = run(
            manifest, only=args.only, output_dir=args.output_dir, log=log
        )
    except ManifestError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(
        f"{summary['trials']} trials in {out_dir}: "
        + ("all passed" if summary["ok"] else "FAILURES PRESENT")
    )
    for group in summary["groups"]:
        wall = group["wall_s"]
        median = "n/a" if wall is None else f"{wall['median']:.3f}s"
        verdict = "ok" if group["ok"] else "FAIL(" + ",".join(group["failures"]) + ")"
        print(
            f"  {group['variant']:<24} n={group['n']:<9} "
            f"{group['ok_count']}/{group['trials']} ok  "
            f"median_wall={median}  {verdict}"
        )
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
