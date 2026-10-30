#!/usr/bin/env python3
"""Durable, byte-exact evidence capture for a single child invocation.

Why this exists
---------------
Benchmark evidence has to survive review: the exact command, the directory it
ran in, the environment it was given, what it wrote, how it ended, how long it
took, and what it consumed. A shell pipeline loses most of that and can render
a failed run as an empty or misleading log. This tool records all of it in one
directory it refuses to reuse, and it does so without reshaping the child's
output on the way through.

Guarantees
----------
* ``argv`` is executed literally -- no shell, no interpolation.
* The working directory and every environment override are explicit and
  recorded. Nothing is inferred from where the tool happened to be started.
* stdout and stderr are handed to the child as already-open file descriptors,
  so bytes travel straight from the child's ``write()`` into ``stdout.log`` and
  ``stderr.log``. Nothing is buffered in this process: an arbitrarily long
  stream costs no memory, and the bytes stay byte-exact -- including sequences
  that are not valid UTF-8, which a text-mode pipe would silently replace.
* The evidence directory is created exclusively and never overwritten. A second
  capture into the same directory fails *before* the child is started.
* The recorded status is the child's own: an exit code OR a terminating signal,
  never one guessed from the other.
* Stream digests are computed by streaming the finished file, not from anything
  held in memory.
* Resource usage comes from ``wait4``'s rusage for the exact child. On request
  the same run can also be wrapped in the platform time tool (``/usr/bin/time
  -l`` on macOS, ``-v`` on GNU/Linux); that report is written to its own file,
  so it never mixes into ``stderr.log``. Wrapper mode is opt-in because the BSD
  tool appends one of its own diagnostics to the child's stderr when a child
  dies by signal -- the default mode cannot contaminate either stream.
* A capture that fails part way does not pretend to be complete. ``complete``
  is ``true`` only after the streams are digested and the record landed; a
  capture-level failure writes a record with ``complete: false`` and the error.

Output layout
-------------
``<evidence>/stdout.log``         child stdout, byte-exact
``<evidence>/stderr.log``         child stderr, byte-exact
``<evidence>/capture.json``       the record described below
``<evidence>/resource.time.txt``  platform time-tool report (opt-in only)

Record
------
::

    {
      "schema": "janet/benchmark-capture/1",
      "label": "tip-aot-loaded",
      "complete": true,
      "evidence_dir": "/abs/path/evidence",
      "command": {
        "argv": ["..."],
        "cwd": "/abs/path",
        "stdin": "devnull",
        "env_overrides": {"JANET_PROFILE": "..."},
        "executable": {"path": "...", "sha256": "...", "bytes": 123, "error": null}
      },
      "timing": {"started_at": "...Z", "finished_at": "...Z", "wall_s": 0.123},
      "result": {"kind": "exited", "exit_status": 0, "signal": null,
                 "signal_name": null, "spawn_error": null},
      "timeout": {"seconds": null, "expired": false,
                  "kill_signal": null, "kill_error": null},
      "streams": {
        "stdout": {"file": "stdout.log", "sha256": "...", "bytes": 456},
        "stderr": {"file": "stderr.log", "sha256": "...", "bytes": 0}
      },
      "resource": {"source": "wait4 rusage", "maxrss_bytes": 12345678, ...},
      "resource_detail": {"file": "resource.time.txt", "metrics": {...}}
    }

Usage
-----
::

    tools/benchmark_capture.py --evidence-dir out/run1 --cwd . \\
        --env JANET_PROFILE=1 --label tip-aot --timeout 180 \\
        -- ./build/janet run.janet

``--timeout`` is a safety deadline, not a measurement. When it fires the child's
whole process group is killed with SIGKILL and the run is recorded as expired.
``result`` still says what the wait status actually said (signaled, SIGKILL),
and ``timeout.expired`` carries the separate fact that *we* stopped it -- a
timed-out run is a successful capture of a failed run, so a driver reads
``timeout.expired`` and must not treat ``complete: true`` as a passing run. The
child gets its own session only when a timeout is requested, so runs without one
keep the previous process-group behaviour.

The process exits with the child's status (128+signal when it was terminated by
a signal) so a wrapper script cannot mistake a failed run for a finished one. A
timed-out child exits 124, the conventional timeout(1) code, deliberately
distinct from 128+9 so a driver can tell "we stopped it" from "it died". It
exits 2 only when the capture itself could not be performed.

Do not wrap this tool in ``subprocess.run(..., timeout=...)``: that kills only
this process and leaves the child tree running into the next sequential
measurement. Use ``--timeout`` here instead.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from dataclasses import dataclass, field, replace

SCHEMA = "janet/benchmark-capture/1"
CHUNK_BYTES = 1024 * 1024
# A killed child is reaped almost immediately; this only bounds a pathological
# wait so an unreapable child cannot hang the tool forever.
REAP_GRACE_S = 30.0
# The conventional timeout(1) status, distinct from 128+SIGKILL (137), so a
# driver can tell "we stopped it" from "it died".
TIMEOUT_STATUS = 124
STDOUT_NAME = "stdout.log"
STDERR_NAME = "stderr.log"
RECORD_NAME = "capture.json"
TIME_REPORT_NAME = "resource.time.txt"

# BSD time (/usr/bin/time on macOS) prints the long rusage listing with -l; GNU
# time spells the same report -v. The flag follows the platform, never a guess.
TIME_FLAGS = {"darwin": ("-l",), "linux": ("-v",)}


class CaptureError(Exception):
    """A capture problem -- bad arguments or an occupied directory.

    This is never the child's failure; a child that fails is a successful
    capture whose record says so.
    """


@dataclass(frozen=True)
class CaptureRequest:
    """Everything that defines one capture, in one value.

    Bundling these keeps the entry point to a single argument: the fields move
    together (an argv without its cwd or its environment is not evidence), and
    a caller cannot silently omit one.
    """

    argv: tuple
    cwd: str
    evidence_dir: str
    env_overrides: dict = field(default_factory=dict)
    label: str = None
    resource_tool: str = "none"
    timeout: float = None

    def resolved(self) -> "CaptureRequest":
        """A copy with an absolute cwd and the overrides pinned as a plain dict."""
        if self.timeout is not None and self.timeout <= 0:
            raise CaptureError(f"timeout must be positive, got {self.timeout!r}")
        return replace(
            self,
            argv=tuple(self.argv),
            cwd=os.path.abspath(self.cwd),
            env_overrides=dict(self.env_overrides or {}),
        )


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="microseconds")


def hash_and_count(path: str) -> dict:
    """SHA-256 and byte count of a file, read in chunks so size never matters."""
    digest = hashlib.sha256()
    size = 0
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(CHUNK_BYTES), b""):
            digest.update(chunk)
            size += len(chunk)
    return {"sha256": digest.hexdigest(), "bytes": size}


def build_env(overrides) -> dict:
    """The child's environment: this process's environment plus overrides."""
    env = dict(os.environ)
    env.update(overrides)
    return env


def parse_env_overrides(entries) -> dict:
    overrides = {}
    for entry in entries or ():
        key, separator, value = entry.partition("=")
        if not separator or not key:
            raise CaptureError(f"environment override must be KEY=VALUE, got {entry!r}")
        overrides[key] = value
    return overrides


def resolve_executable(argv, cwd, env) -> dict:
    """Identify the file the kernel will actually exec, and hash it.

    Resolution uses the child's own PATH and working directory, so a run that
    overrides PATH is hashed against the candidate that would really run.
    """
    candidate = argv[0]
    if os.path.isabs(candidate) or os.sep in candidate:
        resolved = candidate if os.path.isabs(candidate) else os.path.join(cwd, candidate)
    else:
        resolved = shutil.which(candidate, path=env.get("PATH", os.defpath)) or os.path.join(
            cwd, candidate
        )
    resolved = os.path.abspath(resolved)
    if not os.path.isfile(resolved):
        return {
            "path": resolved,
            "sha256": None,
            "bytes": None,
            "error": "not a regular file",
        }
    try:
        digest = hash_and_count(resolved)
    except OSError as exc:
        return {
            "path": resolved,
            "sha256": None,
            "bytes": None,
            "error": f"{type(exc).__name__}: {exc}",
        }
    return {
        "path": resolved,
        "sha256": digest["sha256"],
        "bytes": digest["bytes"],
        "error": None,
    }


def status_block(status) -> dict:
    """The child's real outcome, read from its wait status alone."""
    if os.WIFSIGNALED(status):
        number = os.WTERMSIG(status)
        return {
            "kind": "signaled",
            "exit_status": None,
            "signal": number,
            "signal_name": signal.Signals(number).name,
        }
    if os.WIFEXITED(status):
        return {
            "kind": "exited",
            "exit_status": os.WEXITSTATUS(status),
            "signal": None,
            "signal_name": None,
        }
    return {
        "kind": "unknown",
        "exit_status": None,
        "signal": None,
        "signal_name": None,
        "wait_status": status,
    }


