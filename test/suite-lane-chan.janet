# Copyright (c) 2026 Calvin Rose & contributors
# MIT-licensed; see suite-ev.janet for the full text.

# Cross-lane channel safety.
#
# A plain ev/chan takes no lock and copies nothing, so it is only meaningful
# inside the VM that created it. A proc spawned onto a worker lane receives a
# MARSHALLED COPY of the closure, so its view of the channel is detached: writes
# land in a copy nobody reads. The runtime must say so loudly rather than
# silently dropping every value.

(import ./helper :prefix "" :exit true)
(start-suite)

# Channels are pinned to the lane that created them.
(def c (ev/chan 4))
(ev/give c :a)
(assert (= :a (ev/take c)) "own-lane give/take round-trips")

# A worker lane runs the thunk, but its closure is marshal-COPIED: captured
# mutable state is detached, so observation must cross lanes explicitly.
(proc/lanes-start 1)
(def ran (ev/thread-chan 8))
(proc/spawn (fn [] (ev/give ran :ran)) 8)
(ev/sleep 0.4)
(assert (= :ran (ev/take ran)) "worker lane ran the thunk (observed via thread-chan)")

# The value pushed from a foreign lane is NOT observable here: this is the
# documented hazard, and thread-chan is the supported mechanism.
(def plain (ev/chan 8))
(proc/spawn (fn [] (ev/give plain :lost)) 8)
(ev/sleep 0.4)
(assert (nil? (ev/try-take plain))
        "a plain channel written from a worker lane is detached (use ev/thread-chan)")

# thread-chan DOES cross lanes.
(def tchan (ev/thread-chan 8))
(proc/spawn (fn [] (ev/give tchan :kept)) 8)
(ev/sleep 0.5)
(assert (= :kept (ev/take tchan)) "thread-chan carries values across lanes")

(proc/lanes-stop)

(end-suite)
