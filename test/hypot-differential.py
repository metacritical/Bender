#!/usr/bin/env python3
"""Bitwise differential test: interpreter math/hypot vs emitted libm kernel."""
import random
import re
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BENDER = ROOT / "build/bender"
SAMPLES = 4096
rng = random.Random(42)
special = [0.0, -0.0, 1e308, -1e308, 1e-308, -1e-308, 5e-324,
           -5e-324, 1e154, -1e154, 1.0, -1.0, 2.0 ** -1022]
pairs = [(a, b) for a in special for b in special]
for _ in range(SAMPLES - len(pairs)):
    a = rng.uniform(-1, 1) * (10 ** rng.randrange(-308, 309))
    b = rng.uniform(-1, 1) * (10 ** rng.randrange(-308, 309))
    pairs.append((a, b))

with tempfile.TemporaryDirectory(prefix="janet-hypot-") as temp:
    temp = Path(temp)
    source = temp / "hypot.janet"
    source.write_text('(defn h {:hint {:params [:double :double] :returns :double}} '
                      '[x y] (math/hypot x y))\n(defn main [] (h 3.0 4.0))\n')
    out = temp / "native"
    subprocess.run([str(BENDER), str(ROOT / "tools/callgraph.janet"),
                    str(source), "--emit-native", str(out)], cwd=ROOT,
                   stdout=subprocess.DEVNULL, check=True)
    emitted = (out / "native.c").read_text()
    match = re.search(r"double h\(double p0, double p1\) \{.*?\n\}", emitted, re.S)
    assert match and "return hypot((double)(p0), (double)(p1));" in match.group(), \
        "hypot was not emitted as a direct libm call"
    csource = temp / "compare.c"
    csource.write_text('#include <stdio.h>\n#include <math.h>\n' + match.group() +
                       '\nint main(void) { double x, y; '
                       'while (scanf("%la %la", &x, &y) == 2) '
                       'printf("%a\\n", h(x,y)); return 0; }\n')
    executable = temp / "compare"
    subprocess.run(["cc", "-O2", str(csource), "-lm", "-o", str(executable)],
                   check=True)
    # Use exact hexadecimal input and output: Janet and C see identical bits.
    data = "".join(f"{a.hex()} {b.hex()}\n" for a, b in pairs)
    c_out = subprocess.run([str(executable)], input=data, text=True,
                           capture_output=True, check=True).stdout.splitlines()
    oracle = temp / "oracle.janet"
    # Literal numeric constants avoid a decimal parse round-trip. Janet's
    # scan-number accepts C99 hex floats; use the same samples and %a output.
    oracle.write_text(''.join('(print (string/format "%a" (math/hypot '
                              f'{a.hex()} {b.hex()})))\n' for a, b in pairs))
    janet_out = subprocess.run([str(BENDER), str(oracle)], cwd=ROOT,
                               capture_output=True, text=True, check=True).stdout.splitlines()
    assert len(c_out) == len(janet_out) == SAMPLES
    for i, (actual, expected) in enumerate(zip(c_out, janet_out)):
        # Parse hex to bits, allowing insignificant spelling differences.
        def bits(value):
            return struct.pack('>d', float.fromhex(value))
        if bits(actual) != bits(expected):
            raise AssertionError(f"pair {i} {pairs[i]}: {actual} != {expected}")
    assert float.fromhex(c_out[special.index(1e308)*len(special)+special.index(1e308)]) < float('inf')
    print(f"hypot differential: {SAMPLES}/{SAMPLES} bit-identical; 1e308 pair finite")
