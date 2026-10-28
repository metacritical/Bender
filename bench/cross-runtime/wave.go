// bench/cross-runtime/wave.go
//
// Trampoline wave: build a chain of N goroutines. Goroutine i receives a value,
// adds 1, forwards it to goroutine i+1. The last one reports the final value,
// which MUST equal N.
//
// A dependency chain, not parallel work: every hop waits for the previous, so
// GOMAXPROCS cannot help. Measures spawn cost + per-hop channel handoff only.
//
// Build: go build -o wave-go wave.go
// Run:   ./wave-go <n>
// Emits one machine-readable line, mirrored by wave.janet and wave.erl.
package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"time"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Println("usage: wave-go <n>")
		os.Exit(2)
	}
	n, err := strconv.Atoi(os.Args[1])
	if err != nil {
		fmt.Println("bad n:", os.Args[1])
		os.Exit(2)
	}

	t0 := time.Now()

	// Chain: each goroutine reads its input channel, forwards v+1 downstream.
	sink := make(chan int, 1)
	next := sink
	for i := 0; i < n; i++ {
		down := next
		up := make(chan int) // unbuffered: a true rendezvous per hop
		next = up
		go func() {
			v := <-up
			down <- v + 1
		}()
	}

	t1 := time.Now()
	next <- 0
	result := <-sink
	t2 := time.Now()

	fmt.Printf("procs=%d result=%d spawn_ms=%d wave_ms=%d total_ms=%d\n",
		n, result,
		t1.Sub(t0).Milliseconds(),
		t2.Sub(t1).Milliseconds(),
		t2.Sub(t0).Milliseconds())

	_ = runtime.NumCPU
}
