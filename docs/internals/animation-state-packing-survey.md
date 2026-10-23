# Animation/event state packing survey (ttfx Janet port)

Date: 2026-10-23
Status: **read-only scout survey.** No source was modified. This document
proposes work; it does not report work done.

Predecessor: `docs/internals/ttfx-pgo-runtime-feedback.md`, items 8 and 9 of
"Proposed runtime-feedback integration":

> 8. **Pack animation state behind accessors.** Effects access `:cur-col`,
>    `:cur-row`, `:cur-sym`, `:cur-fg`, `:active-path`, `:active`, `:layer`,
>    `:paths`, and `:scenes` directly. Migrating those to accessors and packed
>    arrays is the next large table/GC reduction.
> 9. **Native frame/color serializer.** `ctx-frame-string` remains hot.

--------------------------------------------------------------------------------

## 0. Corrections to the premise, up front

Three things in the handoff do not match the trees on disk. They change the
plan, so they come first.

### 0.1 The port is not in this repository

`janet-port/` does not exist under `/Users/pankajdoharey/Development/Projects/janet`.
The ttfx port lives in a sibling project:

```
/Users/pankajdoharey/Development/Projects/ML/ttfx/          # Rust reference (src/), ports
/Users/pankajdoharey/Development/Projects/ML/ttfx/janet-port/src/*.janet
```

Every `janet-port/...` path below is relative to `.../Projects/ML/ttfx/`. This
document lives in the compiler repo because the compiler changes (kernels,
executors, hints) live here; the application changes live there.

### 0.2 The all-37 parity+timing harness referenced in the handoff does not exist

`ttfx-pgo-runtime-feedback.md` line 266 says:

```sh
python3 bench-cli-packed-path-vs-rust.py
```

No file of that name, and no packed-vs-Rust comparison script of any name,
exists anywhere under `Projects/ML/ttfx`, `Projects/janet`, `/tmp`, or
`$HOME` (searched by name and by content). It was an uncommitted ad-hoc
script. The committed harness, `bench/validate-languages.py`, covers **five**
effects, not 37:

```python
# ML/ttfx/bench/validate-languages.py:17
EFFECTS = ("wipe", "highlight", "expand", "randomsequence", "scattered")
```

**Consequence: the gate that validated the three accepted packing stages at
37/37 is not reproducible today.** Reconstructing it is a prerequisite for
this work, not an optional extra. Section 4.3 specifies it.

### 0.3 Native kernels are not hand-written C

The task framing asks for "a validated native C serializer ... registered as a
native kernel". That is not how this toolchain works. A kernel is an ordinary
Janet `defn` carrying `:hint` metadata; `tools/callgraph.janet` type-checks it
and *emits* the C. There is no registry of hand-authored C kernel functions for
application code to join.

The working template is in the port itself, `janet-port/src/rng-kernels.janet:13`:

```janet
(defn k-next-word! {:hint {:params [:array :array :array :array :array :array :array :array]
                           :returns :array}} [r s0 s1 s2 s3 t1 t2 t3]
  ...)
```

and in `janet/test/fixtures/hinted-export/module.janet`:

```janet
(defn hot {:hint {:params [:long] :returns :long}} [x] (+ x 1))
```

So "write a C serializer" really means **one of two different projects**, and
they have very different costs. Section 5.4 separates them — and concludes
that the cheap one is **blocked**: an emitted kernel cannot construct a string
(`mkstr` → `:unknown` → refuse, `callgraph.janet:2201-2202`), so it cannot
return a frame. There are two native tiers, neither of which accepts
hand-authored C from application code; see §5.4 and §5.7.

--------------------------------------------------------------------------------

## 1. Field inventory

### 1.1 The record being packed

`janet-port/src/canvas.janet:44` — the only constructor. One mutable table per
character, 21 keys:

```janet
(defn charanim-new [id col row sym]
  @{:id id :input-col col :input-row row :input-sym sym
    :visible false :scenes @{} :active nil
    :cur-sym sym :cur-fg nil :cur-col col :cur-row row
    :paths @{} :active-path nil :is-fill false :layer 0
    :links @[] :north nil :east nil :south nil :west nil
    :uses-preexisting-colors true})
```

Instantiated twice: once per input glyph (`canvas.janet:111`) and once per
*unoccupied canvas cell* as a fill (`canvas.janet:194`). On an 80x24 canvas the
fill arena dominates — roughly 1900 records for a ~100-glyph logo. The arena
`(cv :anims)` is never compacted; ids are stable arena indices, deliberately
mirroring Rust's `CharId` slots (`canvas.janet:378-379`). `ctx-new` aliases it:
`:chars (canvas :anims)` (`scene.janet:217`).

There is **no `:cur-bg`**. The handoff lists it speculatively; the port has
foreground only, and `ctx-format-cell`/`ctx-frame-string` emit `C/color-fg`
plus `C/COLOR-RESET` with no background escape. Do not budget a bg lane.

### 1.2 Raw occurrence counts across `janet-port/src/**.janet`

| Field | Occurrences | `put` writes | Verdict |
|---|---:|---:|---|
| `:input-col` | 128 | 1 (anchor) | setup-heavy, read in a few hot loops |
| `:input-row` | 119 | 1 (anchor) | same |
| `:active` (charanim + ctx, disambiguated) | 47 | 36 | **hot** |
| `:input-sym` | 85 | 0 | setup / frame construction |
| `:visible` | 39 | 6 | **hot (render)** |
| `:cur-col` | 28 | 2 | **hot** |
| `:cur-row` | 28 | 2 | **hot** |
| `:paths` | 25 | 8 | mixed |
| `:active-path` | 23 | 8 | **hot** |
| `:id` | 18 | 0 | **hot (render tiebreak)** |
| `:cur-sym` | 17 | 6 | **hot** |
| `:layer` | 17 | 5 | **hot (render)** |
| `:cur-fg` | 14 | 6 | **hot** |
| `:scenes` | 10 | 5 | setup + 1 hot read |
| `:is-fill` | 8 | 1 | setup / per-build filters |
| `:links` | 8 | 2 | setup only |
| `:uses-preexisting-colors` | 4 | 2 | setup only |
| `:north` `:east` `:south` `:west` | 3 each | 4 | setup only |

`:active` needs care when grepping: `:active\b` also matches `:active-path`
and `:active-scratch` because `-` is a non-word character. Disambiguated
counts: `:active-path` 23, `:active-scratch` 2, bare `:active` 47 (a mix of
charanim scene-name and the ctx active-id array).

### 1.3 Hot sites, by field, with file:line

"Hot" = executes per tick per active character, or per cell per frame.

#### `:cur-col` / `:cur-row` — position

Writers (both hot, both in `scene.janet`):
- `ctx-motion-move` `scene.janet:447-448` — per active char per tick, the
  single hottest write pair in the engine.
- `ctx-motion-set-coordinate` `scene.janet:360-361` — called by effects.

Hot readers:
- `ctx-frame-string` `scene.janet:704-705` — **per character per frame**, over
  the whole arena including fills.
- `ctx-frame-cells` `scene.janet:760-761` — same, GUI backend.
- `ctx-motion-activate-path` `scene.janet:312-313,319` — origin segment.
- `effects/matrix.janet:65,67` — per column per tick.
- `effects/thunderstorm.janet:76,123-124,225,245-246`.
- `effects/orbittingvolley.janet:36,46,64,84`.
- `effects/unstable.janet:21-22,48-49,68-69` — inside `filter` over `(ctx :active)`.
- `effects/bubbles.janet:46,65`, `effects/spotlights.janet:67,83`,
  `effects/binarypath.janet:49-50`, `effects/overflow.janet:17,32`,
  `effects/print.janet:43`, `effects/rings.janet:90-91`.

#### `:cur-sym` / `:cur-fg` — appearance

Writers (all hot, all `scene.janet`): `ctx-activate-scene:262-263`,
`ctx-set-appearance:271-272`, `ctx-step-synced:548-549,572-573`,
`ctx-step-animation:593-594,601-602`.

Hot readers: `ctx-frame-string:736,745,747` (**per visible cell per frame**),
`ctx-frame-cells:764-767`, `effects/matrix.janet:49,92,101-102,125-129`,
`effects/thunderstorm.janet:162`, `effects/laseretch.janet:62`.

#### `:active` (charanim: the active scene *name*, a string, or nil)

