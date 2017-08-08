#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include "klee/klee.h"

pthread_mutex_t L = PTHREAD_MUTEX_INITIALIZER;

void* run(void* in) {
  // do nothing
  pthread_mutex_lock(&L);
  pthread_mutex_unlock(&L);
  klee_debug("THREAD COMPLETE!\n");
  return NULL;
}

int main(int argc, char **argv) {
  int x;
  pthread_t t[2];

  klee_make_symbolic(&x, sizeof x, "x");
  klee_ics_begin();

  // This produces just one schedule, which has thread creation
  // and mutex locking, and all other "early abort" schedules are
  // represented implicitly as prefixes of this one schedule.
  //
  if (x < 10) {
    if (x < 0) {
      klee_debug("exit: abort\n");
      assert(false); // abort();
    } else {
      klee_debug("exit: code=0\n");
      assert(false); // exit(0);
    }
  }
  if (x < 30) {
    if (x < 20) {
      klee_debug("exit: code=1\n");
      assert(false); // exit(1);
    } else {
      klee_debug("exit: code=2\n");
      assert(false); // exit(2);
    }
  }

  for (int i = 0; i < 2; ++i)
    pthread_create(&t[i], 0, run, NULL);

  for (int i = 0; i < 2; ++i)
    pthread_join(t[i], 0);

  return 0;
}
