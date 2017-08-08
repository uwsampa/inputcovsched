/*
 * Cloud9 Parallel Symbolic Execution Engine
 *
 * Copyright (c) 2011, Dependable Systems Laboratory, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Dependable Systems Laboratory, EPFL nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE DEPENDABLE SYSTEMS LABORATORY, EPFL BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * All contributors are listed in CLOUD9-AUTHORS file.
 *
 */

#ifndef KLEE_POSIX_PTHREAD_WRAP_H_
#define KLEE_POSIX_PTHREAD_WRAP_H_

// for __BEGIN_DECLS
#include <sys/cdefs.h>
// for uint64_t
#include <stdint.h>
// for a few constants
#include "../config.h"

////////////////////////////////////////////////////////////////////////////////
// Wrapper around pthread types
////////////////////////////////////////////////////////////////////////////////

// Define our own pthread types
#define _PTHREAD_H 1
#define _BITS_PTHREADTYPES_H 1

struct timespec;

typedef uint64_t pthread_t;
typedef int pthread_once_t;
typedef int pthread_key_t;
// These are ignored, for now
typedef struct { uint64_t val; } pthread_attr_t;
typedef struct { uint64_t val; } pthread_mutexattr_t;
typedef struct { uint64_t val; } pthread_condattr_t;
typedef struct { uint64_t val; } pthread_barrierattr_t;
typedef struct { uint64_t val; } pthread_rwlockattr_t;

enum {
  PTHREAD_MUTEX_NORMAL,
  // Unsupported:
  // PTHREAD_MUTEX_RECURSIVE
  // PTHREAD_MUTEX_ERRORCHECK
  // PTHREAD_MUTEX_DEFAULT (N.B.: need to check if our current impl is "normal" or "default")
};

enum {
  PTHREAD_PROCESS_PRIVATE,
  // Unsupported:
  // PTHREAD_PROCESS_SHARED
};

#define PTHREAD_MUTEX_INITIALIZER  { 0, 0, 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER   { 0, 0, 0 }
#define PTHREAD_RWLOCK_INITIALIZER { 0, 0, 0, 0, 0, 0, 0, 0 }

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#define PTHREAD_ONCE_INIT             (0)

#define SEM_VALUE_MAX                 (32000)

// Copied from uClibc
struct _pthread_cleanup_buffer {
  void (*__routine) (void *);  // function to call
  void *__arg;                 // its argument
  int __canceltype;            // saved cancellation type
  struct _pthread_cleanup_buffer *__prev;  // chaining of cleanup functions
};

////////////////////////////////////////////////////////////////////////////////
// Synchronization Data Structures
////////////////////////////////////////////////////////////////////////////////

// WARNING: don't change these types without *at least* changing
// pthread_mutex_t (see comments)
typedef uint64_t wlist_id_t;
typedef uint64_t hbnode_id_t;

typedef struct pthread_mutex_t   pthread_mutex_t;
typedef struct pthread_cond_t    pthread_cond_t;
typedef struct pthread_barrier_t pthread_barrier_t;
typedef struct pthread_rwlock_t  pthread_rwlock_t;
typedef struct sem_t             sem_t;

struct pthread_mutex_t {
  wlist_id_t wlist;
  hbnode_id_t last_hbnode;

  // N.B.: To work with uclibc, we need to ensure that uclibc's
  // PTHREAD_MUTEX_INITIALIZERs will work here.  The fields in
  // that struct are (from ../uclibc/include/bits/pthreadtypes.h):
  //    int reserved    // overlaps with wlist
  //    int count       // overlaps with wlist
  //    pthread* owner  // overlaps with last_hbnode
  //    int kind        // overlaps with owner
  //    lock_t lock     // overlaps with owner / queued
  // The only non-zero field in uclibc's mutex initializer is the
  // "kind", so we overlap that with our "owner", which is unused
  // when taken == 0.
  pthread_t owner;
  unsigned int queued;
  char taken;

#ifdef ENABLE_KLEE_PTHREAD_PADDING
  char pad[64];  // stupid hack for nondet
#endif
};

struct pthread_cond_t {
  wlist_id_t wlist;

  pthread_mutex_t *mutex;
  unsigned int queued;
  hbnode_id_t fake_initialized;

#ifdef ENABLE_KLEE_PTHREAD_PADDING
  char pad[64];  // stupid hack for nondet
#endif
};

struct pthread_barrier_t {
  wlist_id_t wlist;

