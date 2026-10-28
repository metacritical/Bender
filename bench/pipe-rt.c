#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
/* Cost of N sequential pipe writes+reads: the floor for any pipe-based wake. */
int main(void){
  int p[2]; pipe(p);
  char buf[48]; memset(buf,0,sizeof buf);
  struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
  for(int i=0;i<20000;i++){ write(p[1],buf,sizeof buf); read(p[0],buf,sizeof buf); }
  clock_gettime(CLOCK_MONOTONIC,&b);
  double us=((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/1000.0/20000.0;
  printf("pipe write+read round trip: %.3f us/op\n",us);
  return 0;
}
