/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Nondet Execution Runtime Library
 * This turns (almost) all klee functions into no-ops
 *
 */

// Import pthread defs used by our runtime library
#include "../POSIX/wrappers/pthread.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

//-----------------------------------------------------------------------------
// Klee API Stubs
//-----------------------------------------------------------------------------

extern "C" {

#define INT_FUNCS(bits, type) \
  u##type klee_int##bits(const char *name) { \
    return rand(); \
  } \
  type klee_range##bits(type start, type end, const char *name) { \
    if (start+1 == end) { \
      return start; \
    } else { \
      return rand() % (end - 1 - start) + start; \
    } \
  }

INT_FUNCS(8,  int8_t)
INT_FUNCS(32, int32_t)
INT_FUNCS(64, int64_t)

#undef INT_FUNCS

void klee_make_symbolic(char *buf, size_t nbytes, const char *name) {}
void klee_read_symbolic_input(char *buf, size_t nbytes, const char *name) {}
void klee_assume(uintptr_t cond) {}
void klee_ics_initialize(void) {}
void klee_ics_begin(void) {}
void klee_ics_region_marker(void) {}
void klee_thread_preempt(int yield) {}
void klee_thread_sleep(uint64_t, void*, uint64_t) {}
void klee_thread_notify(uint64_t, void*, uint64_t, int, uint64_t, uint64_t, int64_t, int) {}

void klee_process_terminate(void) { exit(0); }
void klee_thread_terminate(void) { pthread_exit(NULL); }

uint64_t klee_add_hbnode(void) { return 0; }
unsigned klee_is_symbolic(uintptr_t) { return 0; }
unsigned klee_is_running(void) { return 0; }

// Spinlock as in lib.cpp
// XXX: Probably should forward to pthreads, but this gives a fairer performance comparison

static int spinlock_lock(pthread_mutex_t *mutex, bool istry) {
  bool success = __sync_lock_test_and_set(&mutex->taken, 1);
  if (!success) {
    // Recursive?
    // N.B.: uclibc initializes locks to be recursive, which we currently
    // do not support in the symbolic models.  I'm not sure they actually
    // *need* recursion support, but just in case, we use the "queued" field
    // as a "count" to support recursion in this spinlock.
    if (mutex->owner == pthread_self()) {
      mutex->queued++;
      return 0;
    }

    // Somebody else holds it
    if (istry)
      return EBUSY;

    while (!__sync_lock_test_and_set(&mutex->taken, 1))
      /* spin */;
  }

  // Success
  mutex->owner = pthread_self();
  mutex->queued = 1;

  return 0;
}

static int spinlock_unlock(pthread_mutex_t *mutex) {
  // N.B.: Technically the first condition here would imply a data race,
  // but, since we assume data race freedom, we'll ignore that possibility.
  if (!mutex->taken || mutex->owner != pthread_self())
    return EPERM;

  // Recursive?
  // See comments in spinlock_lock() about recursion
  if (mutex->queued > 0) {
    mutex->queued--;
    if (mutex->queued != 0)
      return 0;
  }

  // Done with the lock
  __sync_lock_release(&mutex->taken);
  return 0;
}

int __uClibc_wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  return spinlock_lock(mutex, false);
}
int __uClibc_wrap_pthread_mutex_trylock(pthread_mutex_t *mutex) {
  return spinlock_lock(mutex, true);
}
int __uClibc_wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
  return spinlock_unlock(mutex);
}

// Printing: enable if requested

static bool DoKleePrints = false;
struct Initializer {
  Initializer() {
    const char *e = getenv("KLEE_ENABLE_DEBUG_PRINTS");
    if (e && strcmp(e, "1") == 0)
      DoKleePrints = true;
  }
};
Initializer initializer;

void klee_warning(const char *msg) {
  if (DoKleePrints) {
    fprintf(stderr, "KLEE.WARNING: %s\n", msg);
  }
}

void klee_debug(const char *msg, ...) {
  if (DoKleePrints) {
    char tmp[32+strlen(msg)];
    snprintf(tmp, sizeof tmp, "KLEE.DEBUG: %s", msg);
    va_list ap;
    va_start(ap, msg);
    vfprintf(stderr, tmp, ap);
    va_end(ap);
  }
}

void klee_print_expr(const char *msg, ...) {
  if (DoKleePrints) {
    fprintf(stderr, "KLEE.DEBUG.EXPR: %s\n", msg);
  }
}

void klee_print_expr_only(const char *msg, ...) {
  if (DoKleePrints) {
    fprintf(stderr, "KLEE.DEBUG.EXPR: %s\n", msg);
  }
}

void klee_print_expr_concretizations(const char *msg, ...) {
  if (DoKleePrints) {
    fprintf(stderr, "KLEE.DEBUG.EXPR: %s\n", msg);
  }
}

}  // extern "C"
