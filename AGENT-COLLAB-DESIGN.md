# AGENT-COLLAB-DESIGN.md
## A coordination layer for LLM agents on separate worktrees — same machine or across the internet

*Derived from the 2026-10-11 bender/bender_clear_memory debugging session, where two
agents worked the same repository simultaneously and every failure mode of
uncoordinated multi-agent work was observed firsthand.*

---

## 1. Problem catalogue (observed today, as design requirements)

| # | Incident (today) | Requirement |
|---|------------------|-------------|
| P1 | The other agent's edits landed in `abstract.c`/`gc.c` **while I was editing**; my edit tool rejected writes ("file changed since read"). I nearly clobbered in-flight work. | **File-claim leases with intent.** Agents declare *what they will touch* before touching it; writes check claims. |
| P2 | I discovered the other agent only forensically — file mtimes, commit authorship. No identity, no way to ask "are you mid-rewrite?" | **Identity registry + presence.** Agents register; runs heartbeat; anyone can list who is active, on what. |
| P3 | My fix needed three rebases onto a moving HEAD; landing required hand-rolled "clean window" polling and race guards; committing risked entangling their uncommitted WIP. | **Serialized merge queue.** Landings are proposals processed one at a time with automated rebase + gated checks in scratch worktrees. |
| P4 | Their wedged processes (suite-proc parked 2–3 h; `nnstrict.ben` spinning 3 h) poisoned my timing-sensitive measurements. I killed processes I couldn't attribute with certainty. | **Run registry + resource reservation.** Every process carries a run ID; benchmark windows are reserved; orphans are attributable and killable by policy. |
| P5 | We duplicated work: both agents reproduced the same UAF independently, hours apart. I had instrumented evidence they never saw. | **Shared task board with evidence.** One task = one claim; findings attach to tasks, not to private context. |
| P6 | My handoff was filesystem graffiti (untracked `HANDOFF-*.md`) — no delivery guarantee, no ack, no lifecycle. | **Durable messages with ack.** A handoff is a typed message that stays unread-flagged until the recipient acknowledges. |
| P7 | Same repo, two worktrees, one machine — and we already hit all of this. The team will be **multi-machine, multi-human**, over the internet. | **Transport tiers:** start on git-only (works anywhere), upgrade to a small server for liveness; humans onboard their agents as first-class members. |

---

## 2. Design principles

1. **Git is the substrate.** Every agent already has it. The board starts as
   documents in a git ref — cross-machine by construction, offline-tolerant,
   auditable.
2. **Ephemeral state is short-lived; durable state is append-only.**
   Leases/presence expire by TTL. Tasks/messages/events are never silently
   overwritten; transitions are events.
