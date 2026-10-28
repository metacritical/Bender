%% bench/cross-runtime/wave.erl
%%
%% Trampoline wave: build a chain of N processes. Process i receives a value,
%% adds 1, forwards to process i+1. The last process reports the final value,
%% which MUST equal N.
%%
%% A dependency chain, not parallel work: every hop waits for the previous.
%% Measures spawn cost + per-hop message latency only.
%%
%% Build: erlc wave.erl
%% Run:   erl -noshell +P 8000000 -pa . -eval 'wave:main(["1000000"]),halt().'
%% Emits one machine-readable line, mirrored by wave.janet and wave.go.
-module(wave).
-export([main/1]).

%% build(N, Sink) returns the head of a chain of N incrementing processes
%% ending at Sink.
build(0, Sink) ->
    Sink;
build(N, Sink) ->
    P = spawn(fun() -> receive V -> Sink ! V + 1 end end),
    build(N - 1, P).

main([Arg]) ->
    N = list_to_integer(Arg),
    Parent = self(),

    %% spawn phase
    T0 = erlang:monotonic_time(microsecond),
    Sink = spawn(fun() -> receive V -> Parent ! {done, V} end end),
    Head = build(N, Sink),
    T1 = erlang:monotonic_time(microsecond),

    %% wave phase
    Head ! 0,
    V = receive {done, X} -> X after 600000 -> io:format("TIMEOUT~n"), halt(1) end,
    T2 = erlang:monotonic_time(microsecond),

    io:format("procs=~p result=~p spawn_ms=~w wave_ms=~w total_ms=~w~n",
              [N, V,
               round((T1 - T0) / 1000),
               round((T2 - T1) / 1000),
               round((T2 - T0) / 1000)]),
    halt(0).
