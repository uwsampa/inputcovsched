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

#ifndef KLEE_POSIX_CONFIG_H_
#define KLEE_POSIX_CONFIG_H_

////////////////////////////////////////////////////////////////////////////////
// System Limits
////////////////////////////////////////////////////////////////////////////////

// XXX: WARNING: KLEE_MAX_THREADS is currently hardcoded in Executor::addThreadIdConstraintsFor()
#define KLEE_MAX_THREADS         16
#define KLEE_CACHELINE_SIZE      64

// Magic incantation to ensure cacheline alignment
// This goes just after the "struct" or "union" keyword
#define KLEE_CACHELINE_ALIGN  __attribute__((aligned(KLEE_CACHELINE_SIZE)))

////////////////////////////////////////////////////////////////////////////////
// Enabled Components
////////////////////////////////////////////////////////////////////////////////

// If defined, we will force preemption after each unlock operation
//#define ENABLE_KLEE_PTHREAD_ALWAYS_PREEMPT_UNLOCK

// For experiments: define this to disable sync invariants
#define DISABLE_KLEE_SYNC_INVARIANTS

// For experiments: define this to disable calling cleanup fns during thread exit
//#define DISABLE_KLEE_PTHREAD_SYMBOLIC_CLEANUP_ON_EXIT


#endif /* KLEE_POSIX_CONFIG_H_ */
