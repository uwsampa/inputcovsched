#include <assert.h>
#include <pthread.h>
#include "klee/klee.h"

struct ARGS { int x,y; };
pthread_mutex_t locks[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };

void* run(void* in) {
  struct ARGS* args = (struct ARGS*)in;
  int x = args->x;
  int y = args->y;

  pthread_mutex_t *L;

  // Schedule should depend on X

  if (x < 10) {
    L = &locks[0];
    klee_debug("BRANCH-1\n");
  } else {
    L = &locks[1];
    klee_debug("BRANCH-2\n");
  }

  // code was carefully chosen so that when compiled with -O3,
  // there is a phi node on the control flow merge point here
  pthread_mutex_lock(L);
  pthread_mutex_unlock(L);

  // Schedule should not depend on Y

  if (y < 20)
    klee_debug("BRANCH-3\n");
  else
    klee_debug("BRANCH-4\n");

  klee_debug("THREAD COMPLETE!\n");
  klee_print_expr_only("X:", x);
  klee_print_expr_only("Y:", y);

  // Simple region test: put a marker at the end of this thread.
  // The parallel region should end with two threads here and one thread in join.
  klee_ics_region_marker();

  return NULL;
}

int main(int argc, char **argv) {
  int x[2],y[2];
  pthread_t t[2];

  klee_make_symbolic(x, sizeof x, "x");
  klee_make_symbolic(y, sizeof y, "y");
  klee_ics_begin();

  struct ARGS args[2];
  for (int i = 0; i < 2; ++i) {
    args[i].x = x[i];
    args[i].y = y[i];
    pthread_create(&t[i], 0, run, &args[i]);
  }

  pthread_joinall();

  // Avoid the gnarly uclib exit code since we're not forming regions
  klee_process_terminate();
}
