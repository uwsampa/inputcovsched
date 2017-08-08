#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include "klee/klee.h"

struct ARGS { int tid, M; };
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void* run(void* in) {
  ARGS *args = (ARGS*)in;

  // Should have 3*4 = 12 schedules

  for (int i = 0; i < args->M; ++i) {
    pthread_mutex_lock(&lock);
    pthread_mutex_unlock(&lock);
  }

  klee_debug("THREAD COMPLETE\n");
}

int main(int argc, char **argv) {
  ARGS x,y;
  x.tid = 0;
  y.tid = 1;

  x.M = klee_range32(1, 4, "X");
  y.M = klee_range32(1, 5, "Y");
  klee_ics_begin();

  pthread_t t[2];
  pthread_create(&t[0], 0, run, (void*)&x);
  pthread_create(&t[1], 0, run, (void*)&y);

  for (int i = 0; i < 2; ++i)
    pthread_join(t[i], 0);

  return 0;
}
