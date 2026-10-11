# Agent Collaboration Layer — design plan (multi-agent, multi-machine)

## Goal
A coordination layer so agents on separate worktrees — same machine or across
the internet — share identity, a task board, file leases, presence, messaging,
and an orderly merge queue, with human team onboarding.

## Approach
- Ground every design decision in today's observed failures (6 incidents).
- Build on git as the L0 substrate (zero infra, works via any remote),
  upgrade to a small server (coordd) for presence/leases/pubsub, keep a
  GitHub-native L2 option.
- Tiny `coord` CLI (~10 verbs) so LLM agents follow the protocol by tooling,
  not discipline.
- Signed writes (ed25519), TTL leases with heartbeats, CAS-on-rev writes
  (no silent clobber), durable acked messages, serialized merge queue with
  automated rebase+check in scratch worktrees.

## Deliverable
AGENT-COLLAB-DESIGN.md in the workspace: problem catalogue → identity model →
board schemas → protocols → transport tiers → CLI → rollout phases.

## Phases
1. Catalogue today's failures as requirements (P1–P7).
2. Design identity/IDs (proj/agent/run/task/lane/lease/msg).
3. Design board + protocols (lease, presence, merge queue, reservation, handoff).
4. Design transports L0/L1/L2 + security + onboarding.
5. Write AGENT-COLLAB-DESIGN.md, present summary, propose Phase 0 first step
   (adopt conventions + coord CLI on this repo).

## Success criteria
- Every today-incident maps to a named mechanism.
- Two agents could adopt L0 tomorrow with zero new infrastructure.
- Protocol tolerates: dead agents, clock skew, offline agents, NAT'd machines,
  non-compliant agents (advisory + hard modes).