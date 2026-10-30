#!/usr/bin/env python3
"""Keep the emitter's whole-definition opcode policy in sync with the verifier.

The emitted descriptor contains the entire definition, including instructions
that exit to the VM before execution. A single opcode missing from the verifier
allowlist silently rejects that entire descriptor at attachment time.
"""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
EMITTER = ROOT / "tools/loop-executor.ben"
VERIFIER = ROOT / "src/core/bytecode.c"
ASSEMBLER = ROOT / "src/core/asm.c"
EXCLUDED = {"sru", "sruim"}  # uint32 domain; no corresponding native guard yet.
EMITTER_ONLY = {"sl", "slim", "sr", "srim"}  # VM signed-shift parity unprovable outside defined domain.


def one(pattern, text, source):
    matches = re.findall(pattern, text, re.DOTALL)
    if len(matches) != 1:
        raise ValueError(f"{source}: expected one match for {pattern!r}, found {len(matches)}")
    return matches[0]


def emitter_opcodes(source):
    policy = one(r"\(defn- allowed-op\? \[op\]\s*(.*?)\s*\(defn- base-eligible\?", source, EMITTER)
    maps = re.findall(r"\(get\s+([\w-]+)\s+op\)", policy)
    singles = re.findall(r"\(=\s+op\s+'([\w-]+)\)", policy)
    remainder = re.sub(r"\(get\s+[\w-]+\s+op\)|\(=\s+op\s+'[\w-]+\)", "", policy)
    if not maps or re.sub(r"[\s()]|\bor\b", "", remainder):
        raise ValueError(f"{EMITTER}: unrecognized allowed-op? expression; review opcode contract")
    admitted = set(singles)
    for name in maps:
        body = one(r"\(def\s+" + re.escape(name) + r"\s+\{(.*?)\}\)", source, EMITTER)
        # These policy maps have literal symbol keys and string/boolean values.
        pairs = re.findall(r"'([\w-]+)\s+(?:true|\"[^\"]*\")", body)
        if not pairs or len(pairs) != len(re.findall(r"'([\w-]+)", body)):
            raise ValueError(f"{EMITTER}: cannot parse all keys in {name}; review opcode contract")
        admitted.update(pairs)
    return admitted


def verifier_opcodes(source, assembler):
    fn = one(r"static int native_structure_matches\(.*?\) \{(.*?)\n\}\s*\n\s*int bender_native_matches", source, VERIFIER)
    cases = one(r"switch \(n->words\[i\] & 0xFF\) \{(.*?)\bdefault:\s*return 0;", fn, VERIFIER)
    opcodes = re.findall(r"\bcase\s+(JOP_[A-Z0-9_]+)\s*:", cases)
    if not opcodes:
        raise ValueError(f"{VERIFIER}: no opcode cases in native_structure_matches")
    names = dict(re.findall(r'\{"([a-z][a-z0-9]*)",\s*(JOP_[A-Z0-9_]+)\}', assembler))
    by_opcode = {opcode: name for name, opcode in names.items()}
    if len(by_opcode) != len(names):
        raise ValueError(f"{ASSEMBLER}: duplicate opcode names in assembly table")
    unknown = set(opcodes) - by_opcode.keys()
    if unknown:
        raise ValueError(f"{ASSEMBLER}: missing mnemonic for {', '.join(sorted(unknown))}")
    return {by_opcode[opcode] for opcode in opcodes}


def main():
    try:
        emitted = emitter_opcodes(EMITTER.read_text())
        verified = verifier_opcodes(VERIFIER.read_text(), ASSEMBLER.read_text())
    except ValueError as exc:
        print(f"native opcode contract: {exc}", file=sys.stderr)
        return 1
    failures = []
    for label, difference in (
        ("unexpected emitter-only opcodes", (emitted - verified) - EMITTER_ONLY),
        ("deliberately emitter-only signed shifts missing from emitter", EMITTER_ONLY - emitted),
        ("deliberately emitter-only signed shifts admitted by verifier", EMITTER_ONLY & verified),
        ("verifier-only (emitter never admits)", verified - emitted),
        ("excluded uint32 opcodes admitted by emitter", emitted & EXCLUDED),
        ("excluded uint32 opcodes admitted by verifier", verified & EXCLUDED),
    ):
        if difference:
            failures.append(f"{label}: {', '.join(sorted(difference))}")
    if failures:
        print("native opcode contract mismatch:\n  " + "\n  ".join(failures), file=sys.stderr)
        return 1
    print(f"native opcode contract: {len(verified)}/{len(emitted)} emitter opcodes admitted; "
          "sl/slim/sr/srim deliberately emitter-only; sru/sruim excluded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