Hot read/write in `ctx-step-animation` `scene.janet:578,597-598`,
`ctx-step-synced:551-552,573`, `ctx-char-active?:616-617` — all three run per
active char per tick. Also `ctx-activate-scene:261`,
`ctx-deactivate-scene:275`, `ctx-fire-event:347-349`.

Setup/teardown writers: `particles.janet:33,48`, `effects/laseretch.janet:17,43`,
`effects/thunderstorm.janet:65,111`. Readers: `effects/beams.janet:35-36`,
`effects/blackhole.janet:116`, `effects/unstable.janet:70`.

#### `:active-path` (the active path *name*, a string, or nil)

Hot: `ctx-motion-move:442` (read, first thing every tick per char),
`ctx-char-active?:615`, `ctx-step-synced:543`.
Writers: `ctx-motion-activate-path:326`, `ctx-motion-deactivate-path:336-339`,
plus **direct nil-clears that bypass the accessor**:
`particles.janet:32,47`, `effects/laseretch.janet:18,44`,
`effects/thunderstorm.janet:64,113`.

Those direct clears are load-bearing. `scene.janet:322-325` documents it:

```janet
  # :path-live counts characters with a non-nil :active-path; only this
  # function turns nil -> non-nil (direct clears elsewhere may leave the
  # counter high, which is safe: it only disables an optimization).
```

Any packing must preserve that exact asymmetry, including the harmless
over-count. "Fixing" the counter changes which ticks call `ctx-motion-move`
and is a parity hazard.

#### `:layer`, `:id`, `:visible` — render arbitration

`ctx-frame-string:710-711` reads `:layer` and `:id` on **both** the incoming
char and the incumbent cell occupant, per character per frame:

```janet
(when (or (= nil prev) (> (a :layer) (prev :layer))
          (and (= (a :layer) (prev :layer)) (> (a :id) (prev :id))))
  (put row idx a))
```

`:visible` is read at `ctx-frame-string:703` and `ctx-frame-cells:759` per
character per frame; written only by `ctx-set-visible:266` and
`effects/laseretch.janet:179`.

`:layer` writers are all setup-ish except `ctx-fire-event:348` (event action
`"layer"`) and `ctx-motion-activate-path:330-331` (path-carried layer):
`effects/binarypath.janet:192`, `bubbles.janet:145`, `burn.janet:24`,
`laseretch.janet:163,178`, `orbittingvolley.janet:168`, `synthgrid.janet:52`,
`thunderstorm.janet:14,21,109`.

#### `:scenes` / `:paths` — per-character sub-tables

`:scenes` is a name→packed-scene table. One hot read:
`ctx-step-animation:579` and `ctx-char-active?:617` do
`(get (a :scenes) (a :active))` — a string-keyed table probe per active char
per tick. This is the `janet_dict_find` traffic visible in release samples.
Everything else is setup (`ctx-new-scene*:235-248`, `ctx-scene:251`).

`:paths` is a name→packed-path table. Hot read: `ctx-motion-move:444`
`(get (a :paths) pname)`. Setup/structural elsewhere:
`ctx-motion-new-path:368-372`, `ctx-motion-add-waypoint:377`,
`ctx-motion-retarget-path:394`, plus effect-side reads
`effects/vhstape.janet:76,79,81,88,167`, `unstable.janet:46,66`,
`orbittingvolley.janet:78`, `rings.janet:65`.
Wholesale resets: `particles.janet:34`, `laseretch.janet:19`,
`thunderstorm.janet:63,112`, `print.janet:103`.

#### Setup-only, safe to leave boxed

`:input-col`, `:input-row`, `:input-sym`, `:is-fill`,
`:uses-preexisting-colors`, `:links`, `:north`, `:east`, `:south`, `:west`.
The neighbour fields are written once in `canvas-build-neighbor-map:204-216`
and read only by `spanning.janet:21` during spanning-tree construction.
`:links` is read/written only in `spanning.janet:10-15,32,61,144`.

### 1.4 What *blocks* packing — the reflection audit

This is the good news. Searched the whole port for `keys`, `pairs`, `freeze`,
`table/clone`, `marshal`, `struct`, and whole-table printing applied to a
charanim. **Zero hits.** Every match of `keys`/`pairs` is on gradient or
effect-state tables (`effects/rain.janet:21`, `spray.janet:89`,
`waves.janet:71`, `binarypath.janet:204,208`, `pour.janet:100`,
`bouncyballs.janet:20`, `randomsequence.janet:61`, `matrix.janet:293`,
`spanning.janet:55`, `effects.janet:84`).

Nothing iterates a charanim's key set, serializes one, or looks a field up
through a computed keyword. Field access is 100% static. Packing is therefore
a mechanical rewrite, not a semantics negotiation.

Two further structural facts that help:

- Only **two** sites iterate the whole arena: `ctx-frame-string:702` and
  `ctx-frame-cells:758`. Everything else goes through `ctx-anim`/`(ctx :chars)`
  by id. `ctx-anim` call sites: `scene.janet` 22, `thunderstorm` 14, `slice`
  13, `print` 13, `orbittingvolley` 11, `matrix` 10, `spanning` 9, then a long
  tail of 1–8.
- No effect stashes a charanim *table reference* in its own state. Effects
  store **ids** and re-resolve. (Verified by grepping
  `array/push ... ctx-anim` and `put ... (ctx-anim`.) So replacing the record
  representation does not create dangling aliases.

### 1.5 Three name collisions that will bite a careless `sed`

1. **`:visible` on matrix columns.** `effects/matrix.janet:14,26,48,53-55,57,63,71-73,76,89,98-99,112,116,163,170,199,203,213` —
   that is a matrix *column* record whose `:visible` is an **array of char ids**,
   unrelated to charanim `:visible` (a boolean). Matrix has 35 raw hits on the
   hot-field regex and most are this.
2. **`:paths` on rings' own char records.** `effects/rings.janet:201` builds
   `@{:id id :coords rotated :paths paths ...}` — an *array* of path names on a
   private record, read at `rings.janet:65,102` with `index-of`. Not a charanim.
3. **`:active-*` on effect state.** `effects/matrix.janet:144,148,154,169-171,178,180,187,263`
   use `:active-columns`; `effects/swarm.janet:26,41,67,176` use `:active-area`;
   `scene.janet:219` uses `:active-scratch`. None are charanim fields.

### 1.6 Where the time actually goes

From `docs/internals/ttfx-kernel-plan.md:53-70`, a full 1398-frame
instrumented run and the Rust comparison:

| Phase | Rust | Janet | Ratio | Janet share |
|---|---|---|---:|---:|
| Motion stepping | 32.3us | 1007us | ~31x | 72.8% |
| Scene animation | 8.2us | 164us | ~20x | 11.9% |
| Frame render | 4.1us | 211us | ~53x | 15.3% |
| **Total** | **44.6us** | **1383us** | **~31x** | |

Animation-state packing is not a 12%-of-runtime project. The charanim field
traffic is spread across **all three** phases: motion writes `:cur-col`/
`:cur-row` and reads `:active-path`/`:paths`; animation reads/writes
`:active`/`:scenes`/`:cur-sym`/`:cur-fg`; render reads `:visible`/`:cur-row`/
`:cur-col`/`:layer`/`:id`/`:cur-fg`/`:cur-sym` once per arena slot per frame.
It attacks the common factor, which is why it is worth doing — and also why
its win will be diffuse and modest rather than concentrated.

--------------------------------------------------------------------------------

## 2. Current data shapes and how they interact with already-packed state

### 2.1 What is already packed

`janet-port/src/scene.janet` establishes the house style: a flat `@[...]` with
module-level integer index constants and a `*-new` constructor.

```janet
(def frame-sym 0) (def frame-fg 1) (def frame-duration 2) (def frame-ticks 3)
(defn frame-new [sym fg duration] @[sym fg duration 0])          ; :23-40

(def scene-name 0) (def scene-ease 1) (def scene-sync 2) (def scene-looping 3)
(def scene-all 4)  (def scene-queue 5) (def scene-head 6) (def scene-map 7)
(def scene-total 8) (def scene-estep 9)
(defn scene-new [name &opt ease-name]
  @[name ease-name nil false @[] @[] 0 @[] 0 0])                 ; :28-43

(def path-name 0)   (def path-speed 1)     (def path-ease 2)    (def path-hold 3)
(def path-loop 4)   (def path-layer 5)     (def path-waypoints 6)
(def path-segments 7) (def path-total 8)   (def path-step 9)
(def path-max-steps 10) (def path-hold-left 11) (def path-last-dist 12)
(def path-origin 13) (def path-origin-wp 14)
(defn motionpath-new [name speed ease hold loop-flag &opt layer]
  @[name speed ease hold loop-flag layer @[] @[] 0.0 0 0 hold 0.0 nil nil])  ; :170-186
```

