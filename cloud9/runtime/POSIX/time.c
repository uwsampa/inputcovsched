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

#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>
#include <utime.h>
#include <utmp.h>

#include "common.h"
#include "multiprocess.h"

FORCE_LINKAGE(time)

DEFINE_MODEL(int, usleep, useconds_t usec) {
  klee_warning("yielding instead of usleep()-ing");

  uint64_t tstart = klee_get_time();
  __thread_preempt(1);
  uint64_t tend = klee_get_time();

  if (tend - tstart < (uint64_t)usec)
    klee_set_time(tstart + usec);

  return 0;
}

DEFINE_MODEL(unsigned int, sleep, unsigned int seconds) {
  klee_warning("yielding instead of sleep()-ing");

  uint64_t tstart = klee_get_time();
  __thread_preempt(1);
  uint64_t tend = klee_get_time();

  if (tend - tstart < ((uint64_t)seconds)*1000000) {
    klee_set_time(tstart + ((uint64_t)seconds) * 1000000);
  }

  return 0;
}

DEFINE_MODEL(int, nanosleep, const struct timespec *req, struct timespec *rem) {
  klee_warning("yielding instead of nanosleep()-ing");

  //uint64_t tstart = klee_get_time();
  __thread_preempt(1);
  //uint64_t tend = klee_get_time();

  // XXX: bad fake: not calling klee_set_time() as above
  return 0;
}

DEFINE_INPUT_MODEL(int, gettimeofday, struct timeval *tv, struct timezone *tz) {
  struct timeval tv_res;
  struct timezone tz_res;

  klee_make_symbolic(&tv_res, sizeof tv_res, "gettimeofday_tv");
  klee_make_symbolic(&tz_res, sizeof tz_res, "gettimeofday_tz");

  if (klee_is_symbolic((uintptr_t)tv) || tv)
    *tv = tv_res;

  if (klee_is_symbolic((uintptr_t)tz) || tz)
    *tz = tz_res;

  // FIXME: Technically this can fail if *tv or *tz is OOB, or if *tz is invalid
  return 0;
}

DEFINE_MODEL(int, settimeofday, const struct timeval *tv, const struct timezone *tz) {
  if (tv) {
    uint64_t ktime = tv->tv_sec * 1000000 + tv->tv_usec;
    klee_set_time(ktime);
  }

  if (tz) {
    klee_warning("ignoring timezone set request");
  }

  klee_warning("producing concrete output for settimeofday()");
  return 0;
}

/* XXX why can't I call this internally? */
DEFINE_MODEL(int, clock_gettime, clockid_t clk_id, struct timespec *res) {
  struct timeval tv;
  klee_warning("producing concrete output for clock_gettime()");
  gettimeofday(&tv, NULL);
  res->tv_sec = tv.tv_sec;
  res->tv_nsec = tv.tv_usec * 1000;
  return 0;
}

DEFINE_INPUT_MODEL(time_t, time, time_t *t) {
  time_t res;
  klee_make_symbolic(&res, sizeof res, "time");
  if (klee_is_symbolic((uintptr_t)t) || t)
    *t = res;
  // FIXME: Technically this can fail if *t is OOB
  return res;
}

DEFINE_MODEL(clock_t, times, struct tms *buf) {
  /* Fake */
  klee_warning("producing concrete output for times()");
  buf->tms_utime = 0;
  buf->tms_stime = 0;
  buf->tms_cutime = 0;
  buf->tms_cstime = 0;
  return 0;
}