def resource_block(usage) -> dict:
    """Child resource usage from wait4, normalised across platforms.

    ``ru_maxrss`` is bytes on macOS and kilobytes on Linux, so the raw value is
    kept beside a normalised ``maxrss_bytes`` instead of being reinterpreted.
    """
    kilobytes = sys.platform.startswith("linux")
    return {
        "source": "wait4 rusage",
        "maxrss_raw": usage.ru_maxrss,
        "maxrss_unit": "kilobytes" if kilobytes else "bytes",
        "maxrss_bytes": usage.ru_maxrss * (1024 if kilobytes else 1),
        "utime_s": round(usage.ru_utime, 6),
        "stime_s": round(usage.ru_stime, 6),
        "page_reclaims": usage.ru_minflt,
        "page_faults": usage.ru_majflt,
        "swaps": usage.ru_nswap,
        "block_input_ops": usage.ru_inblock,
        "block_output_ops": usage.ru_oublock,
        "messages_sent": usage.ru_msgsnd,
        "messages_received": usage.ru_msgrcv,
        "signals_received": usage.ru_nsignals,
        "voluntary_context_switches": usage.ru_nvcsw,
        "involuntary_context_switches": usage.ru_nivcsw,
    }


def prepare_evidence_dir(path) -> str:
    """Create the evidence directory exclusively; an existing one is an error."""
    evidence = os.path.abspath(path)
    try:
        os.makedirs(evidence)
    except FileExistsError:
        raise CaptureError(
            f"evidence directory already exists; refusing to overwrite: {evidence}"
        ) from None
    except OSError as exc:
        raise CaptureError(f"cannot create evidence directory {evidence}: {exc}") from exc
    return evidence