Access is positional application: `(f frame-sym)`, `(put sc scene-head ...)`.
There is no accessor-function layer for frames/scenes, and only four thin ones
for paths (`ctx-path-waypoints`, `ctx-path-set-speed`, `ctx-path-reset-hold`,
`ctx-path-set-hold`, `scene.janet:187-194`). **Precedent: positional access at
the use site, named constants, no getter functions.** Follow it; adding a
getter-function layer would reintroduce the call overhead packing exists to
remove, and `callgraph.janet` has no native-to-native continuation
(`ttfx-pgo-runtime-feedback.md:140-142`), so every accessor call is a VM exit.

### 2.2 What is still boxed

- **charanim**: a 21-key mutable table, one per arena slot (~2000 on 80x24).
- **`:scenes`**: `@{}` string→packed-scene, per character.
- **`:paths`**: `@{}` string→packed-path, per character.
- **ctx `:events`**: `@{}` id→array of `[kind caller atype avalue]` tuples
  (`scene.janet:297-303`).
- **ctx itself**: a 13-key table (`scene.janet:216-220`), touched a handful of
  times per tick — not worth packing.

### 2.3 Lifetimes

| Structure | Created | Mutated | Destroyed |
|---|---|---|---|
| charanim | canvas build (`canvas.janet:111,194`) or `ctx-add-character:227` | every tick | never (arena) |
| `:scenes` entries | effect build | per tick (frame ticks, head) | `scene-reset`; wholesale `@{}` in thunderstorm/laseretch |
| `:paths` entries | effect build + `ctx-motion-new-path` | per tick | wholesale `@{}` resets |
| `:events` entry | `ctx-register-event` | append-only | `ctx-clear-char-events:537` |
| `ctx :frame-rows` | first render | reused in place | never |
| `ctx :frame-buf` | first render | `buffer/clear` per frame | never |

The arena-never-shrinks property is the key enabler: a packed charanim can be
a set of **parallel arrays indexed by id**, with no free list, no compaction,
and no id remapping. `ctx-add-character` appends, so the arrays grow
monotonically — exactly like `(cv :anims)` does today.

### 2.4 The interaction seam

Packed scenes and paths are *values stored in boxed per-character tables*. The
remaining cost is not inside them — it is reaching them:

```janet
; scene.janet:577-580, per active char per tick
(def a ((ctx :chars) id))                 ; array index  — cheap
(when (= nil (a :active)) (break nil))    ; table probe  #1
(def sc (get (a :scenes) (a :active)))    ; table probe  #2 + #3 (string key)
```

Three hashed lookups to reach a structure that is itself already flat. That is
the specific waste this project removes. Same story in `ctx-motion-move:441-445`:
`(a :active-path)` then `(get (a :paths) pname)`.

--------------------------------------------------------------------------------

## 3. Packing proposal

### 3.1 Design constraints derived from Sections 1–2

