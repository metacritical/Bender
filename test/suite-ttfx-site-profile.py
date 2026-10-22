#!/usr/bin/env python3
"""Validate diagnostic VM output, not optimization PGO evidence.

Usage: python3 test/suite-ttfx-site-profile.py <diagnostic stderr> [saturation]
The fixture has hand-counted CALL and branch loops plus a 301-PC function.
Run it using both computed-goto and switch interpreter builds and compare
full stderr; a reduced JANET_SITE_COUNTER_MAX build exercises saturation.
"""
import sys
from pathlib import Path

lines = Path(sys.argv[1]).read_text().splitlines()
assert lines and lines[0].startswith("ttfx-site-profile "), lines[:2]
fields = lambda line: dict(part.split("=", 1) for part in line.split() if "=" in part)
header = fields(lines[0])
rows = {}
sites = {}
for line in lines[1:]:
    if line.startswith("site-def="):
        row = fields(line)
        rows[row["site-def"]] = row
        sites[row["site-def"]] = {}
    elif line.startswith("site="):
        site = fields(line)
        sites[site["site"]][int(site["pc"])] = site
    else:
        raise AssertionError(f"unexpected diagnostic line: {line}")

assert int(header["defs"]) == len(rows), header
by_name = {row["name"]: (row, sites[id]) for id, row in rows.items()}
assert "probe" in by_name and "branch-probe" in by_name and "leaf" in by_name

def counts(name):
    row, pc = by_name[name]
    return [int(pc.get(i, {}).get("count", 0)) for i in range(int(row["words"]))]

if len(sys.argv) > 2 and sys.argv[2] == "saturation":
    assert int(header["dropped"]) > 0, header
    row, pcs = by_name["probe"]
    assert int(row["total"]) == 2 and int(pcs[6]["calls"]) == 2
    assert all(int(row["total"]) <= 2 for row in rows.values())
else:
    assert int(header["dropped"]) == 0, header
    assert counts("probe") == [1, 1, 4, 4, 3, 3, 3, 3, 3, 1]
    assert counts("branch-probe") == [1, 1, 4, 4, 3, 3, 3, 2, 2, 1, 3, 3, 1]
    assert counts("leaf") == [3, 3]
    row, pcs = by_name["probe"]
    assert int(row["total"]) == 26 and int(pcs[6]["calls"]) == 3
    assert int(by_name["branch-probe"][0]["total"]) == 31
    for row in rows.values():
        entries = sites[row["site-def"]]
        assert sum(int(s["count"]) for s in entries.values()) == int(row["total"]), row
        assert all(int(s["calls"]) <= int(s["count"]) for s in entries.values())
    long = [(row, sites[id]) for id, row in rows.items() if row["words"] == "301"]
    assert len(long) == 1, long
    row, pcs = long[0]
    assert int(row["total"]) == 301
    assert all(int(pcs[k]["count"]) == 1 for k in (255, 256, 299, 300))
    # Allocator churn must not merge definitions with different bytecodes.
    hashes = {row["hash"] for row in rows.values() if row["words"] == "2"}
    assert len(hashes) >= 2, hashes
print("ttfx-site-profile diagnostic fixture: passed")