3. **No silent clobber, ever.** Every mutable document carries `rev`; writes
   are CAS (`if-rev-matches`). A stale write fails loudly and the writer
   re-reads. (Today's failure: edits under my feet.)
4. **Conventions enforced by tooling, not discipline.** LLM agents follow a
   10-verb CLI reliably; they do not reliably follow a 40-page etiquette doc.
5. **Advisory first, hard when needed.** Leases and reservations are advisory
   by default (a non-compliant agent can still work); critical paths support
   hard mode (write refusal) per project policy.
6. **Nothing secret in the board.** Public keys and capability tokens only;
   secrets live in agent environments.
7. **Dead agents are normal.** Everything must tolerate agents that vanish
   mid-protocol: TTL reclaim, idempotent retries, durable redelivery.

---

## 3. Identity & naming model

All IDs are namespaced, short enough for LLM/human ergonomics, unambiguous
enough for machines.

| Entity | ID form | Example | Notes |
|--------|---------|---------|-------|
| Project | `proj:<slug>:<id8>` | `proj:bender:a3f9c2d1` | `id8` = first 8 hex of SHA-256 of the canonical remote URL → the same project is recognized from any clone on any machine. |
| Agent | `agent:<handle>:<id8>` | `agent:blueant:91f3` | Owned by a human (`owner:`). Carries an ed25519 public key. |
| Human | `hum:<handle>` | `hum:pankaj` | Humans never hold leases directly; their agents act for them. |
| Run | `run:<agent-id8>:<ulid>` | `run:91f3:01JAX8…` | One agent turn-loop. Registers with a heartbeat TTL. All spawned processes carry it as `COORD_RUN_ID`. |
| Task | `task:<proj-slug>:<n>:<slug>` | `task:bender:42:segfault-clear-memory` | `n` monotonic per project. Immutable body + mutable state. |
| Lane (worktree) | `wt:<agent-id8>:<branch>` | `wt:91f3:fix/ta-walk-hardening` | One worktree = one branch = one integration lane. |
| Lease | `lease:<proj-slug>:<id8>` | `lease:bender:77b1` | Pathspec + intent + TTL + mode. |
| Message | `msg:<ulid>` | `msg:01JAX9…` | Signed envelope, durable, acked. |
| Machine | `mach:<hostname>` | `mach:pankajs-mbp` | Capability tags + benchmark reservation target. |

**Document envelope** (every board document):
```json
{ "kind": "task", "v": 1, "proj": "proj:bender:a3f9c2d1",
  "id": "task:bender:42:segfault-clear-memory", "rev": 7,
  "updated_by": "agent:blueant:91f3", "updated_at": "…Z", "sig": "ed25519:…" }
```
Writes are CAS on `rev`. Conflicts fail loudly — never a silent clobber.

---

## 4. Board model (the documents)

Lives in a **coordination ref**: an orphan branch `agents-coord` in the project
repo (L0) or the coordd store (L1) in the same format. Never mixed into code
merges.

```
agents-coord/
  project.json      # proj id, canonical URL, policy (lease TTLs, hard-mode paths, gates)
  roster.json       # humans → agents → pubkeys → capabilities → home server
  machines.json     # machine inventory + capability tags
  tasks/<nnnn>-<slug>.json
  lanes.json        # worktree lanes: branch, base SHA, predicted/actual file manifests, status
  leases.json       # active leases (also mirrored per-machine for offline resolution)
  merge-queue.json  # ordered landing proposals + coordinator decisions
  reservations.json # machine benchmark windows
  events/NNNNNN.jsonl   # append-only event log (the news feed + replay cursor)
  messages/inbox/<agent>/…json   # durable mailboxes
```

**Task document:**
```json
{ "kind": "task", "id": "task:bender:42:segfault-clear-memory", "rev": 7,
  "title": "Shutdown segfault in bender_clear_memory",
  "intent": "Reproduce, root-cause, fix, verify",
  "state": "in-progress",
  "claimed_by": "agent:blueant:91f3", "claim_expires": "…Z",
  "deps": [], "blocks": [],
  "artifacts": [ { "kind": "evidence", "path": "notes/ta-dead-sweep-2026-10-11.log" },
                 { "kind": "branch", "ref": "fix/ta-walk-hardening" } ],
  "acceptance": [ "suite-proc 6/6 x3", "amp repro: no 0x70 fault x20" ],
  "events": [ "opened", "claimed", "evidence-attached", … ] }
```

Task state machine:
```
open → claimed → in-progress → in-review → landed
                    ↓                ↓
                 blocked ────────────↘
                 (auto-reopens)    abandoned (claim TTL expiry)
```

**Lane manifest** (predicted vs actual file touches — the anti-P1 device):
```json
{ "lane": "wt:91f3:fix/ta-walk-hardening", "task": "task:bender:42",
  "base": "78de4a75", "status": "in-progress",
  "predicted_files": ["src/core/gc.c", "src/core/abstract.c"],
  "actual_files": ["src/core/gc.c", "src/core/abstract.c"] }
```
Overlap between any two active lanes' manifests is **computed and surfaced the
moment it appears** — not discovered by an edit-tool rejection.

---

## 5. Protocols

### 5.1 Lease protocol (P1)
`coord lease take "src/core/gc.c,src/core/abstract.c" --task 42 --ttl 30m --intent "walk hardening"`
- Registry records pathspec, task, run, TTL.
- Overlap with an existing lease → `coord` refuses (soft) unless `--force`
  (recorded as an override event, surfaces in the feed).
- Policy can mark paths **hard-mode** (e.g. `src/core/gc.[ch]`): writes to a
  hard-path without a lease are refused by the project's pre-commit hook.
- TTL + heartbeat (`coord ping` piggybacked on long operations); **dead runs'
  leases expire automatically** — the zombie problem cannot hold claims.

### 5.2 Presence & runs (P2, P4)
- `coord run start --task 42` → run doc with TTL; wrapper exports `COORD_RUN_ID`;
  all child processes inherit it.
- `coord ps` = live runs + their owned processes → orphans attributable and
  killable by policy (`coord kill-run <run>` — logged, and only for stale runs
  unless forced).
- `coord reserve <mach> --for 30m --reason "amp-repro measurement"` → exclusive
  measurement window. Timing-sensitive benchmark runs **require** holding the
  reservation; others see it in the feed and defer heavy load.

### 5.3 Merge-queue protocol (P3) — the crown jewel

**Why worktrees alone don't solve it.** Worktrees isolate uncommitted file
state; they do nothing about (a) the base moving under you — commits from other
agents invalidate verification no matter how private your tree is, (b) semantic
conflict — hunks that merge cleanly but break behavior in interaction (e.g. a
new teardown hook vs. new walk guards: each green alone, red together), or
(c) duplicated intent — two agents investigating the same defect in parallel.
Worktrees relocate the pain to merge time; they don't remove it. The only
defenses are coordination: declare intent before working, and *serialize and
re-verify every landing on the merged result*.

No agent ever lands on master directly. `coord propose-merge <branch>
--base <sha> --evidence <log-paths>`:

1. Coordinator (server bot, or a designated agent in L0) appends to
   `merge-queue.json`.
2. Serially, per proposal, in a **scratch worktree**:
   - base == HEAD? else attempt automated rebase;
   - conflict with an earlier queued lane → bounce to author with a conflict
     report (path-level);
   - run gated checks (project policy: e.g. `suite-proc 6/6 ×3`);
   - pass → merge (ff or merge-commit per policy), push, mark `landed`,
     broadcast `lane-landed`, release the lane's leases;
   - fail → bounce with the check log attached to the task.
3. Emergency hotfix lane: priority-jumps the queue, same rigor.

This mechanizes everything I hand-rolled today: clean-window polling, race
guards, rebase-on-moving-HEAD, and "commit must not entangle WIP".

### 5.4 Messaging (P6)
```json
{ "kind": "msg", "id": "msg:01JAX9…",
  "from": "agent:blueant:91f3", "to": ["agent:metacritical:44ab"],
  "type": "handoff|question|answer|claim-request|conflict|status|alert",
  "refs": { "task": "task:bender:42", "lane": "wt:91f3:…" },
  "body": { … }, "ack": "pending", "sig": "…" }
```
- Durable mailbox per agent; `coord inbox` / `coord ack <msg>`.
- `handoff` type = task reassignment with evidence — the anti-graffiti device.
- Unread handoffs surface on `coord task show` — no silent drops.

### 5.5 Cross-task discovery (P5)
`coord task list --state open` and full-text search over tasks+events is the
**first thing an agent does before investigating**. Today's duplication
(independent UAF reproductions hours apart) becomes impossible: the second
agent finds `task:42 in-progress` + attached instrumented evidence and joins
instead of duplicating.

---

## 6. Transport tiers

### L0 — git-sync (day 0, zero infrastructure)
Board = the `agents-coord` orphan branch. Agents pull/push; `coord` handles
document CAS locally (re-read + retry on push rejection). Liveness via poll
interval (30–60 s). Works across the internet **today** — any git remote
(including a private one) is a coordination hub. Limitation: presence/leases
are eventually-consistent; no push notifications.

### L1 — coordd (recommended, ~1–2 days of build)
Single static binary (Go) or Node service:
- HTTPS + WSS; board stored in a bare repo **in L0 format** (L0 stays as the
  offline fallback and audit trail).
- Adds: pub/sub (`coord watch` = websocket push), lease TTL sweeper, merge
  coordinator automation, web board UI, token auth (ed25519-signed requests).
- Hosting: one always-on machine (this Mac) + **Tailscale/WireGuard** for
  NAT-free internet reach, or plain HTTPS with tokens on a cheap VPS.
- Clock discipline: server timestamps are authoritative for ordering; ULIDs
  give total order; client skew is harmless.
- Delivery: at-least-once + `msg_id` dedupe; reconnect replays events after a
  client cursor.

### L2 — GitHub-native (when the team already lives there)
Board = GitHub Projects + issues; leases = bot-enforced PR/issue labels;
merge queue = required checks + auto-merge + a merge-bot Action; messages =
review comments via a GitHub App. Slower and platform-coupled, but zero
self-hosting. Same IDs and document shapes map 1:1.

**Federation note:** every doc names its project and each roster entry names a
home server; cross-server routing (agent on team A's coordd messaging agent on
team B's) is L3 territory — same envelopes, server-to-server relay. Deferred.

---

## 7. The `coord` CLI (~10 verbs — the whole UX)

```
coord init                       # create/adopt project board (derives proj id from remote)
coord whoami / coord ping        # identity check / heartbeat (TTL refresh)
coord task new|list|show|claim|update|done|block|attach
coord lease take|release|list    # pathspec claims, TTL, --force = logged override
coord lane register|files|status # worktree lane + file manifest
coord propose-merge <branch>     # join the merge queue with evidence
coord msg send|inbox|ack         # durable messaging
coord run start|ps|kill-run      # run registry, process attribution
coord reserve <mach> --for 30m   # benchmark exclusivity window
coord watch                      # subscribe to events (long-poll / websocket)
coord roster add|list            # team onboarding
```
One static binary (or one Python file, stdlib-only, for Phase 0). Verbs are
idempotent where possible (`claim` twice = no-op returning the claim).

---

## 8. Team onboarding & multi-machine

- **Add a human:** `coord roster add hum:alice --pubkey <ed25519> --server wss://…`
  → invite token → alice's agent registers itself (`agent:alice-bot:…`), gets
  capability scope (e.g. `read-all, write-tasks, lease-soft, propose-merge`).
- **Add a machine:** `coord machine register mach:alice-lab --tags linux,arm64`
  → agents there join the same project by cloning + `coord init` (same remote
  URL ⇒ same `proj:` id).
- **Join an existing project:** clone repo → `coord init` → fetch
  `agents-coord` → roster challenge: sign a nonce with the agent key; owner
  approves in the feed/web UI. Never an unauthenticated write.

---

## 9. Security

- ed25519 keypair per agent; every board write and message signed; server
  verifies.
- Capability tokens (scoped: read / write-tasks / lease / merge-propose /
  admin); destructive ops (board reset, kill-run of a live run) require
  `admin` + are event-logged.
- No secrets on the board; tokens live in agent env (`COORD_TOKEN`).
- Non-compliant agents: soft violations are visible in the feed (reputation is
  just history); hard-mode paths + merge gates protect master regardless.

---

## 10. Rollout plan

**Phase 0 — today (0 infra, this repo):** conventions + `coord` as a single
stdlib-Python script committed to the coordination branch; roster with both
agents; open `task:bender:42` with today's evidence attached; adopt lease+lane
declarations. Both agents get `AGENTS.md` instructions: *before investigating,
`coord task list`; before editing, `coord lease take`; before landing,
`coord propose-merge`.*

**Phase 1 — this week:** `coordd` on this Mac (Go single binary), Tailscale
exposure, websocket `watch`, TTL sweeper, web board page. Second machine joins
(bender CI box or a collaborator's laptop) to prove cross-machine.

**Phase 2:** merge coordinator automation with scratch-worktree rebase + gated
checks; reservation scheduler; attribution (`coord ps` / `kill-run`).

**Phase 3:** GitHub-native mode; cross-server federation; audit export.

## 11. Incident → mechanism map (acceptance check)

| Today's incident | Mechanism that prevents it |
|---|---|
| Edit under my feet (P1) | lane manifest + lease overlap surfaced at claim time; hard-mode pre-commit hook |
| Anonymous co-agent (P2) | roster + runs + presence feed |
| Moving-HEAD rebases, clean-window races (P3) | merge queue: serialized, auto-rebase, gated, bounce-with-report |
| Poisoned benchmarks, unattributable orphans (P4) | reservations + `COORD_RUN_ID` + `coord ps/kill-run` |
| Duplicated investigation (P5) | shared task board with evidence attachments |
| Graffiti handoff (P6) | durable typed messages with ack |
| Single-machine assumptions (P7) | L0 git transport from day 0; coordd for liveness |

## 12. Risks & open questions
- **Board thrash:** chatty agents spamming the feed → per-agent rate caps;
  events batched.
- **LLM protocol drift:** the CLI's outputs are the contract — agents parse
  JSON, never prose; strict schemas per verb.
- **Coordinator is a single point of order** (not of truth — L0 remains the
  store): any agent can run the coordinator loop; leader = lowest active run ID
  holding a `coord-leader` lease.
- **Open:** retention policy for events/messages (compress monthly?);
  encryption-at-rest for private projects (board is in a private repo or
  coordd-local, so effectively private already).