1. Ids are dense, stable, append-only ⇒ **parallel arrays indexed by id (SoA)**,
   not an array-of-packed-records (AoS). SoA also matches what the compiler's
   profiler already looks for (`src/core/vm.c:874-877`: "Evidence for SoA-kernel
   shaping").
2. Field access is fully static ⇒ mechanical rewrite is safe.
3. No accessor-call layer (no native-to-native continuation).
4. Scene/path *names* are strings with semantic content — `effects/swarm.janet:35-41`
   reads `(a :active-path)` and does
   `(scan-number (string/slice (st :active-area) 0 1))`, parsing a numeric
   prefix out of the path name. Names cannot become bare integers without
   keeping the string somewhere.
5. The `:path-live` over-count asymmetry must survive verbatim.

### 3.2 Proposed layout

Add to `janet-port/src/scene.janet` (or a new `src/anim.janet` imported by
both `scene.janet` and `canvas.janet`), in the established style:

```janet
# Packed per-character animation state: parallel arrays indexed by char id.
# Lane naming mirrors frame-*/scene-*/path-*: `anim-<field>` is the LANE,
# `(anim-cur-col ...)` is never a function -- lanes are indexed directly.
(def anim-cur-col   0)   # number        hot: motion write, render read
(def anim-cur-row   1)   # number        hot
(def anim-cur-sym   2)   # string        hot
(def anim-cur-fg    3)   # color|nil     hot
(def anim-layer     4)   # number        hot: render arbitration
(def anim-visible   5)   # boolean       hot: render gate
(def anim-active    6)   # scene idx|-1  hot  (was: scene name string)
(def anim-apath     7)   # path idx|-1   hot  (was: path name string)
(def anim-nlanes    8)

# One array per lane, each `length == (length (ctx :chars))`.
(defn anim-pack-new [n]
  (def lanes (array/new anim-nlanes))
  (for i 0 anim-nlanes (array/push lanes (array/new-filled n 0)))
  lanes)
```

Stored on ctx as `(ctx :anim)`; lane `L` for char `id` is
`(((ctx :anim) L) id)`. Two indexed reads, no hashing, no barrier on the
table-header path.

**Scene and path indirection.** Replace the per-character `@{}` name tables
with per-character dense arrays plus a name sidecar, so the hot path indexes
and only the cold path stringifies:

```janet
(def anim-scenes    8)   # array of packed scenes, dense, build-order
(def anim-scene-ix  9)   # @{} name -> index   (SETUP ONLY, never hot)
(def anim-paths    10)   # array of packed paths, dense, build-order
(def anim-path-ix  11)   # @{} name -> index   (SETUP ONLY, never hot)
(def anim-nlanes   12)
```

Then the hot path becomes, for `ctx-step-animation`:

```janet
(def ax (ctx :anim))
(def ai (((ax anim-active)) id))               ; integer, -1 == none
(when (< ai 0) (break nil))
(def sc ((((ax anim-scenes)) id) ai))          ; two indexes, no hash
```

and for `ctx-motion-move`:

```janet
(def pi (((ax anim-apath)) id))
(when (< pi 0) (break nil))
(def p ((((ax anim-paths)) id) pi))
```

The scene/path **name** stays available as `(sc scene-name)` / `(p path-name)`,
which already exist (`scene.janet:29,170`). That satisfies constraint 4:
`effects/swarm.janet` reads the name off the packed path record instead of off
the charanim. Same for `ctx-fire-scene-complete`, which needs the completed
scene's name as the event caller (`scene.janet:551-553,597-599,605-606`).

**Stays boxed, deliberately:**

- `:id` — redundant once lanes are id-indexed. The render tiebreak
  `(> (a :id) (prev :id))` becomes `(> id previd)` with the id carried as the
  loop variable. One field deleted, not packed.
- `:input-col`, `:input-row`, `:input-sym`, `:is-fill`,
  `:uses-preexisting-colors`, `:links`, `:north/:east/:south/:west` — setup
  only (Section 1.3). Keep them in a slimmed boxed table, or move them to
  separate setup-time arrays as a cosmetic follow-up. No measurable win.
- **ctx `:events`** — leave alone. `ttfx-pgo-runtime-feedback.md:182` records
  this experiment already: *"event table -> array: 1.0082 (13/37 faster)"* —
  **measured 0.8% slower and rejected.** The `janet-port/build/ttfx-cli-packed-events*`
  binaries on disk are that rejected attempt. Do not redo it. The task brief
  lists event state as a packing target; the evidence says no.

### 3.3 Transitional shape (how to land this without a flag day)

Do **not** delete the charanim table in stage 1. Keep it, and make the lanes
authoritative for the hot fields only. During the migration a charanim table
retains its setup fields and loses `:cur-col :cur-row :cur-sym :cur-fg :layer
:visible :active :active-path :scenes :paths`. Because only two sites iterate
the arena and nothing aliases a charanim (Section 1.4), each field can be
moved independently and the build verified in isolation.

### 3.4 Staged migration order

Ordered by (parity risk ascending) x (expected win descending). Each stage is
one commit, one A/B, one 37/37 parity run.

| Stage | Scope | Files touched | Risk | Rationale |
|---|---|---|---|---|
| **0** | Rebuild the all-37 parity+timing harness (Section 4.3) | `ML/ttfx/bench/` | none | §0.2: nothing below is verifiable without it |
| **1** | `:cur-col`, `:cur-row` → lanes | `scene.janet`, `canvas.janet`, 12 effects | **low** | pure numbers; 2 writers, mechanical readers; biggest single hot pair |
| **2** | `:visible`, `:layer`, drop `:id` | `scene.janet`, 9 effects | **low** | render-only; `ctx-frame-string`/`ctx-frame-cells` are the only readers plus setup writers |
| **3** | `:cur-sym`, `:cur-fg` → lanes | `scene.janet`, `matrix`, `thunderstorm`, `laseretch` | **low-med** | 6 writers all in `scene.janet`; matrix compares `:cur-fg` by identity (`matrix.janet:101,126`) — preserve color value identity, do not normalize |
| **4** | `:scenes` → dense array + `:active` → index | `scene.janet`, `beams`, `blackhole`, `unstable`, `laseretch`, `thunderstorm`, `particles` | **medium** | kills hot probe #2/#3; must keep `scene-name` flowing to `ctx-fire-scene-complete`; wholesale `(put a :scenes @{})` resets (`thunderstorm.janet:110`) become array clears |
| **5** | `:paths` → dense array + `:active-path` → index | `scene.janet`, `vhstape`, `unstable`, `orbittingvolley`, `rings`, `swarm`, `print`, `particles`, `laseretch`, `thunderstorm` | **high** | see §4.1; auto-naming, swarm string parsing, `:path-live` asymmetry, wholesale resets |
| **6** | Slim the residual charanim table | `canvas.janet`, `spanning.janet` | low | cosmetic; measure before keeping |
| **7** | Serializer work — **not recommended**, see §5.4 | separate project | n/a | the cheap in-toolchain route is blocked; what remains is a C native module |

Stages 1–3 are the safe, high-value core: they cover the render loop's
per-cell reads and motion's per-tick writes without touching any naming or
lifetime semantics. **If only part of this gets done, do 0–3.**

Stage 5 is the one to be genuinely cautious about, and it is the one with the
smallest marginal win (one hashed probe per *moving* char per tick, and the
accepted packed-motion-path stage already returned only 0.9893).

--------------------------------------------------------------------------------

## 4. Parity risk map

### 4.1 Ranked risks

**R1 — `effects/swarm.janet` path-name string parsing. HIGHEST.**
`swarm.janet:31-45`:

```janet
(when (< (length (ctx :active)) (length (st :current)))
  ...
  (def active ((S/ctx-anim ctx id) :active-path))
  ...
          (= active (st :active-area))
  ...
         (scan-number (string/slice (st :active-area) 0 1)))
      (put st :active-area active)
  ...
                (S/ctx-motion-activate-path ctx other (st :active-area)))))
```

`:active-path` is read as a string, stored in effect state, string-compared,
numerically prefix-parsed, and fed back to `ctx-motion-activate-path` as a
name. Turning `:active-path` into an integer index breaks all four uses.
Mitigation: swarm reads `(p path-name)` off the packed path record.

**And a caveat about swarm's parity signal.** The brief and
`ttfx-pgo-runtime-feedback.md` both say swarm sits at the 36/37 line. That
claim is **recorded nowhere in either tree** — every committed record claims
37/37 (see §4.4). Two possibilities, with opposite consequences:
- *If the divergence is real*, swarm's Rust comparison is already failing, so a
  regression introduced here is the one **least likely to be caught**. Freeze a
  Janet-vs-Janet baseline before stage 5 and compare against that.
- *If it is a misremembering* (plausibly of `bench/languages.md:43`, where
  swarm is "not ported" for Lua/Chez), then swarm has a working Rust gate and
  should simply be held to it.

Either way swarm is the highest-risk effect here on code grounds alone, and
resolving which case applies is Stage 0 work, not something to assume.

**R2 — `ctx-motion-new-path` auto-naming depends on table cardinality.**
`scene.janet:366-371`:

```janet
(when (empty? pname)
  (var n (length ((ctx-anim ctx id) :paths)))
  (while (not (nil? (get ((ctx-anim ctx id) :paths) (string n))))
    (++ n))
  (set pname (string n)))
```

The generated name is derived from `(length :paths)` then probed upward. With
a dense array the length is the same number, but **only if no path is ever
removed**. Wholesale `(put a :paths @{})` resets (`particles.janet:34`,
`laseretch.janet:19`, `thunderstorm.janet:63,112`, `print.janet:103`) set the
count back to zero, so subsequent auto-names restart. A dense array must
reproduce that exactly — clear the array *and* the name sidecar together.
Any divergence silently renames paths, which changes event-caller matching in
`ctx-fire-event:344-345` and therefore the animation. Effects relying on
auto-naming must be identified before stage 5.

**R3 — `:path-live` counter asymmetry.** §1.3. Six effect sites clear
`:active-path` directly without decrementing `(ctx :path-live)`. The counter
only gates an optimization (`ctx-update:639`), so an over-count is benign —
but it is observable through nothing, which means an "improvement" here is
undetectable by inspection and only shows up as a frame diff. Port the
asymmetry verbatim, comment included.

**R4 — `matrix` color/symbol identity comparisons.**
`effects/matrix.janet:101` `(= ((S/ctx-anim ctx last) :cur-fg) (st :highlight-color))`
and `:125-129` compare `:cur-sym`/`:cur-fg` against neighbour values. Janet `=`
on colors depends on the color representation chosen in `color.janet`. Stage 3
must not change representation, only storage. Matrix is also the densest
effect on the hot-field regex (35 hits) and has the `:visible` name collision
(§1.5).

**R5 — `thunderstorm` wholesale state resets.** `thunderstorm.janet:63-65`
and `:109-113` reset `:paths`, `:active-path`, `:active`, `:scenes`, `:layer`
together, mid-run, for recycled spark characters. Lane writes must be
simultaneous and complete; a half-reset character is a new failure mode that
the boxed version could not express.

**R6 — `unstable` filters over `(ctx :active)` reading position and `:paths`.**
`unstable.janet:46-50,66-71` builds predicates that read
`(S/ctx-path-waypoints (get (a :paths) "explode"))` and compare `:cur-col`/
`:cur-row` against the last waypoint, inside `filter`. Touched by stages 1 and 5
simultaneously — sequence them, do not batch.

**R7 — `rings` `index-of` over its private `:paths` array.**
`rings.janet:101-102`:

```janet
(def current ((S/ctx-anim ctx (ch :id)) :active-path))
(def index (index-of current (ch :paths)))
```

The charanim's `:active-path` (a name) is looked up *by position* in rings'
own ordered array of path names. Stage 5 must keep this an
index-of-name-in-rings-array, feeding it `(p path-name)`.

**R8 — `print`/`vhstape`/`orbittingvolley`/`spotlights` nil-checks on
`:active-path`.** `print.janet:57`, `vhstape.janet:98`,
`orbittingvolley.janet:76`, `spotlights.janet:110`, `blackhole.janet:116` all
test `(nil? ... :active-path)`. These become `(< idx 0)`. Mechanical, but a
missed site is a silent truthiness bug: `0` is truthy in Janet, so a forgotten
`nil?` site that becomes index `0` flips from "no path" to "path 0". **Audit
every `nil?`/truthiness test on `:active` and `:active-path` before stage 4/5.**
This is the single most likely way to ship a wrong frame.

A sentinel of `-1` was chosen in §3.2 over `nil` precisely so that a missed
site fails loudly in arithmetic rather than quietly in a truthiness test. The
alternative — keeping `nil` as the sentinel — preserves every existing
`nil?` test unchanged and is the lower-risk option if stage 4/5 proves painful.
Recommendation: **keep `nil`** for `anim-active`/`anim-apath` unless profiling
shows the mixed number/nil lane defeats type inference (it probably will, per
`ttfx-pgo-runtime-feedback.md:152-155` — "slot mixes `keyword/unknown`"). This
is a real tension with no free answer; decide it with a measurement at stage 4,
not up front.

**R9 — effects untouched by any of this.** 19 of 37 effects have no hot
charanim field access at all (`bouncyballs`, `colorshift`, `crumble`,
`decrypt`, `errorcorrect`, `expand`, `fireworks`, `highlight`, `middleout`,
`pour`, `rain`, `randomsequence`, `scattered`, `slice`, `slide`, `smoke`,
`spray`, `waves`, `wipe`). They still need parity runs — they exercise
`scene.janet`'s hot path — but no edits.

### 4.2 Risk tiers, by effect

| Tier | Effects | Note |
|---|---|---|
| **Risky** | `swarm`, `thunderstorm`, `unstable`, `rings`, `vhstape`, `matrix`, `print` | R1–R7; convert last, one at a time |
| **Moderate** | `orbittingvolley`, `laseretch`, `spotlights`, `bubbles`, `binarypath`, `overflow`, `blackhole`, `beams`, `sweep` | positional reads + a `nil?` test or two |
| **Trivial** | the 19 in R9, plus `burn`, `synthgrid` (single `:layer` write) | no edits beyond a mechanical rename |

### 4.3 The validation harness — what exists, and what must be rebuilt

**Dump contract** (this is the stable part, and it is byte-exact).
`janet-port/src/scene.janet:823-836`, `faithful-dump`: for each frame, print
the frame's byte length, then the frame; on stderr `frames=N`. Uses the
virtual clock, so output is timing-independent. `tools/parity/dump.py:5-6`
documents the identical format for the Python reference. Rust implements it
behind `--parity-dump` (`ML/ttfx/src/cli.rs:149-150`, `src/main.rs:170,212`),
which also implies the virtual clock (`src/main.rs:149`).

**Flags.** Janet CLI: `--dump-frames` (implies virtual clock,
`bin/ttfx-cli.janet:180`), `--max-frames N` (`:944`), `--seed N` (`:110`),
`--input-file` (`:29`), `--bench` (`:932`). Rust: `--parity-dump`,
`--max-frames`, `--seed`.

**What exists:** `bench/validate-languages.py` — byte-compares complete
bounded dumps via SHA-256 (`digest()`, line 55) across engines, for 5 effects
x 3 inputs x seeds (42, 1337), with `COLUMNS=80 LINES=24` pinned and
`--max-frames 400`. Its docstring states the standard plainly: *"This
validates complete bounded frame dumps, not similar-looking screenshots. No
engine is eligible for a timing row until it passes this gate."* Note it
hardcodes the Janet binary as `janet-port/build/ttfx-cli-rel2` (line 36) and
an absolute path to the bender build (line 37).

**What must be rebuilt (Stage 0).** Generalize `validate-languages.py` into an
all-37 gate:

1. Enumerate the canonical 37 from `tools/fx/cases/*.txt` (37 files, one per
   effect, and the same set `tools/fx/speed.py:46-48` derives from
   `src/fx/effects/*.rs`). `janet-port/src/effects.janet:44-81` `REGISTRY` and
   `ttfx-cli --list` agree, but the `cases` directory is the Rust-side source
   of truth and also carries each effect's option set.
2. Keep the existing `digest()` SHA-256-of-whole-dump criterion and the pinned
   `COLUMNS=80 LINES=24`, `JANET_NO_PROFILE=1` env.
3. Parameterize the candidate binary instead of hardcoding `ttfx-cli-rel2`, so
   stage N's build can be compared against both Rust and the previous stage.
4. Record per-effect pass/fail, and carry a declared known-divergence list.
   See §4.4 — the 36/37 swarm figure is recorded **nowhere** in either tree.
5. Emit the paired A/B timing table in the same run, so parity and speed come
   from one invocation.

Per-stage procedure:

```sh
cd /Users/pankajdoharey/Development/Projects/ML/ttfx
cargo build --release
RUST=target/release/ttfx
JAN=janet-port/build/<stage-binary>
for fx in $(basename -s .txt -a tools/fx/cases/*.txt); do
  for inp in tools/parity/inputs/basic.txt tools/parity/inputs/paragraph.txt \
             ruby-port/logo.txt; do
    for seed in 42 1337; do
      COLUMNS=80 LINES=24 "$RUST" --seed $seed --parity-dump --max-frames 400 \
        "$fx" < "$inp" > /tmp/rs.dump
      COLUMNS=80 LINES=24 JANET_NO_PROFILE=1 "$JAN" --seed $seed --max-frames 400 \
        --dump-frames --input-file "$inp" "$fx" > /tmp/jn.dump
      cmp -s /tmp/rs.dump /tmp/jn.dump \
        && echo "PASS $fx $(basename $inp) $seed" \
        || echo "FAIL $fx $(basename $inp) $seed"
    done
  done
done
```

**Four non-obvious invocation traps**, each of which silently produces a
meaningless comparison rather than an error:

- **Seed defaults differ.** Janet defaults to `12345`
  (`bin/ttfx-cli.janet:110-112`); Rust's `--seed` is `Option<u64>`, unset
  (`src/cli.rs:124`). Omit `--seed` and the two sides run *different streams*
  and every effect "fails". Always pass it explicitly on both.
- **Input routing is asymmetric.** Rust `--parity-dump` reads **stdin**; the
  Janet port takes `--input-file`. And `--text` takes precedence over
  `--input-file` (`:24-28`), while *no* input at all silently falls back to a
  baked-in Omarchy logo (`:134`) instead of erroring.
- **`--bench` beats `--dump-frames`.** The `--bench` branch is checked first
  (`:932-942`) and exits 0 without serializing. A command carrying both
  produces an empty dump that `cmp` reports as a mismatch.
- **Virtual clock is implicit on both sides, by different flags.**
  `--dump-frames` enables it in Janet (`:180`); `--parity-dump` implies it in
  Rust (`src/cli.rs:162-166`). The Janet port has **no** `--virtual-clock` and
  **no** `--parity-dump` flag.

**Also available, and more than I first credited:**

- `tools/parity/run_suite.sh` + `tools/parity/cases.txt` — a real 178-case
  corpus (x2 seeds), **6 of them swarm** (`cases.txt:138-143`). But it compares
  **Rust vs the pinned Python TTE reference**, not Janet vs Rust, so it
  validates the reference rather than the port. It also needs
  `tools/parity/fetch_reference.sh` run first: `reference/tte` is **absent in
  this checkout**, so `dump.py`, `m0_dump.py` and `tty_run.py` cannot run at
  all right now.
- `tools/fx/oracle.sh <effect> full` — Rust fx engine vs original engine over
  all 37 case files; useful to confirm the *reference* is stable before
  blaming a Janet stage.
- `janet-port/bench/hot-stack.janet` — workload generator for an external
  profiler; asserts cross-replay determinism, and `--dump` emits a
  `cmp`-able length-prefixed stream.
- `bench/compare-languages.py` (timing), `bench/validate-gui-state.py` and
  `compare-gui-languages.py` (GUI path — `ctx-frame-cells` consumes the same
  fields, so stages 1–3 need a GUI parity pass too, easy to forget).

### 4.4 The swarm 36/37 claim is unrecorded — treat it as unverified

This deserves its own subsection because the task brief states it as fact.

**Every committed record in both trees claims 37/37.** Searched for `36/37`,
`known-fail`, `expected-mismatch`, skip lists, allowlists, `XFAIL`, and bare
`36`:

- `janet-port/TODO-effects.md:1` — *"37/37 done, ground-truth gate: 37/37 vs
  Rust"*; `:64` lists swarm as checked.
- `janet-port/README.md:73,87-88,133,159` — *"37/37 effects byte-identical"*.
- `bench-engines.md:88-92` — *"Janet and Rust completed 37/37"* (the only
  documented shortfall is **Node** at 31/37).
- `bench/languages-validation.log` — 30 PASS lines, zero FAIL lines.
- The only `XFAIL` mechanism in either tree is `js-port/scripts/sweep.sh:9-11`,
  for laseretch-vs-ruby, unrelated.
- `bench/languages.md:43` records swarm as *"not ported"* for Lua/Chez — that
  is an absence in those ports, not a Janet failure, and is a plausible source
  of a misremembered "36".

So the 36/37 figure appears only in the prose of
`ttfx-pgo-runtime-feedback.md` and in the brief for this survey. Either it was
observed ad hoc and never committed, or it is a conflation with the Lua/Chez
gap. **Resolve this empirically as the first act of Stage 0** — run swarm
against Rust at seeds 42/1337 before assuming a known-divergence exists.

If it does reproduce, the usable historical baseline is
`docs/benchmarks/2026-09-21.csv:31,68,105,142,179,216`, which carries swarm
output **sha256 hashes** — the only committed swarm digests available to diff
against. Otherwise freeze a pre-stage Janet dump and compare against that:

```sh
cmp /tmp/swarm-baseline-prestage.dump /tmp/cand-swarm.dump
```

**Also available:** `janet-port/bench/hot-stack.janet` (hot-stack sampling,
for attribution not parity), `bench/compare-languages.py` (timing),
`bench/validate-gui-state.py` and `compare-gui-languages.py` (GUI path —
`ctx-frame-cells` is the GUI's consumer of the same fields, so stages 1–3 need
a GUI parity pass too, easy to forget).

--------------------------------------------------------------------------------

## 5. `ctx-frame-string`: the serializer

### 5.1 Where it is

`janet-port/src/scene.janet:676-753`. Supporting state: `space-runs`
(`:663`, a lazily grown array of space strings indexed by run length),
`color-strings` (`:668`, a never-evicted `@{}` color→escape cache), and
`color-string` (`:670-674`, whose body is manually inlined at the hot call
site `:740-744`). Sibling consumer: `ctx-frame-cells:755-769` (GUI).

### 5.2 Current algorithm

Three passes per frame.

**Pass A — grid preparation (`:693-701`).** An SoA grid of dense row arrays is
cached on ctx as `:frame-rows` and reused across frames; rows are grown/popped
to the visible width and every cell is nil-ed. Cost: O(rows x width) writes
per frame regardless of occupancy.

**Pass B — scatter with layer arbitration (`:702-712`).** Iterates the whole
arena `(ctx :chars)`, skips invisible, applies `:row-offset`/`:col-offset`,
bounds-checks against `visible-{top,bottom,left,right}`, and writes the
charanim *table reference* into the cell — unless an incumbent wins. Winner
rule: higher `:layer`; tie broken by higher `:id`. Cost: O(arena) table probes
per frame, 2–6 field reads each, 4 of them on the incumbent too.

**Pass C — emission (`:715-753`).** One reused ctx-owned buffer
(`:frame-buf`), cleared per frame. Row-major, top row first, `"\n"` between
rows (not after the last). Per cell:

- empty ⇒ increment a `gap` counter;
- occupied ⇒ flush any pending gap as `(space-runs gap)`, one indexed read of
  a pre-made space string; then if `:cur-fg` is truthy emit
  *escape, symbol, `C/COLOR-RESET`*; else emit the bare symbol.

Trailing gap flushed at row end (`:750-751`). Returns `(string buf)` — one
copy, and the frame.

The color cache is keyed on the color value and never evicted; the comment at
`:666-668` records that re-encoding on eviction measured **3x worse** than
retention. The per-cell nil tests deliberately use truthiness rather than
`nil?` — `:725-728` records that the core `nil?` call costs **2.6–4x** the
inline branch at one call per cell per frame.

### 5.3 Why it is hot, quantified

From `docs/internals/native-loop-executors.md:226` and `:294`:

- the `ctx-frame-string` bytecode is **313 instructions**, including **43
  calls, 6 `put`, 6 `len`, and 3 collection allocations**;
- on decrypt it records **~154k native entries, ~980k ops, ~152k VM exits** —
  i.e. essentially one VM exit per native entry. The executor cannot stay
  native because almost every per-cell operation is an effectful call.

And the honest ceiling, from `ttfx-kernel-plan.md:69` and `:79-82`: render is
**15.3%** of Janet frame time and the **worst ratio of the three phases (53x
vs Rust's 4.1us)**. *"211us vs 4us on formatting: buffer pushes, color-escape
encoding, per-cell dispatch, space-run/gap logic in the interpreter."*

So: the worst ratio, but only ~15% of the budget. A *perfect* serializer is
bounded at ~15% end-to-end on that workload, and a realistic one well under.

### 5.4 Three routes, and why the obvious one is blocked

There are **two independent native tiers** in this toolchain, and neither is
the "register a C kernel" of the task framing.

- **Tier A — emitted native functions.** `tools/callgraph.janet` type-proves a
  whole `defn` and emits C (`emit-fn-c:2653`, `wrapper-for:4168`), registers it
  with `janet_cfuns(env, "native", regs)` (`:5650`), writes a
  `native-names.txt` manifest (`:5671`), and `tools/mkimage.janet:126-139`
  reads that manifest to **strip the Janet defn** from the image. This is the
  `:hint`/kernel tier that `rng-kernels.janet` uses.
- **Tier B — loop executors.** `tools/loop-executor.janet` plus the
  `JANET_NATIVE_EXECUTORS` regions in `src/core/vm.c`. Binds **structurally**,
  by exact bytecode comparison, never by name, and is double opt-in
  (`-DJANET_NATIVE_EXECUTORS` at build plus `JANET_NATIVE_EXECUTORS=1` at run).
  This is the tier `ctx-frame-string` already goes through today.

**Option A — a Tier-A hinted kernel. Blocked, and I had this wrong.** An
earlier draft of this section argued a kernel could take a `:buffer` and
return a `:string`, citing the ABI type map (`callgraph.janet:266,284,4247`).
That map governs *passing through* values that already exist; it does not
permit **constructing** them. Verified: `mkstr`, `mkbuf` and `mkstu` all set
their destination slot to `:unknown` —

```janet
; tools/callgraph.janet:2201-2202
(or (= op 'mkbuf) (= op 'mkstr) (= op 'mkstu) (= op 'mktab))
(do (put st (ins 1) :unknown))
; and again in the declaration walk, :2760-2761
```

— and `:unknown` refuses downstream, fail-closed. `ctx-frame-string` ends in
`(string buf)`, which *is* `mkstr`. So a Tier-A kernel **cannot return a fresh
frame string**. The fallback of writing into a caller-supplied buffer and
returning a byte count does not rescue it either, because the writes are
`buffer/push-string` — a call, not an op — and the corroborating evidence on
allocation-in-loops stands: `test/fixtures/native-executor.janet:11-13` shows
`unsupported-loop` refused precisely for calling `array/push`, and
`rng-kernels.janet:1-11` states the envelope as *"Fixed-limb int arrays,
in-place, no allocation"* with kernel-to-kernel calls refusing `:array`
returns.

**Tier A refuses loops outright, which blocks Option A a second time.**
(An earlier draft of this doc recorded this as unverified because I searched
inside `emit-fn-c:2653-3000` and found only vararg/return-shape refusals plus
a comment about declaring loop-reassigned slots. Wrong function: the veto is
one layer up, in candidate selection, `callgraph.janet:5253-5262`.)

```janet
; tools/callgraph.janet:5253-5262
; Hybrid wrappers replace the bytecode definition entirely. A C loop
; cannot yield a VM quantum and later resume its native stack/locals.
; Keep loops boxed until native continuations exist; ...
; Include self-edges, which can otherwise spin without advancing pc.
(def loops (some (fn [ins]
                  (def op (ins 0))
                  (or (and (= op 'jmp) (<= (ins 1) 0))
                      (and (get emit-cond-jumps op) (<= (ins 2) 0))))
                (or (d :bytecode) @[])))
```

Any backward jump — including self-edges — disqualifies the whole function,
with no override flag. `ctx-frame-string` is three nested `while` loops. So
Option A fails on **two** independent structural grounds: it cannot construct
the result string, and it cannot contain the loops that build it. Note also
the comment's own framing: *"Keep loops boxed **until native continuations
exist**"* — which is the same missing compiler feature §6.3 identifies as the
real prize.

**Option B — Tier B, i.e. what already happens.** `ctx-frame-string` is
*already* a loop-executor target: ~154k native entries and ~152k VM exits on
decrypt (`native-loop-executors.md:226`), and the measured release A/B of the
expanded-executor prototype was a **net loss** (geomean 0.948x, §6.1 of that
doc). Allocation and callbacks are forbidden in-region, so each per-cell
`buffer/push-string` is a boundary crossing. More Tier B is not the answer
here; that experiment has been run.

**Option C — a plain native module (or `ffi/defbind-alias`) over reshaped flat
state.** A real C function, compiled as a native module or bound through the
FFI, which the port calls like any other function. This is the **only** route
that may both allocate and return a fresh Janet string. It needs the packed
cell record of §5.5 to be worth doing, and it carries the GC/barrier review
burden that `native-loop-executors.md:301-305` is blunt about — a prior
prototype's signature-derived "leaf" shortcut was unsound because *"A C call
can allocate or callback regardless of its signature."* Putting ttfx-specific
ANSI formatting into `src/core/` of the language would be worse still; a
separate native module keeps it out of the core.

**Recommendation: do not attempt the serializer in stage 7 as originally
scoped.** Option A is blocked, Option B has already been measured as a loss,
and Option C is a genuine C-module project with a real soundness review
against a target worth at most ~15% of frame time (§5.3) — and realistically
a fraction of that. Do stages 1–3, re-measure render's share over packed
lanes, and only then decide whether Option C earns its risk. If the serializer
is pursued, the §5.5 cell record and the §5.6 byte contract are the parts of
this section that remain valid and necessary.

### 5.5 The packed cell record a kernel would need

Pass B currently stores a *table reference* per cell. A kernel needs flat
numbers. Proposal — three parallel `width*nrows` lanes, allocated once on ctx
and reused (same lifetime discipline as `:frame-rows`):

```janet
(def cell-sym  0)   # index into a per-frame symbol table (int, -1 == empty)
(def cell-fg   1)   # index into an escape-string table   (int, -1 == none)
(def cell-lay  2)   # layer, for in-place arbitration during scatter
```

Pass B writes `cell-sym`/`cell-fg`/`cell-lay` by flat offset
`(+ (* row width) col)` and arbitrates on `cell-lay` plus the carried id.
Pass C's kernel then needs, as its proven-type parameters:

| Param | Type | Meaning |
|---|---|---|
| `syms` | `:array` of int | per-cell symbol index, -1 empty |
| `fgs` | `:array` of int | per-cell escape index, -1 none |
| `symtab` | `:array` of `:string` | index → symbol bytes |
| `fgtab` | `:array` of `:string` | index → full SGR escape bytes |
| `spaces` | `:array` of `:string` | run length → that many spaces |
| `reset` | `:string` | `C/COLOR-RESET` |
| `width`, `nrows` | `:long` | geometry |
| `out` | `:buffer` | pre-sized destination |
| returns | `:string` or `:long` | the frame, or bytes written |

`symtab`/`fgtab` are exactly today's `color-strings` cache, re-shaped from a
hash to an index — and the existing never-evict policy (§5.2) carries over
unchanged, which is important because the 3x-worse eviction measurement
already settled that question.

### 5.6 The byte-compatibility contract

A replacement must reproduce these, and they are the corner cases that will
break first. Every item is read off `scene.janet:676-753`.

1. **Row order and separator.** Row-major from `visible-top` downward. `"\n"`
   *between* rows only — no leading and no trailing newline (`first-row` flag,
   `:721`).
2. **Geometry.** `width = right - visible-left + 1`,
   `nrows = top - bottom + 1`. Negative widths must yield zero columns and
   must not pop an empty row (`:700` guards this explicitly).
3. **Gap runs emit literal spaces, never cursor motion.** Out-of-window and
   unoccupied cells both become spaces. There is no `ESC[C`, no erase-to-EOL,
   no absolute positioning anywhere in this function. A "smarter" serializer
   that emits cursor movement is **not** byte-compatible, however much better
   it looks.
4. **Trailing spaces are emitted.** The pending gap is flushed at end of row
   (`:750-751`). Rows are not right-trimmed. (Contrast `layout-lines` in
   `canvas.janet:91-92`, which *does* trim — different function, do not
   generalize.)
5. **No color-run coalescing.** This is the one most likely to be "optimized"
   into a parity failure. Every colored cell emits its **own** escape, its
   symbol, and its **own** `COLOR-RESET` (`:740-746`). Two adjacent cells of
   identical color emit the escape and reset twice. The output is deliberately
   redundant, and the dump is compared by SHA-256, so coalescing identical
   adjacent colors — the obvious win — changes the bytes and fails the gate.
   **If run-coalescing is wanted, it is a change to the reference Rust engine
   first, not an optimization of the serializer.**
6. **Uncolored cells emit the bare symbol** with no escape and no reset
   (`:747`).
7. **`:cur-fg` is tested for truthiness, not non-nil** (`:737`). Any colour
   value that is falsy would take the uncolored path. Preserve the test, do
   not "correct" it to `nil?`.
8. **Layer arbitration: strictly greater wins; ties go to the higher id**
   (`:710-711`). Equal layer *and* equal id cannot occur (ids are unique).
   First-write-wins and last-write-wins are both wrong.
9. **Multi-byte symbols.** Symbols are UTF-8 character *strings* from
   `utf8-chars` (`canvas.janet:25-37`), not bytes. A kernel must treat them as
   opaque byte runs of length ≥ 1 and must not assume one byte per cell.
10. **The return is a fresh immutable string** (`(string buf)`, `:753`); the
    buffer is reused. A kernel returning the buffer itself would alias.

### 5.7 Registration, concretely

There is no "register a kernel" API call that application code invokes. Per
tier (verified):

**Tier A — emitted native functions.**

1. Annotate the Janet function: `(defn k-frame-emit {:hint {:params [...] :returns :long}} [...] ...)`.
   Handling is at `tools/callgraph.janet:678-684` (dict modifier on `defn`),
   with contract validation and malformed-hint rejection exercised by
   `test/fixtures/hinted-export/{module,malformed,direct,duplicate,name-collision,alias-collision,prefix}.janet`.
2. `callgraph.janet` type-proves it and emits C via `emit-fn-c:2653` plus a
   boxed wrapper from `wrapper-for:4168`.
3. Registration is `janet_cfuns(env, "native", regs)` (`:5650`), and the
   accepted set is written to a `native-names.txt` manifest (`:5671`).
4. `tools/mkimage.janet:126-139` reads that manifest and **strips the Janet
   defn** from the image, so the native version is the only one left. This is
   the step that makes acceptance load-bearing: a silent refusal leaves the
   boxed Janet function in place and you get no error, just no speedup.
5. Build with `JANET_NATIVE_EXECUTORS=1 bender --aot bin/ttfx-cli.janet
   --native -o build/...`, which prints `native-lib <n> kernels <path>`. Use
   `--emit-native-lib <dir>` (`callgraph.janet:72-73`) to dump the generated C
   and confirm **your** function is in it. Refusals print the offending
   signature (`params [table ? table] ret nil`, `slot mixes keyword/unknown`).
   "N kernels emitted" is not evidence yours is one of them, and
   `native-loop-executors.md:306-309` warns directly that neither "PGO
   enabled" nor a descriptor count is evidence of hot-loop coverage.

**Tier B — loop executors.** Binding is **structural**, by exact bytecode-word
comparison, not by name — so there is nothing to annotate and nothing to
register from the application side. Double opt-in: `-DJANET_NATIVE_EXECUTORS`
at build plus `JANET_NATIVE_EXECUTORS=1` at run. Dispatch lives in
`src/core/vm.c` (`JANET_NATIVE_EXECUTORS` regions at `:327, 1555, 1867-1871,
2732-2736, 3305`; poll at `janet_native_poll:375`); registration refusal is
reported by the generated image itself (`callgraph.janet:5531`,
`"native executor registration refused"`). Because binding is by bytecode
identity, **any edit to a function silently unbinds its executor** — relevant
to this whole migration: stages 1–5 will invalidate existing executor
descriptors for every function they touch, including `ctx-update`,
`ctx-motion-move`, `ctx-step-animation` and `ctx-frame-string`. Expect the
executor set to be re-derived per stage, and do not read a per-stage timing
change as purely a packing effect.

Application work edits neither tier's C. Only Option C (§5.4) involves writing
C, and it lives in a separate native module, not in `src/core/`.

--------------------------------------------------------------------------------

## 6. Effort and expected win

### 6.1 Calibration against what actually landed

From `ttfx-pgo-runtime-feedback.md:171-185`:

| Change | All-37 geomean | Median | Faster | Parity |
|---|---:|---:|---:|---|
| Packed scene frames | 0.9744 | 0.9773 | 29/37 | 37/37 |
| Packed scene state | 0.9779 | 0.9796 | 32/37 | 37/37 |
| Packed motion-path state | 0.9893 | 0.9895 | 25/37 | 37/37 |
| Combined | **~0.942** (5.8% faster) | | | |

And the rejections: event table→array **1.0082** (slower), packed RGB
color-cache key **1.0285** (slower), merged all-37 PGO **1.0128** (slower).

Two lessons. First, each accepted stage bought **1.1–2.6%**, trending *down*
as the easy structure was consumed. Second, **three of six experiments in this
family made things slower.** The base rate of a packing idea in this codebase
being a win is about 50%.

### 6.2 Estimates

| Stage | Effort | Expected geomean | Confidence |
|---|---|---|---|
| 0 — rebuild all-37 harness | 0.5–1 day | n/a (enabler) | high |
| 1 — `:cur-col`/`:cur-row` lanes | 1–2 days | 0.985–0.995 | medium |
| 2 — `:visible`/`:layer`, drop `:id` | 1 day | 0.985–0.995 | medium |
| 3 — `:cur-sym`/`:cur-fg` lanes | 1–2 days | 0.990–1.000 | medium-low |
| 4 — `:scenes` + `:active` index | 2–3 days | 0.990–1.000 | low |
| 5 — `:paths` + `:active-path` index | 3–5 days | 0.995–1.005 | low |
| 6 — slim residual table | 0.5 day | ~1.000 | low |
| **1–6 combined** | **9–15 days** | **0.97–0.99 (1–3%)** | **medium** |
| 7 — serializer (Option C native module) | 1–3 weeks + soundness review | unknown, capped ~0.85 | **very low — not recommended** |

Stage 7's estimate changed after §5.4 was corrected: the cheap in-toolchain
route (a hinted kernel) is **blocked** by `mkstr`/`mkbuf` → `:unknown`, and the
loop-executor route has already been measured as a net loss. What remains is a
C native module with a GC/barrier soundness review — a different size of
project than "add a kernel", against a target capped at render's 15.3% share.
Treat it as out of scope for this migration.

**Headline: 1–3% for the whole animation-state packing project, in line with
the prior stages and no better.** Stage 7 is bounded above by render's 15.3%
share and gated on the kernel actually being accepted by the type prover,
which the current refusal messages suggest is the hard part.

### 6.3 Honest framing

- This is **not** the big win. `ttfx-pgo-runtime-feedback.md:140-142` and
  `:209-217` both name the real one: **native-to-native continuation**, so
  `ctx-update → ctx-motion-move / ctx-step-animation / ctx-path-step` stops
  exiting to `run_vm` at every call. Motion is 72.8% of frame time and ~31x off
  Rust. Packing reduces the *cost per field access*; continuation would reduce
  the *number of VM boundary crossings*, which is the dominant term. If there
  is capacity for exactly one project, it is continuation, not this.
- Packing does make continuation more likely to succeed later, by removing the
  `table` and `keyword/unknown` slots that currently cause type-proof refusals
  (`:147-155`). That is a legitimate strategic reason to do it even at 1–3%,
  and it should be stated as the primary justification rather than the raw
  speed number.
- The CLI is ~22x slower than Rust end-to-end (`:271`). A 1–3% change does not
  alter that conclusion and should not be presented as if it might.
- **Stop rules.** Abandon a stage whose geomean is ≥ 1.000, exactly as the
  event-table and color-key experiments were abandoned. Do not land stage 5 on
  a geomean inside the noise band given its risk profile (§4.1, R1–R3).

--------------------------------------------------------------------------------

## 7. Immediate next actions

1. **Stage 0 is blocking and non-negotiable.** Rebuild the all-37
   parity+timing harness (§4.3) and commit it, with a machine-readable
   known-divergence entry for `swarm`. Nothing else in this document can be
   evaluated without it.
2. **Resolve the swarm 36/37 question empirically** (§4.4) — it is asserted in
   the brief but recorded nowhere in either tree. Run swarm against Rust at
   seeds 42/1337 before assuming a known divergence exists. Also run
   `tools/parity/fetch_reference.sh`: `reference/tte` is absent, so the Python
   reference suites cannot run at all today.
3. Freeze Janet-vs-Janet baseline dumps for `swarm` (and ideally for all 37) at
   the current HEAD, so later stages have a reference that does not depend on
   the Rust comparison.
4. Decide the `nil` vs `-1` sentinel question (§4.1 R8) **with a measurement**
   at stage 4, not by preference.
5. Execute stages 1–3. Re-evaluate 4–6 against their measured results. Expect
   loop-executor descriptors to be re-derived each stage (§5.7, Tier B binds by
   bytecode identity), so do not attribute a per-stage delta purely to packing.
6. **Drop stage 7 from this migration.** §5.4 as corrected: the hinted-kernel
   route cannot construct a string *and* cannot contain a loop, and the
   loop-executor route is already measured as a net loss. Only a separate C
   native module remains, and it should be costed on its own against a ≤15%
   target — not carried along here.
7. Separately, cost native-to-native continuation (§6.3) and compare it against
   stages 4–6 before committing to those. Note that `callgraph.janet:5253-5256`
   names this exact feature as the unblocker for its own loop veto: *"Keep
   loops boxed until native continuations exist."*

## 8. Reference index

Port (`/Users/pankajdoharey/Development/Projects/ML/ttfx/`):

- `janet-port/src/canvas.janet:44` — `charanim-new`, the 21-field record
- `janet-port/src/canvas.janet:102-112,184-198` — arena + fill construction
- `janet-port/src/scene.janet:23-43,170-194` — existing packed frame/scene/path layouts
- `janet-port/src/scene.janet:407-460` — `ctx-path-step`, `ctx-motion-move` (hot motion)
- `janet-port/src/scene.janet:575-620` — `ctx-step-animation`, `ctx-char-active?` (hot animation)
- `janet-port/src/scene.janet:628-649` — `ctx-update` (the tick)
- `janet-port/src/scene.janet:663-753` — space-run/color caches, `ctx-frame-string`
- `janet-port/src/scene.janet:755-769` — `ctx-frame-cells` (GUI consumer)
- `janet-port/src/scene.janet:823-836` — `faithful-dump`, the parity format
- `janet-port/src/rng-kernels.janet:1-13` — the working `:hint` kernel template
- `janet-port/src/effects.janet:44-81` — the 37-effect registry
- `janet-port/bin/ttfx-cli.janet:110,180,932,944` — `--seed`, `--dump-frames`, `--bench`, `--max-frames`
- `bench/validate-languages.py` — the 5-effect byte-parity gate to generalize
- `tools/parity/dump.py:5-6` — dump format spec
- `src/cli.rs:149-159`, `src/main.rs:149,170,212` — Rust `--parity-dump`

Compiler (this repo):

- `tools/callgraph.janet:72-73` — `--emit-native-lib`
- `tools/callgraph.janet:199,678-684,761-772` — `:hint` contract handling
- `tools/callgraph.janet:266,284,4247` — `:string`/`:buffer` ABI for values that
  already exist (pass-through only — **not** construction)
- `tools/callgraph.janet:2201-2202,2760-2761` — `mkstr`/`mkbuf`/`mkstu` →
  `:unknown` → downstream refusal: why a kernel cannot return a built string
- `tools/callgraph.janet:5253-5262` — Tier A's backward-jump/self-edge veto
  ("A C loop cannot yield a VM quantum"): why a kernel cannot contain the loop
- `tools/callgraph.janet:2653,4168,5650,5671` — `emit-fn-c`, `wrapper-for`,
  `janet_cfuns` registration, `native-names.txt` manifest (Tier A)
- `tools/callgraph.janet:5531` — `"native executor registration refused"` (Tier B)
- `tools/mkimage.janet:126-139` — manifest-driven stripping of the Janet defn
- `tools/callgraph.janet:389-430` — `:kernel` pread/pwrite intrinsics
- `tools/loop-executor.janet` — executor eligibility and emitted C
- `src/core/vm.c:375` — `janet_native_poll`
- `src/core/vm.c:874-877` — SoA-shaping evidence in the profiler
- `test/fixtures/native-executor.janet:11-13` — allocation disqualifies a loop
- `test/fixtures/hinted-export/` — hint acceptance/rejection cases
- `docs/internals/ttfx-kernel-plan.md:44-95` — the phase-split measurements
- `docs/internals/native-loop-executors.md:220-230,290-310` — `ctx-frame-string`
  bytecode shape, executor A/B, the unsound-shortcut warning
- `docs/internals/ttfx-pgo-runtime-feedback.md:167-228` — accepted/rejected
  experiments and items 8–9
