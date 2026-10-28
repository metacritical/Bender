#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <unistd.h>
static atomic_int req, ack;
static void *worker(void *p){
  for(;;){
    while(!atomic_load(&req)) { }
    atomic_store(&req,0);
    atomic_store(&ack,1);
  }
  return 0;
}
int main(void){
  pthread_t t; pthread_create(&t,0,worker,0);
  struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
  for(int i=0;i<20000;i++){ atomic_store(&req,1); while(!atomic_load(&ack)){} atomic_store(&ack,0); }
  clock_gettime(CLOCK_MONOTONIC,&b);
  double us=((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/1000.0/20000.0;
  printf("2-thread spin handoff round trip: %.3f us/op\n",us);
  return 0;
}
