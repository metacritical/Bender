package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

func main() {
	n, _ := strconv.Atoi(os.Args[1])
	t0 := time.Now()
	sink := make(chan int, 1)
	next := sink
	for i := 0; i < n; i++ {
		down := next
		up := make(chan int)
		next = up
		go func() { down <- (<-up) + 1 }()
	}
	spawn := time.Since(t0)
	t1 := time.Now()
	next <- 0
	v := <-sink
	wave := time.Since(t1)
	fmt.Printf("procs=%d result=%d spawn=%dms wave=%dms total=%dms\n",
		n, v, spawn.Milliseconds(), wave.Milliseconds(),
		(spawn + wave).Milliseconds())
}