def time_tool_argv(evidence, argv):
    """Wrap argv in the platform time tool, reporting to its own file."""
    flags = TIME_FLAGS.get(sys.platform)
    if flags is None:
        raise CaptureError(
            f"no time-tool flags known for platform {sys.platform!r}; "
            "use --resource-tool none"
        )
    binary = shutil.which("time")
    if binary is None:
        raise CaptureError("no time(1) binary on PATH; use --resource-tool none")
    report = os.path.join(evidence, TIME_REPORT_NAME)
    return [binary, *flags, "-o", report, *argv], report


def parse_time_report(path) -> dict:
    """Parse the time tool's numeric rusage lines into a name -> integer map.

    The raw report stays as evidence; this map is the parsed convenience beside
    it, never a replacement for it.
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            lines = handle.read().splitlines()
    except OSError as exc:
        return {
            "file": TIME_REPORT_NAME,
            "metrics": {},
            "error": f"{type(exc).__name__}: {exc}",
        }
    metrics = {}
    for line in lines:
        parts = line.split()
        if len(parts) >= 2 and parts[0].isdigit():
            metrics[" ".join(parts[1:])] = int(parts[0])
    return {
        "file": TIME_REPORT_NAME,
        "metrics": metrics,
        "elapsed_line": lines[0].strip() if lines else None,
        "error": None,
    }


def _terminate_group(pgid) -> str:
    """SIGKILL the child's whole process group; report an unaddressable group.

    A group kill, not a pid kill: the benchmark is a native binary that may have
    spawned workers, and killing only the leader would leave them burning CPU
    into the next sequential arm. If the group cannot be addressed at all the
    leader is still killed, so a failed group kill can never leave the capture
    hanging on a live child; the shortfall is returned for the record rather
    than hidden. Returns None on success, else a reason string.
    """
    try:
        os.killpg(pgid, signal.SIGKILL)
        return None
    except ProcessLookupError:
        # The group is already gone, which is exactly the outcome we wanted.
        return None
    except OSError as exc:
        group_error = f"killpg({pgid}) failed: {type(exc).__name__}: {exc}"
    try:
        os.kill(pgid, signal.SIGKILL)
    except OSError as exc:
        return f"{group_error}; kill({pgid}) failed: {type(exc).__name__}: {exc}"
    return f"{group_error}; killed the leader only (workers may have escaped)"


def _reap_with_timeout(proc, timeout):
    """Reap the child, killing its group when the deadline passes.

    ``wait4`` runs on a thread so wall timing and rusage stay exact and the
    main thread never polls. The reaper owns the single ``wait`` for this child,
    which is why the main thread only observes the event instead of waiting too.
    """
    outcome = {}

    def reap():
        try:
            _, status, usage = os.wait4(proc.pid, 0)
            outcome["status"], outcome["usage"] = status, usage
        except ChildProcessError as exc:  # someone else reaped it
            outcome["error"] = f"{type(exc).__name__}: {exc}"
        finally:
            outcome["done"].set()

    outcome["done"] = threading.Event()
    reaper = threading.Thread(target=reap, name="capture-reaper", daemon=True)
    reaper.start()
    expired = not outcome["done"].wait(timeout)
    if expired:
        kill_error = _terminate_group(proc.pid)
        # SIGKILL is not ignorable, so the reap completes promptly; the wait is
        # bounded only so a pathological kernel state cannot hang the tool.
        outcome["done"].wait(REAP_GRACE_S)
        outcome["kill_error"] = kill_error
    return expired, outcome


def _run_child(argv, cwd, env, evidence, timeout=None) -> dict:
    """Give the child real descriptors, then reap it with wait4.

    The files are opened before the spawn so a failure to create them is a
    capture error, and both use exclusive mode so no path can overwrite
    existing evidence. A requested timeout puts the child in its own session so
    its whole tree is addressable for a group kill.
    """
    started = time.monotonic()
    with open(os.path.join(evidence, STDOUT_NAME), "xb") as out, open(
        os.path.join(evidence, STDERR_NAME), "xb"
    ) as err:
        try:
            proc = subprocess.Popen(
                argv,
                cwd=cwd,
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=out,
                stderr=err,
                close_fds=True,
                start_new_session=timeout is not None,
            )
        except OSError as exc:
            return _run_outcome(started, spawn_error=f"{type(exc).__name__}: {exc}")
        reap = _await_child(proc, timeout)
        status = reap[0]
        # Bookkeeping only: the child is already reaped, and without this Popen
        # would still report it as running.
        if status is not None:
            proc.returncode = (
                -os.WTERMSIG(status) if os.WIFSIGNALED(status) else os.WEXITSTATUS(status)
            )
    return _run_outcome(started, reap)


def _await_child(proc, timeout):
    """Reap the child, honouring the deadline when one was requested.

    Returns ``(status, usage, expired, kill_error)``. A status of None with a
    kill_error of None means the reap itself failed.
    """
    if timeout is None:
        _, status, usage = os.wait4(proc.pid, 0)
        return status, usage, False, None
    expired, outcome = _reap_with_timeout(proc, timeout)
    kill_error = outcome.get("kill_error")
    if "error" in outcome:
        return None, None, expired, f"{outcome['error']}: {kill_error or ''}".strip()
    return outcome.get("status"), outcome.get("usage"), expired, kill_error


def _run_outcome(started, reap=None, spawn_error=None) -> dict:
    """One normalized child-run result, in the shape the record builder reads.

    ``reap`` is a ``(status, usage, expired, kill_error)`` tuple from
    ``_await_child``; ``spawn_error`` covers the cases where no child ever ran.
    """
    status, usage, expired, kill_error = reap if reap is not None else (None, None, False, None)
    return {
        "spawn_error": spawn_error,
        "status": status_block(status) if status is not None else None,
        "usage": usage,
        "wall_s": time.monotonic() - started,
        "expired": expired,
        "kill_error": kill_error,
    }


def write_record(evidence, record) -> str:
    """Write capture.json exclusively and durably."""
    path = os.path.join(evidence, RECORD_NAME)
    with open(path, "x", encoding="utf-8") as handle:
        handle.write(json.dumps(record, indent=2) + "\n")
        handle.flush()
        os.fsync(handle.fileno())
    return path


def _validated(request) -> None:
    """Reject a request that cannot describe a real run."""
    if not request.argv:
        raise CaptureError("an argv is required; nothing was executed")
    if any(not isinstance(entry, str) for entry in request.argv):
        raise CaptureError("every argv entry must be a string")
    if not os.path.isdir(request.cwd):
        raise CaptureError(f"cwd is not a directory: {request.cwd}")


def _spawn_argv(evidence, request):
    """The argv actually executed, plus the time report path when wrapped."""
    if request.resource_tool == "time":
        return time_tool_argv(evidence, request.argv)
    if request.resource_tool == "none":
        return list(request.argv), None
    raise CaptureError(f"unknown resource tool {request.resource_tool!r}")


def _assemble(request, evidence, outcome) -> dict:
    """The complete record for one finished child."""
    run = outcome["run"]
    result = run["status"] or {
        "kind": "not-started",
        "exit_status": None,
        "signal": None,
        "signal_name": None,
    }
    result["spawn_error"] = run["spawn_error"]
    record = {
        "schema": SCHEMA,
        "label": request.label,
        "complete": True,
        "evidence_dir": evidence,
        "command": {
            "argv": list(request.argv),
            "cwd": request.cwd,
            "stdin": "devnull",
            "env_overrides": dict(request.env_overrides),
            "executable": outcome["executable"],
        },
        "timing": {
            "started_at": outcome["started_at"],
            "finished_at": outcome["finished_at"],
            "wall_s": round(run["wall_s"], 6),
            "wall_includes_wrapper": outcome["report_path"] is not None,
        },
        "result": result,
        "timeout": {
            "seconds": request.timeout,
            "expired": bool(run["expired"]),
            "kill_signal": "SIGKILL" if run["expired"] else None,
            "kill_error": run["kill_error"],
        },
        "streams": {
            name: {"file": filename, **hash_and_count(os.path.join(evidence, filename))}
            for name, filename in (("stdout", STDOUT_NAME), ("stderr", STDERR_NAME))
        },
        "resource": resource_block(run["usage"]) if run["usage"] is not None else None,
    }
    if outcome["report_path"] is not None:
        record["resource_detail"] = parse_time_report(outcome["report_path"])
    return record


def _failed_record(request, evidence, exc) -> dict:
    """The record for a capture that never produced a trustworthy run."""
    return {
        "schema": SCHEMA,
        "label": request.label,
        "complete": False,
        "evidence_dir": evidence,
        "error": f"{type(exc).__name__}: {exc}",
        "command": {"argv": list(request.argv), "cwd": request.cwd},
    }


def capture(request) -> dict:
    """Run one ``CaptureRequest`` and record its complete evidence set.

    Returns the record. Raises CaptureError when the capture could not be
    performed at all; a child that fails is recorded, not raised.
    """
    request = request.resolved()
    _validated(request)
    evidence = prepare_evidence_dir(request.evidence_dir)
    env = build_env(request.env_overrides)
    try:
        executable = resolve_executable(request.argv, request.cwd, env)
        spawn_argv, report_path = _spawn_argv(evidence, request)
        outcome = {"executable": executable, "report_path": report_path}
        outcome["started_at"] = utc_now()
        outcome["run"] = _run_child(
            spawn_argv, request.cwd, env, evidence, request.timeout
        )
        outcome["finished_at"] = utc_now()
        record = _assemble(request, evidence, outcome)
        write_record(evidence, record)
    except Exception as exc:
        write_record(evidence, _failed_record(request, evidence, exc))
        raise
    return record


def cli_status(record) -> int:
    """The child's status for the shell; 2 when the capture itself failed."""
    if not record.get("complete", False):
        return 2
    result = record.get("result") or {}
    if result.get("spawn_error"):
        return 2
    if (record.get("timeout") or {}).get("expired"):
        return TIMEOUT_STATUS
    if result.get("kind") == "signaled":
        return 128 + int(result["signal"])
    exit_status = result.get("exit_status")
    return exit_status if isinstance(exit_status, int) else 2


