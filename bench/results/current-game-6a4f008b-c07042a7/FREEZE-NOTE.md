# FREEZE NOTE — Trinecraft game source for the current-game A/B

Frozen at: `/tmp/bender-current-game-ab-evidence/game-source`
Freeze time: 2026-10-30 (session time)
Source worktree: `/Users/pankajdoharey/Development/Projects/ML/trinecraft` (read-only toward it; nothing there was modified)

## Identity

| field | value |
|---|---|
| source-hash (sha256 of SOURCE-MANIFEST.txt) | `dfc64b0eafe36b414a39b075cf578238c1b9bc408d73e751283c6716fd472709` |
| files frozen | 228 |
| frozen size | ~54 MiB |
| `git describe` at freeze | `2c68438-dirty` |
| commits in history | 552 |
| remotes | **none configured** (so this worktree is the only copy — the freeze is the reproducibility mechanism) |

`HEAD` at the moment `HEAD` was captured (during freezing) was `2c6843849077c1032e9b5ce8e0c8e9c21d4b25d2`.
The game source itself was captured before that capture step; see "Drift" below.

## Drift during the freeze (important)

The repository is **being committed to by another agent while these artifacts were produced.**
HEAD advanced `67c1238` → `72cfe2b` → `2c68438` during this session.

The frozen copy was verified against the live worktree immediately afterwards, per subtree:

| subtree | frozen vs live |
|---|---|
| `main.ben` | **IDENTICAL** |
| `run.sh` | **IDENTICAL** |
| `data/` | **IDENTICAL** |
| `src/` | **IDENTICAL** |
| `test/` | differs — `test/test_progressive_worker.py` only |
| `tools/` | differs — `tools/progressive_worker.py` only (plus live `__pycache__`, excluded) |

**Every gameplay-relevant file is byte-identical between the freeze and the live tree.**
The only two differing files are the in-progress progressive-world worker work, which:

- is **not imported by `main.ben`** — all 36 imports are `./src/*.ben` and none is progressive;
- is **not referenced by `src/*.ben`** at runtime (the sole `tools/` references are
  `tools/terrain_backend.py` and `tools/terrain_new_game.py`, neither of which imports it);
- is not in the `run.sh test` Bender suite list (that list is Bender-only).

So the freeze is safe for the A/B. If the other agent later changes `main.ben`, `src/`, or `data/`,
re-freeze; use `live-subtree-hashes.txt` to detect that cheaply.

Per-subtree hashes of the live tree at capture time are in `live-subtree-hashes.txt`.

## What was excluded, and why

Excluded from the freeze:

| path | reason |
|---|---|
| `.git/` | provenance recorded separately (HEAD/describe/log/status) |
| `build/` | AOT artifacts, 1.4 MB binary + generated C; gitignored; not used by interpreter runs |
| `tmp/` | 5.5 GB of scratch incl. `tmp/terrain-runtime/venv`; not needed for a procedural arm |
| `worlds-v1/` | 28 MB saved worlds owned by another agent; a `--frames` arm never writes here |
| `__pycache__/`, `*.pyc` | Python bytecode caches |
| `.DS_Store` | macOS noise |
| `*.trcr` | saved-world format, gitignored |
| `native/proof/fixtures/stage/*.f32`, `stage2/*.f32`, `fixtures/oracle/*` | **130 MB of regenerable raw oracle dumps**, gitignored by the repo's own rule (`.gitignore:19` — "the compressed meta.json (+ per-stage stats) is what gets committed"). Pruned to keep the freeze portable. **All tracked fixture metadata was preserved** (`stage-meta.json`, `summary.json`, `conditioning-golden.json`, `goldens/`, `conditioning-quantiles.bin`). |

Not needed for playback: `native/` is only used by `run.sh test-native-arithmetic`.

## What was deliberately INCLUDED despite being gitignored

`assets/*.png` — six PNGs, ~50 MB:

```
assets/Trinexraft_main.png                      8.9 MB   <-- main startup artwork
assets/trineraft_main_loader_sampel.png         8.9 MB
assets/Gemini_Generated_Image_17nb0617nb0617nb.png
assets/Gemini_Generated_Image_75pyju75pyju75.png
assets/Gemini_Generated_Image_hunr1nhunr1nhunr.png
assets/Gemini_Generated_Image_i9hoooi9hoooi9ho.png
```

These are gitignored (`*.png`, `.gitignore:10`) but are **decoded at startup**
(`src/loading-screen.ben:42` default main art, `:455` `rl/load-image`). Without them
`main.ben:3449-3455` catches the failure and falls back to a text loading screen — a visible
difference that would be misread as a rendering regression. A plain `git archive` would silently
drop all of them; this freeze does not.

`docs/screenshot.png` is also carried (it is the one PNG that `.gitignore` re-admits).

## Byte-exact integrity check

`SOURCE-MANIFEST.txt` holds one `sha256  ./relative/path` line per frozen file, sorted by path.
To re-verify a freeze later:

```sh
cd /tmp/bender-current-game-ab-evidence/game-source
shasum -a 256 -c /tmp/bender-current-game-ab-evidence/SOURCE-MANIFEST.txt
shasum -a 256 /tmp/bender-current-game-ab-evidence/SOURCE-MANIFEST.txt \
  | awk '{print "source-hash:", $1}'
# must still print: dfc64b0eafe36b414a39b075cf578238c1b9bc408d73e751283c6716fd472709
```

## Runtime contract the freeze must satisfy (verified against this source)

- launch **from the frozen directory** — `data/appearance.ben` and `data/materials.ben` are
  resolved relative to cwd (`src/appearance-settings.ben:37`, `src/material-recipes.ben:92`).
  `--asset-root` covers assets only, not `data/`.
- a window is mandatory: `rl/init-window` then `require-ready-window!`; there is no headless-window flag.
- `--frames N` suppresses the scripted console and skips `save-on-exit!`, so a bounded arm writes
  **nothing** into the frozen tree.
- the Python terrain worker is diffusion-only; a procedural arm never starts it.

See `RUN-AB-USAGE.md` for the exact commands.
