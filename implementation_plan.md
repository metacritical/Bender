# Phase 1 Completion, Dual-Target Battery, and Erlang-Caliber Parity

1. Finish the dual-target AOT battery: suite-ev (resume the funcenv hunt at the ev.c:4095 breakpoint inside cfun_ev_thread — the marsh.c:465 first-hit was the helper-import's recovered refusal), suite-marsh (7 weak-entry pins; watchpoint treatment on the census/retention path), suite-lanes (never AOT-run — compile and run it, then triage).
2. On each battery green, commit atomically; keep the interpreter battery and the six green AOT suites verified as regressions after every change.
3. Gather whole-objective evidence (all interpreter suites green, all AOT suites green, milestones landed, feature commits present), read the goal, and mark it complete.
4. Erlang-caliber parity roadmap (documented in proc-lanes.md; implementation is future work, not claimed):
   - Covered by existing phases: M4b preemption (SIGURG poll-point), Phase 2b collector swap, Phase 3 named registry, supervision primitives, M:N scheduler core.
   - NEW tasks recorded and now part of the standing plan: copy-by-default message semantics; messaging-throughput campaign (BEAM-class msg/s); resident-proc migration atop M4b; distribution design doc (nonzero nodes); hot-code-loading design doc; standing battle-hardening (soak + fuzz for the marshal/teardown lattice).
5. Later documented work only, never claimed: AOT Phase 2b collector swap; Phase 3 messaging refinements; Phase 4 native-tier steps 8-13.