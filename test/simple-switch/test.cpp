#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include "klee/klee.h"

pthread_mutex_t L = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char **argv) {
  int x = klee_int32("x");
  klee_ics_begin();

  // Should be 4 cases:
  //   x == 0 || x == 2 || x == 3 || x == 5
  //   x == 1
  //   x == 4
  //   x > 5

  switch (x) {
  case 0:
    klee_debug("CASE #0\n");
    break;

  case 1:
    pthread_mutex_lock(&L);
    klee_debug("CASE #1\n");
    pthread_mutex_unlock(&L);
    break;

  case 2:
    klee_debug("CASE #2\n");
    break;

  case 3:
    klee_debug("CASE #3\n");
    break;

  case 4:
    pthread_mutex_lock(&L);
    klee_debug("CASE #4\n");
    pthread_mutex_unlock(&L);
    break;

  case 5:
    klee_debug("CASE #5\n");
    break;

  default:
    pthread_mutex_lock(&L);
    klee_debug("CASE DEFAULT\n");
    pthread_mutex_unlock(&L);
    break;
  }

  return 0;
}
