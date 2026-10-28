%% bench/cross-runtime/memory.erl
%%
%% How much memory does one LIVE process cost? Counterpart to memory.janet.
%%
%% Every process stays alive (blocked in receive) until the whole set is up,
%% then erlang:memory(total) is reported. Spawning processes that exit
%% immediately measures a queue, not a resident process -- that mistake
%% invalidated an earlier comparison.
%%
%% Build: erlc memory.erl
%% Run:   erl -noshell +P 8000000 +Q 10000000 -pa . -eval 'memory:main(["1000000"]),halt().'
-module(memory).
-export([main/1]).

main([Arg]) ->
    N = list_to_integer(Arg),
    Parent = self(),

    %% Baseline BEFORE spawning: erlang:memory(total) includes the VM's own
    %% footprint (~48 MiB), which would otherwise dominate the per-process
    %% figure at small N.
    Base = erlang:memory(total),
    T0 = erlang:monotonic_time(microsecond),
    Pids = [spawn(fun() ->
                      Parent ! {up, self()},
                      receive {stop, V} -> Parent ! {done, V} end
                  end) || _ <- lists:seq(1, N)],
    collect_up(N),
    T1 = erlang:monotonic_time(microsecond),

    %% erlang:memory(total) includes the VM's own baseline (~48 MiB here), so
    %% report the MARGINAL cost of the N live processes. Without this the per
    %% process figure is dominated by the runtime itself at small N.
    Mem = erlang:memory(total),
    io:format("live_procs=~p build_ms=~w mem_mb=~w base_mb=~w~n",
              [N, round((T1 - T0) / 1000),
               round(Mem / 1048576), round(Base / 1048576)]),

    %% release them so the VM can exit promptly
    [P ! {stop, 1} || P <- Pids],
    collect_done(N),
    halt(0).

collect_up(0) -> ok;
collect_up(N) -> receive {up, _} -> collect_up(N - 1) end.

collect_done(0) -> ok;
collect_done(N) -> receive {done, _} -> collect_done(N - 1) end.
