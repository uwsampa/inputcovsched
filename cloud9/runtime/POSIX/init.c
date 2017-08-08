//===-- init.c -----------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common.h"
#include "multiprocess.h"

FORCE_LINKAGE(init)

////////////////////////////////////////////////////////////////////////////////
// Semaphores
////////////////////////////////////////////////////////////////////////////////

#ifdef ENABLE_KLEE_SEMAPHORES

sem_set_t __sems[KLEE_MAX_SEMAPHORES];

static void klee_init_semaphores(void) {
  STATIC_LIST_INIT(__sems);
}

#else

static void klee_init_semaphores(void) {
}

#endif

////////////////////////////////////////////////////////////////////////////////
// Threads
////////////////////////////////////////////////////////////////////////////////

tsync_data_t __tsync;

static void klee_init_default_thread(void) {
  thread_data_t *def_data = &__tsync.threads[DEFAULT_THREAD].data;
  def_data->allocated = 1;
  def_data->joinable = 1; // Why not?
  def_data->wlist = klee_get_wlist();
}

static void klee_init_threads(void) {
  STATIC_LIST_INIT(__tsync.threads);
  __tsync.live_threads = 1;
  __tsync.last_hbnode = 0;
  __tsync.joinall_wlist = klee_get_wlist();
}

////////////////////////////////////////////////////////////////////////////////
// Symbolic initialization
////////////////////////////////////////////////////////////////////////////////

__attribute__((used))
void klee_init_system_for_new_symbolic_region(void) {
  // Initialize system-wide structures other than __tsync
  klee_init_semaphores();

  // Initialize __tsync
  __tsync.live_threads = 0;  // will be incremented below
  __tsync.last_hbnode = 0;
  // __tsync.joinall_wlist stays symbolic

  // Initialize __tsync.threads
  // TODO: check static analysis: if there exist calls to pthread_detach(), the
  //  joinable field should be made symbolic, and otherwise it can be left at 1.
  // TODO: check static analysis: if there is a _pthread_cleanup_push() up the stack,
  // include it here (this should be pretty easy, since the calls must be in scope)
  char islive[KLEE_MAX_THREADS];
  char iszombie[KLEE_MAX_THREADS];
  int tid;

  memset(islive, 0, sizeof islive);
  memset(iszombie, 0, sizeof iszombie);
  klee_enumerate_threads(islive, iszombie);

  for (tid = 0; tid < KLEE_MAX_THREADS; ++tid) {
    thread_data_t *tdata = &__tsync.threads[tid].data;

    if (!islive[tid] && !iszombie[tid]) {
      tdata->allocated = 0;
      continue;
    }

    // tdata->wlist stays symbolic
    tdata->cleanup_stack = NULL; // TODO: read via a simple static analysis
    tdata->allocated = 1;
    tdata->joinable = 1;

#ifndef DISABLE_KLEE_SYNC_INVARIANTS
    unsigned may,must;
    __enumerate_possible_waiters(&tdata->wlist, SyncOpMutex, &may, &must);
    if (must > 0) {
      tdata->hasjoiner = 1;
    } else if (may == 0) {
      tdata->hasjoiner = 0;
    }
#endif

    if (islive[tid]) {
      __tsync.live_threads++;
      tdata->return_value = NULL;
      tdata->terminated = 0;
    } else if (iszombie[tid]) {
      // tdata->return_value is unknown, so leave it symbolic
      tdata->terminated = 1;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// Commandline and Environment
////////////////////////////////////////////////////////////////////////////////

#define __emit_error(msg) \
  klee_report_error(__FILE__, __LINE__, msg, "user.err")

/* Helper function that converts a string to an integer, and
   terminates the program with an error message is the string is not a
   proper number */   
static long int __str_to_int(char *s, const char *error_msg) {
  long int res;
  char *endptr;

  if (*s == '\0')
    __emit_error(error_msg);

  res = strtol(s, &endptr, 0);

  if (*endptr != '\0')
    __emit_error(error_msg);

  return res;
}

static int __isprint(const char c) {
  /* Assume ASCII */
  return ((32 <= c) & (c <= 126));
}

static int __streq(const char *a, const char *b) {
  while (*a == *b) {
    if (!*a)
      return 1;
    a++;
    b++;
  }
  return 0;
}

static char *__get_sym_str(int numChars, char *name) {
  int i;
  char *s = malloc(numChars+1);
  klee_mark_global(s);
  klee_make_symbolic(s, numChars+1, name);

  for (i=0; i<numChars; i++)
    klee_prefer_cex(s, __isprint(s[i]));
  
  s[numChars] = '\0';
  return s;
}

static void __add_arg(int *argc, char **argv, char *arg, int argcMax) {
  if (*argc==argcMax) {
    __emit_error("too many arguments for klee_init_env");
  } else {
    argv[*argc] = arg;
    (*argc)++;
  }
}

__attribute__((noinline))
void klee_init_env(int argc, char **argv) {
  klee_init_threads();
  klee_init_default_thread();
  klee_init_semaphores();
}

__attribute__((noinline))
void klee_process_args(int* argcPtr, char*** argvPtr) {
  int argc = *argcPtr;
  char** argv = *argvPtr;

  int new_argc = 0, n_args;
  char* new_argv[1024];
  unsigned max_len, min_argvs, max_argvs;
  char** final_argv;
  char sym_arg_name[5] = "arg";
  unsigned sym_arg_num = 0;
  int k=0, i;

  sym_arg_name[4] = '\0';

  while (k < argc) {
    if (__streq(argv[k], "--sym-arg") || __streq(argv[k], "-sym-arg")) {
      const char *msg = "--sym-arg expects an integer argument <max-len>";
      if (++k == argc)        
        __emit_error(msg);
                
      max_len = __str_to_int(argv[k++], msg);
      sym_arg_name[3] = '0' + sym_arg_num++;
      __add_arg(&new_argc, new_argv, 
                __get_sym_str(max_len, sym_arg_name),
                1024);
    }
    else if (__streq(argv[k], "--sym-args") || __streq(argv[k], "-sym-args")) {
      const char *msg = 
        "--sym-args expects three integer arguments <min-argvs> <max-argvs> <max-len>";

      if (k+3 >= argc)
        __emit_error(msg);

      k++;
      min_argvs = __str_to_int(argv[k++], msg);
      max_argvs = __str_to_int(argv[k++], msg);
      max_len = __str_to_int(argv[k++], msg);

      n_args = klee_range32(min_argvs, max_argvs+1, "n_args");
      for (i=0; i < n_args; i++) {
        sym_arg_name[3] = '0' + sym_arg_num++;
        __add_arg(&new_argc, new_argv, 
                  __get_sym_str(max_len, sym_arg_name),
                  1024);
      }
    }
    else {
      /* simply copy arguments */
      __add_arg(&new_argc, new_argv, argv[k++], 1024);
    }
  }

  final_argv = (char**) malloc((new_argc+1) * sizeof(*final_argv));
  klee_mark_global(final_argv);
  memcpy(final_argv, new_argv, new_argc * sizeof(*final_argv));
  final_argv[new_argc] = 0;

  *argcPtr = new_argc;
  *argvPtr = final_argv;

  klee_event(KLEE_EVENT_BREAKPOINT, KLEE_BRK_START_TRACING);
}
