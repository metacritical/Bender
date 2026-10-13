# Fix lost-wakeup bug in Janet fork proc fan-in

1. Build (`make -j8`) and reproduce flakiness (loop /tmp/fo_cap.janet with `proc/lanes-start` added or direct-abstract variant).
2. Read src/core/ev.c: janet_loop_done/janet_loop drain ordering, janet_schedule `soon` path, pending registration (root_fiber/sched_id usage in non-root fibers), janet_ev_chanpends_deinit.
3. Identify root cause (leading hypothesis: drain/kevent ordering hole or stale root_fiber/sched_id in pending claims for proc fibers).
4. Implement fix in ev.c/proc.c/gc.c as needed.
5. Verify: 8 consecutive runs of 20k repro complete with sum 20000; `make test` 52/52; suite-proc, suite-mailbox, suite-lanes exit 0.
6. Add fan-in regression test (~4000 senders, direct-abstract) to test/suite-proc.janet.
7. Commit only changed files with `fix(ev): ...` message.