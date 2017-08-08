#include <assert.h>
#include <pthread.h>
#include "klee/klee.h"

static char dummy[1000];
struct ARGS { int x; };
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

__attribute((noinline))
static int plusone(int x) {
  return x + 1;
}

void* run(void* in) {
  struct ARGS* args = (struct ARGS*)in;
  int x = args->x;
  int z = plusone(x);

  klee_print_expr_only("X:", x);
  klee_print_expr_only("Z:", z);

  // Schedule should depend on X

  if (z == 3) {
    klee_debug("BRANCH-1\n");
  } else {
    // klee should take this branch first
    klee_debug("BRANCH-2\n");
  }

  if (x == 2) {
    klee_debug("LOCKING\n");
    pthread_mutex_lock(&lock);
    pthread_mutex_unlock(&lock);
  }

  klee_debug("THREAD COMPLETE!\n");
  klee_print_expr("X:", x);
  return NULL;
}

int main(int argc, char **argv) {
  int x = klee_range32(0, 10, "x");
  klee_ics_begin();

  struct ARGS args;
  args.x = x;

  pthread_t t;
  pthread_create(&t, 0, run, &args);
  pthread_join(t, 0);

  return 0;
}
