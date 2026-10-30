%% bench/agents_erl.erl — Erlang equivalent of bench/agents.ben, for a
%% direct, same-machine comparative measurement (not a theoretical claim).
%%
%% Same actor kernel shape: N processes, each parked in receive, T tick
%% messages sent to each, trivial arithmetic per tick, then the actor
%% exits. Single Erlang node (erl's default: one BEAM scheduler per core,
%% NOT a single OS thread, but presented to the user as "the one Erlang
%% process/runtime" the same way bench/agents.ben is "the one bender
%% process" -- a fair comparison at the application level).
%%
%% Usage: erl -noshell -run agents_erl main N T -s init stop
%%   N = actor count (default 100000)
%%   T = ticks per actor (default 4)

-module(agents_erl).
-export([main/1]).

actor(0, Acc) ->
    Acc;
actor(Remaining, Acc) ->
    receive
        Msg when is_integer(Msg) ->
            NewAcc = (Acc + 3 * Msg) rem 1000003,
            actor(Remaining - 1, NewAcc)
    end.

actor_entry(Ticks) ->
    actor(Ticks, 0).

spawn_n(N, Ticks) ->
    spawn_n(N, Ticks, 0, []).

spawn_n(N, _Ticks, N, Acc) ->
    lists:reverse(Acc);
spawn_n(N, Ticks, I, Acc) ->
    Pid = spawn(fun() -> actor_entry(Ticks) end),
    spawn_n(N, Ticks, I + 1, [Pid | Acc]).

send_round(Pids) ->
    lists:foreach(fun(P) -> P ! 1 end, Pids).

main(Args) ->
    [NStr, TStr] = case Args of
        [] -> ["100000", "4"];
        [A] -> [A, "4"];
        [A, B | _] -> [A, B]
    end,
    N = list_to_integer(NStr),
    Ticks = list_to_integer(TStr),
    io:format("agents_erl: n=~p ticks=~p~n", [N, Ticks]),
    T0 = erlang:monotonic_time(millisecond),
    Pids = spawn_n(N, Ticks),
    T1 = erlang:monotonic_time(millisecond),
    SpawnMs = T1 - T0,
    SpawnRate = case SpawnMs of 0 -> 0; _ -> (N * 1000) div SpawnMs end,
    io:format("spawn=~p ms (~p spawns/s)~n", [SpawnMs, SpawnRate]),
    T2 = erlang:monotonic_time(millisecond),
    lists:foreach(fun(_) -> send_round(Pids) end, lists:seq(1, Ticks)),
    Sent = N * Ticks,
    %% Wait for all actors to finish (they exit after Ticks messages).
    Refs = [erlang:monitor(process, P) || P <- Pids],
    lists:foreach(fun(Ref) ->
        receive
            {'DOWN', Ref, process, _, _} -> ok
        end
    end, Refs),
    T3 = erlang:monotonic_time(millisecond),
    TickMs = T3 - T2,
    MsgRate = case TickMs of 0 -> 0; _ -> (Sent * 1000) div TickMs end,
    io:format("ticks=~p in ~p ms (~p msgs/s)~n", [Sent, TickMs, MsgRate]),
    io:format("PASS~n"),
    halt().
