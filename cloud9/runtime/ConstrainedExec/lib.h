/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Constrained Execution Runtime Library
 *
 * This library is essentially an imlementation of pthreads.  In
 * lib/ICS/Instrumentor.cpp, we transform the program so that all
 * pthreads calls are redirected into this library (this includes
 * pthreads calls made by the application as well as calls made by
 * uclibc).  In this library, our pthreads implementation contrains
 * execution to follow the set of schedules that were dumped into
 * global tables by lib/ICS/Instrumentor.cpp.
 *
 * N.B.: This library should NOT use any stdlib functions except
 * for a few pthreads functions that have been specially blessed.
 * The main reason is that stdlib functions may use pthreads
 * synchronization, which would cause a callback into this library,
 * which would deadlock because that synchronization did not appear
 * in the original program.  To avoid temptation, we do not include
 * any std headers here (except for stdint.h, which includes no
 * function prototypes.)
 *
 */

#ifndef ICS_CONSTRAINEDEXEC_LIB_H_
#define ICS_CONSTRAINEDEXEC_LIB_H_

#ifdef __cplusplus
extern "C" {
#endif

// For KLEE_MAX_THREADS, etc.
#include "../POSIX/config.h"

// Import pthread defs used by our runtime library
#define KLEE_NO_PTHREAD_PROTOS
#include "../POSIX/wrappers/pthread.h"

// For uint64_t and friends
#define __need_size_t
#define __need_NULL
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#define _FCNTL_H 1
#include <bits/fcntl.h>
#define _SYS_STAT_H 1
#include <bits/stat.h>
// For va_arg
#include <stdarg.h>

// Ugly mangling necessary so we can call uclibc directly (otherwise,
// lib/ICS/Instrumentor.cpp will rewrite some calls to point at our
// wrappers, and in the wrappers we need to call the actual functions).
#define MANGLED(name) __ics_libcall_##name

// Options
//#define ENABLE_DEBUGGING
//#define ENABLE_INSTRUMENTATION

//-----------------------------------------------------------------------------
// Some global definitions that mimic libc
//-----------------------------------------------------------------------------

#define assert(expr) \
  ((expr) ? (void)0  \
          : klee_assert_fail (#expr, __FILE__, __LINE__, __PRETTY_FUNCTION__))

//-----------------------------------------------------------------------------
// Defined in lib.cpp
//-----------------------------------------------------------------------------

void klee_assert_fail(const char *expr, const char *file, unsigned int line, const char *func)
  __attribute__((noreturn));

//-----------------------------------------------------------------------------
// Defined in pthreadcall.c
//-----------------------------------------------------------------------------

int  __ics_call_real_pthread_create(const void *attr, void *(*startRoutine)(void*), void *arg);
void __ics_call_real_pthread_exit(void *returnValue) __attribute__((noreturn));
void __ics_call_real_exit(void) __attribute__((noreturn));
void __ics_call_real_abort(void) __attribute__((noreturn));
int  __ics_call_real_sched_yield(void);

void __ics_srand(void);
uint8_t __ics_rand8(void);
uint32_t __ics_rand32(void);
uint64_t __ics_rand64(void);

//-----------------------------------------------------------------------------
// Defined in printf.c
//-----------------------------------------------------------------------------

#define PrintStdout(fmt, ...) __ics_printf(1, fmt, ##__VA_ARGS__)
#define PrintStderr(fmt, ...) __ics_printf(2, fmt, ##__VA_ARGS__)

extern void __ics_printf(int fd, const char *fmt, ...)
                         __attribute__((format(printf, 2, 3)));

extern void __ics_vprintf(int fd, const char *fmt, va_list ap);

extern char* __ics_snprintf(char *buf, unsigned long int bufsz, const char *fmt, ...)
                            __attribute__((format(printf, 3, 4)));

extern unsigned long int __ics_strlen(const char *str);

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // ICS_CONSTRAINEDEXEC_LIB_H_
