/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Constrained Execution Runtime Library
 * Hooks for calling actual pthreads functions.  This is
 * in its own library to limit the amount of code that can
 * includes <pthread.h> (see comments in lib.h).
 *
 */

#include "lib.h"

#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <time.h>

extern long int syscall(long int syscallno, ...);
extern void _exit(int status) __attribute__((noreturn));

// From <pthread.h>
extern int MANGLED(pthread_create)(pthread_t *restrict newthread,
                                   const pthread_attr_t *restrict attr,
                                   void *(*start_routine) (void*),
                                   void *restrict arg) __THROW __nonnull ((1, 3));

extern int MANGLED(pthread_attr_init)(pthread_attr_t *attr) __THROW __nonnull ((1));

extern void MANGLED(pthread_exit)(void *retval) __attribute__ ((__noreturn__));

// Wrappers
int __ics_call_real_pthread_create(const void *attr, void *(*startRoutine)(void*), void *arg) {
  pthread_t th;
  pthread_attr_t tmpAttr;
  // Since our pthread_attr_t format is potentially different from the format
  // used by the local system's pthread library, we need to do a translation.
  // TODO: If we ever support attrs, we need to update this
  MANGLED(pthread_attr_init)(&tmpAttr);
  return MANGLED(pthread_create)(&th, &tmpAttr, startRoutine, arg);
}

void __ics_call_real_pthread_exit(void *returnValue) {
  MANGLED(pthread_exit)(returnValue);
}

void __ics_call_real_exit(void) {
  syscall(SYS_exit_group, 0);
  _exit(0);  // this statement should be unreachable
}

void __ics_call_real_abort(void) {
  // Okay, this is not the "real" abort function, but the "real" abort function
  // is a libc library call, but here we want to make syscalls and avoid library
  // calls, so instead we emit an instruction that should cause a processor trap
  // that will invoke a debugger breakpoint.
  __asm__ __volatile__("hlt");
  _exit(0);  // this statement should be unreachable
}

int __ics_call_real_sched_yield(void) {
  syscall(SYS_sched_yield);
  return 0;
}

///////////////////////////////////////////////////////////////
// OK to call uclibc below here
///////////////////////////////////////////////////////////////

void __ics_srand(void) {
  srand(time(NULL));
}

uint8_t __ics_rand8(void) {
  return (uint8_t)rand();
}

uint32_t __ics_rand32(void) {
  return rand();
}

uint64_t __ics_rand64(void) {
  return (uint64_t)rand() | ((uint64_t)rand() << 32);
}
