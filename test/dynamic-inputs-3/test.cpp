#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include "klee/klee.h"

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int global;

int main(int argc, char **argv) {
  // Initial input
  klee_make_symbolic(&global, sizeof global, "global");
  klee_ics_begin();

  if (global) {
    pthread_mutex_lock(&lock);
    klee_debug("GLOBAL IS TRUE\n");
    pthread_mutex_unlock(&lock);
  } else {
    pthread_mutex_lock(&lock);
    klee_debug("GLOBAL IS FALSE\n");
    pthread_mutex_unlock(&lock);
  }

  klee_ics_region_marker();

  // Second input
  klee_debug("READING FOO ...\n");
  int foo = klee_int32("foo");
  klee_debug("DONE.\n");

  if (foo == global) {
    pthread_mutex_lock(&lock);
    klee_debug("FOO IS TRUE\n");
    pthread_mutex_unlock(&lock);
  } else {
    pthread_mutex_lock(&lock);
    klee_debug("FOO IS FALSE\n");
    pthread_mutex_unlock(&lock);
  }

  // Second input
  klee_debug("READING BAR ...\n");
  int bar = klee_int32("bar");
  klee_debug("DONE.\n");

  if (bar == global+1) {
    pthread_mutex_lock(&lock);
    klee_debug("BAR IS TRUE\n");
    pthread_mutex_unlock(&lock);
  } else {
    pthread_mutex_lock(&lock);
    klee_debug("BAR IS FALSE\n");
    pthread_mutex_unlock(&lock);
  }

  return 0;
}
