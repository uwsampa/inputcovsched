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

#ifndef KLEE_POSIX_COMMON_H_
#define KLEE_POSIX_COMMON_H_

#include "config.h"

#include <sys/types.h>
#include <unistd.h>

#ifdef __USE_MISC
#undef __USE_MISC
#define __REDEF_MISC
#endif

#ifdef __USE_XOPEN2K8
#undef __USE_XOPEN2K8
#define __REDEF_XOPEN2K8
#endif

#include <sys/stat.h>

#ifdef __REDEF_MISC
#define __USE_MISC 1
#undef __REDEF_MISC
#endif

#ifdef __REDEF_XOPEN2K8
#define __USE_XOPEN2K8 1
#undef __REDEF_XOPEN2K8
#endif

#include <string.h>
#include "klee/klee.h"

////////////////////////////////////////////////////////////////////////////////
// Misc
////////////////////////////////////////////////////////////////////////////////

// Macro used to report an error when a function is called that is not implemented
// N.B.: Using this macro will cause LLVM to optimize the program under the assumption
// that the calling function does-not-return, so we use this only when we explicitly
// assume that a function is not called by user programs
#define FUNCTION_NOT_IMPLEMENTED \
  klee_report_error(__FILE__, __LINE__, "function not implemented", "posix.err")

// Disable branch counter flag in a scoped region
#define KLEE_PUSH_BRANCH_COUNTER_DISABLED \
  int __the_old_brcnt_flag = klee_get_option(KLEE_OPTION_BRANCH_COUNTER_ENABLED); \
  klee_set_option(KLEE_OPTION_BRANCH_COUNTER_ENABLED, 0);

#define KLEE_POP_BRANCH_COUNTER_DISABLED \
  klee_set_option(KLEE_OPTION_BRANCH_COUNTER_ENABLED, __the_old_brcnt_flag); \


////////////////////////////////////////////////////////////////////////////////
// Model Infrastructure
////////////////////////////////////////////////////////////////////////////////

//
// A model is a stand-in for an existing C library call.  For example,
// a model for the read() call is named "__klee_model_read".  If the
// read() model needs to invoke the actual implementation, it can do so
// by calling "__klee_original_read".  Linking is implemented in
// lib/Core/Init.cpp and works as follows:
//
//   Step1: We link in uClibc
//
//   Step2: We link in this library
//     To ensure that the linker pulls in all model functions, we mark
//     them with __attribute__((used)).
//
//   Step3: Foreach model __klee_model_foo, we make the following replacements:
//     (a) foo->replaceAllUsesWith(__klee_model_foo)
//     (b) __klee_original_foo->replaceAllUsesWith(foo)
//     After step (b), __klee_original_foo can be deleted
//
// Unlinking of models is also possible by simply doing:
//     (a) __klee_model_foo->replaceAllUsesWith(foo)
//     After this step, __klee_model_foo can be deleted
//
// There are two kinds of models: pthreads models (__klee_pthread_model_*)
// and everything else (__klee_model_*).  The pthreads models includes only
// those functions that are normally defined in libpthread, and we separate
// them so they can be treated differently (see lib/ICS/Instrumentor.cpp).
// Perhaps, for consistency, each model should be annotated with its source
// library (libc, libm, etc.)?
//

// Model naming

#define __MODEL(name)         __klee_model_ ## name
#define __PTHREAD_MODEL(name) __klee_pthread_model_ ## name
#define __ORIGINAL(name)      __klee_original_ ## name

// Declaring a model prototype

#define __DECLARE_MODEL(model, type, name, ...) \
  extern type model(__VA_ARGS__); \
  extern type __ORIGINAL(name)(__VA_ARGS__);

#define DECLARE_MODEL(type, name, ...) \
  __DECLARE_MODEL(__klee_model_ ## name, type, name, ##__VA_ARGS__)

#define DECLARE_PTHREAD_MODEL(type, name, ...) \
  __DECLARE_MODEL(__klee_pthread_model_ ## name, type, name, ##__VA_ARGS__)

#define DECLARE_INPUT_MODEL(type, name, ...) \
  __DECLARE_MODEL(__klee_input_model_ ## name, type, name, ##__VA_ARGS__)

// Defining a model function
// N.B.: "noinline" is needed to enable the above linking strategy

#define __DEFINE_MODEL(model, type, name, ...) \
  extern type name(__VA_ARGS__); \
  type model(__VA_ARGS__); \
  __attribute__((used)) __attribute__((noinline)) type model(__VA_ARGS__)

#define DEFINE_MODEL(type, name, ...) \
  __DEFINE_MODEL(__klee_model_ ## name, type, name, ##__VA_ARGS__)

#define DEFINE_PTHREAD_MODEL(type, name, ...) \
  __DEFINE_MODEL(__klee_pthread_model_ ## name, type, name, ##__VA_ARGS__)

#define DEFINE_INPUT_MODEL(type, name, ...) \
  __DEFINE_MODEL(__klee_input_model_ ## name, type, name, ##__VA_ARGS__)

// Call the underlying "original" function

#define CALL_UNDERLYING(name, ...) \
  __ORIGINAL(name)(__VA_ARGS__)

// Declare this in each file containing model functions to ensure linkage

#define FORCE_LINKAGE(module) \
  __attribute__((used)) void __klee_force_ ## module ## _linkage(void) {}


////////////////////////////////////////////////////////////////////////////////
// Static Lists
////////////////////////////////////////////////////////////////////////////////

#define STATIC_LIST_INIT(list)  \
  do { memset(&list, 0, sizeof(list)); } while (0)

#define STATIC_LIST_ALLOC(list, item) \
  do { \
    item = sizeof(list)/sizeof(list[0]); \
    unsigned int __i; \
    for (__i = 0; __i < sizeof(list)/sizeof(list[0]); __i++) { \
      if (!list[__i].data.allocated) { \
        list[__i].data.allocated = 1; \
        item = __i;  break; \
      } \
    } \
  } while (0)

#define STATIC_LIST_CLEAR(list, item) \
  do { memset(&list[item], 0, sizeof(list[item])); } while (0)

#define STATIC_LIST_CHECK(list, item) \
  (((item) < sizeof(list)/sizeof(list[0])) && (list[item].allocated))


#endif /* KLEE_POSIX_COMMON_H_ */
