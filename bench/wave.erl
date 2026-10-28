-module(wave).
-export([main/1]).
build(0, Sink) -> Sink;
build(N, Sink) ->
    P = spawn(fun() -> receive V -> Sink ! V + 1 end end),
    build(N - 1, P).
main([Arg]) ->
    N = list_to_integer(Arg),
    Parent = self(),
    T0 = erlang:monotonic_time(microsecond),
    Sink = spawn(fun() -> receive V -> Parent ! {done, V} end end),
    Head = build(N, Sink),
    T1 = erlang:monotonic_time(microsecond),
    Head ! 0,
    V = receive {done, X} -> X after 600000 -> 0 end,
    T2 = erlang:monotonic_time(microsecond),
    io:format("procs=~p result=~p spawn=~pms wave=~pms total=~pms~n",
              [N, V, (T1-T0) div 1000, (T2-T1) div 1000, (T2-T0) div 1000]),
    halt(0).
