#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include "klee/klee.h"

struct ARGS { int tid,x,y,z; };
int global = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void* run(void* in) {
  struct ARGS* args = (struct ARGS*)in;
  int tid = args->tid;
  int x = args->x;
  int y = args->y;
  int z = args->z;

  // Schedules should depend on X only

  if (y > 10) {
    klee_debug("BRANCH-1\n");
    if (z > 20)
      klee_debug("BRANCH-2\n");
    else
      klee_debug("BRANCH-3\n");
  }

  if (x == tid) {
    klee_debug("BRANCH-4\n");
    if (y == 11)
      klee_debug("BRANCH-5\n");
    else
      klee_debug("BRANCH-6\n");

    pthread_mutex_lock(&lock);

    if (z == 21)
      klee_debug("BRANCH-7\n");
    else
      klee_debug("BRANCH-8\n");

    if (global == 0)
      klee_debug("BRANCH-9\n");
    if (global == 100)
      klee_debug("BRANCH-10\n");
    if (global == 101)
      klee_debug("BRANCH-11\n");

    global = 100 + tid;

    pthread_mutex_unlock(&lock);
  }

  if (y == 12) {
    klee_debug("BRANCH-12\n");
    if (z == 22)
      klee_debug("BRANCH-13\n");
    else
      klee_debug("BRANCH-14\n");
  }

  klee_debug("THREAD COMPLETE!\n");
  klee_print_expr_only("T:", tid);
  klee_print_expr_only("X:", x);
  klee_print_expr_only("Y:", y);
  klee_print_expr_only("Z:", z);
  return NULL;
}

int main(int argc, char **argv) {
  int x[2],y[2],z[2];
  pthread_t t[2];

  klee_make_symbolic(x, sizeof x, "x");
  klee_make_symbolic(y, sizeof y, "y");
  klee_make_symbolic(z, sizeof z, "z");
  klee_ics_begin();

  struct ARGS args[2];
  for (int i = 0; i < 2; ++i) {
    args[i].tid = i;
    args[i].x = x[i];
    args[i].y = y[i];
    args[i].z = z[i];
    pthread_create(&t[i], 0, run, &args[i]);
  }

  for (int i = 0; i < 2; ++i)
    pthread_join(t[i], 0);

  return 0;
}