  unsigned int init_count;
  unsigned int left;
  hbnode_id_t fake_initialized;

#ifdef ENABLE_KLEE_PTHREAD_PADDING
  char pad[64];  // stupid hack for nondet
#endif
};

struct pthread_rwlock_t {
  wlist_id_t wlist_readers;
  wlist_id_t wlist_writers;

  hbnode_id_t last_reader_hbgroup;
  hbnode_id_t last_writer_hbnode;

  unsigned int nr_readers;
  unsigned int nr_readers_queued;
  unsigned int nr_writers_queued;
  unsigned int writer;
};

struct sem_t {
  wlist_id_t wlist;
  hbnode_id_t last_hbnode;
  unsigned int count;
};

////////////////////////////////////////////////////////////////////////////////
// Thread Info Data Structures
////////////////////////////////////////////////////////////////////////////////

typedef struct thread_data_t        thread_data_t;
typedef union  thread_data_padded_t thread_data_padded_t;
typedef struct tsync_data_t         tsync_data_t;

struct thread_data_t {
  // N.B.: ExecutorUtil.cpp expects this to come first
  wlist_id_t wlist;

  // used by ConstrainedExec runtime, not used by symbolic runtime
  void *(*start_routine)(void*);
  void *start_arg;
  unsigned id;
  unsigned initial_fragment_id;
  uint32_t wait_slot;

  // used by both runtimes
  void *return_value;
  struct _pthread_cleanup_buffer *cleanup_stack;

  char allocated;
  char terminated;
  char joinable;
  char hasjoiner;
};

union KLEE_CACHELINE_ALIGN thread_data_padded_t {
  char padding[KLEE_CACHELINE_SIZE*2];
  thread_data_t data;
};

struct KLEE_CACHELINE_ALIGN tsync_data_t {
  thread_data_padded_t threads[KLEE_MAX_THREADS];
  uint32_t    live_threads;   // total count of currently live threads
  hbnode_id_t last_hbnode;    // used to serialize pthread_create and pthread_join
  wlist_id_t  joinall_wlist;  // used by pthread_joinall()
};

extern tsync_data_t __tsync;

////////////////////////////////////////////////////////////////////////////////
// Wrapper around pthread functions
////////////////////////////////////////////////////////////////////////////////

#ifndef KLEE_NO_PTHREAD_PROTOS

#include <time.h>

__BEGIN_DECLS

extern pthread_t pthread_self(void);
extern int pthread_equal(pthread_t thread1, pthread_t thread2);

extern int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void*), void *arg);

extern void pthread_exit(void *value_ptr);
extern int pthread_join(pthread_t thread, void **value_ptr);
extern int pthread_joinall(void);
extern int pthread_detach(pthread_t thread);
extern int pthread_attr_init(pthread_attr_t *attr);
extern int pthread_attr_destroy(pthread_attr_t *attr);

extern int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

extern int pthread_mutexattr_init(pthread_mutexattr_t *attr);
extern int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
extern int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);

extern int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
extern int pthread_mutex_destroy(pthread_mutex_t *mutex);
extern int pthread_mutex_lock(pthread_mutex_t *mutex);
extern int pthread_mutex_trylock(pthread_mutex_t *mutex);
extern int pthread_mutex_unlock(pthread_mutex_t *mutex);
extern int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
extern int pthread_cond_destroy(pthread_cond_t *cond);
extern int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                  const struct timespec *abstime);
extern int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
extern int pthread_cond_broadcast(pthread_cond_t *cond);
extern int pthread_cond_signal(pthread_cond_t *cond);

extern int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr,
                                unsigned int count);
extern int pthread_barrier_destroy(pthread_barrier_t *barrier);
extern int pthread_barrier_wait(pthread_barrier_t *barrier);

extern int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);
extern int pthread_rwlock_destroy(pthread_rwlock_t *rwlock);
extern int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
extern int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock);
extern int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
extern int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock);
extern int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);

// Semaphores
int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_destroy(sem_t *sem);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
int sem_post(sem_t *sem);

// Unsupported
extern int pthread_cancel(pthread_t thread);
extern int pthread_key_create(pthread_key_t *key, void (*dtor) (void *));
extern int pthread_key_delete(pthread_key_t key);
extern void* pthread_getspecific(pthread_key_t key);
extern int pthread_setspecific(pthread_key_t key, const void *ptr);

__END_DECLS

#endif

#endif /* KLEE_POSIX_PTHREAD_WRAP_H_ */
