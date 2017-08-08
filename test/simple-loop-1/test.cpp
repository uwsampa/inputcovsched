#include <assert.h>
#include <pthread.h>
#include "klee/klee.h"

struct ARGS { int x; };
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void* run(void* in) {
  struct ARGS* args = (struct ARGS*)in;
  int x = args->x;

  // Schedule should depend on X

  for (int i = 0; i < 2; ++i) {
    klee_debug("ITERATION: %d\n", i);
    pthread_mutex_lock(&lock);
    pthread_mutex_unlock(&lock);

    // Make sure precond slicing captures the data dependence
    // from this x to the other x going back one iteration
    if (x < 10) {
      klee_debug("EXIT-EARLY\n");
      return NULL;
    }

    x = 100;
  }

  klee_debug("THREAD COMPLETE!\n");
  klee_print_expr_only("X:", x);
  return NULL;
}

int main(int argc, char **argv) {
  int x[2];
  pthread_t t[2];

  klee_make_symbolic(x, sizeof x, "x");
  klee_ics_begin();

  struct ARGS args[2];
  for (int i = 0; i < 2; ++i) {
    args[i].x = x[i];
    pthread_create(&t[i], 0, run, &args[i]);
  }

  for (int i = 0; i < 2; ++i)
    pthread_join(t[i], 0);

  return 0;
}