def split_at_separator(argv):
    """Split ``--`` so the child's own flags are never parsed by argparse."""
    if "--" not in argv:
        return argv, []
    index = argv.index("--")
    return argv[:index], argv[index + 1 :]


def describe(record) -> str:
    """One human line summarizing a finished capture."""
    result = record["result"]
    if (record.get("timeout") or {}).get("expired"):
        outcome = f"TIMED OUT after {record['timeout']['seconds']}s"
    elif result["kind"] == "exited":
        outcome = f"exited {result['exit_status']}"
    else:
        outcome = f"{result['kind']} {result.get('signal_name') or ''}".strip()
    return (
        f"captured {record['evidence_dir']} ({outcome}, "
        f"{record['timing']['wall_s']:.3f}s wall, "
        f"{record['streams']['stdout']['bytes']} B out, "
        f"{record['streams']['stderr']['bytes']} B err)"
    )


def main(argv=None) -> int:
    raw = list(sys.argv[1:] if argv is None else argv)
    head, child_argv = split_at_separator(raw)
    parser = argparse.ArgumentParser(
        description="Capture one command's stdout/stderr and resource evidence.",
        epilog="Pass the child command after '--': -- ./build/janet run.janet",
    )
    parser.add_argument("--evidence-dir", required=True, help="new directory for this run")
    parser.add_argument("--cwd", default=None, help="working directory for the child")
    parser.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="environment override for the child (repeatable)",
    )
    parser.add_argument("--label", default=None, help="human name for this capture")
    parser.add_argument(
        "--resource-tool",
        choices=("none", "time"),
        default="none",
        help="'time' also wraps the run in the platform time tool",
    )
    parser.add_argument("--json", action="store_true", help="print the whole record")
    parser.add_argument(
        "--timeout",
        type=float,
        default=None,
        metavar="SECONDS",
        help=(
            "kill the child's whole process group after SECONDS and exit 124; "
            "the child is run in its own session so no descendant survives"
        ),
    )
    args = parser.parse_args(head)

    if not child_argv:
        parser.error("a command is required after '--'")

    try:
        record = capture(
            CaptureRequest(
                argv=tuple(child_argv),
                cwd=args.cwd or os.getcwd(),
                evidence_dir=args.evidence_dir,
                env_overrides=parse_env_overrides(args.env),
                label=args.label,
                resource_tool=args.resource_tool,
                timeout=args.timeout,
            )
        )
    except CaptureError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(json.dumps(record, indent=2) if args.json else describe(record))
    return cli_status(record)


if __name__ == "__main__":
    sys.exit(main())
