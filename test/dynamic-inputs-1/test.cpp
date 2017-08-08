#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include "klee/klee.h"

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char **argv) {
  // Initial input
  int foo = klee_int32("foo");
  klee_ics_begin();

  if (foo) {
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

  if (bar) {
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
