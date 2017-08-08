/*
 * POSIX malloc wrapper
 * Author: Tom Bergan
 *
 */

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/user.h>
#include <unistd.h>

#include "common.h"

FORCE_LINKAGE(malloc)

////////////////////////////////////////////////////////////////////////////////
// Utils
////////////////////////////////////////////////////////////////////////////////

static inline int IsPow2(size_t x) {
  return (x & (x - 1)) == 0;
}

enum {
  DontCareAlignment = 0,
  NoZeroInit = 0,
  ZeroInit = 1
};

////////////////////////////////////////////////////////////////////////////////
// LibC Memory allocation
////////////////////////////////////////////////////////////////////////////////

// Alloc

DEFINE_MODEL(void*, malloc, size_t size) {
  return klee_malloc(size, NULL, NoZeroInit, DontCareAlignment);
}

DEFINE_MODEL(void*, calloc, size_t numelem, size_t size) {
  return klee_malloc(numelem*size, NULL, ZeroInit, DontCareAlignment);
}

DEFINE_MODEL(void*, realloc, void *ptr, size_t size) {
  return klee_malloc(size, ptr, NoZeroInit, DontCareAlignment);
}

DEFINE_MODEL(void*, memalign, size_t alignment, size_t size) {
  void *ptr = klee_malloc(size, NULL, NoZeroInit, alignment);
  return (void*)klee_expr_select64(IsPow2(alignment), (uintptr_t)ptr, (uintptr_t)0);
}

DEFINE_MODEL(int, posix_memalign, void **memptr, size_t alignment, size_t size) {
  void *ptr = klee_malloc(size, NULL, NoZeroInit, alignment);
  *memptr = ptr;
  return klee_expr_select32(!IsPow2(alignment), EINVAL,
         klee_expr_select32(ptr == NULL, ENOMEM, 0));
}

DEFINE_MODEL(void*, valloc, size_t size) {
  return memalign(PAGE_SIZE, size);
}

// Free

DEFINE_MODEL(void, free, void *ptr) {
  klee_free(ptr);
}

// Misc

// TODO: symbolic
DEFINE_MODEL(struct mallinfo, mallinfo, void) {
  struct mallinfo info;
  memset(&info, 0, sizeof info);
  return info;
}

DEFINE_MODEL(int, malloc_trim, size_t pad) {
  return 0;
}

DEFINE_MODEL(void, malloc_stats, FILE *file) {
  // nop
}

DEFINE_MODEL(int, mallopt, int param, int val) {
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// LibC++ Memory allocation
////////////////////////////////////////////////////////////////////////////////

typedef struct St9nothrow_t nothrow_t;

// operator new

DEFINE_MODEL(void*, _Znwj, unsigned size) {
  return klee_malloc(size, NULL, NoZeroInit, DontCareAlignment);
}
DEFINE_MODEL(void*, _ZnwjRKnothrow_t, unsigned size, const nothrow_t *nt) {
  return klee_malloc(size, NULL, NoZeroInit, DontCareAlignment);
}
DEFINE_MODEL(void*, _Znwm, unsigned long size) {
  return klee_malloc(size, NULL, NoZeroInit, DontCareAlignment);
}
DEFINE_MODEL(void*, _ZnwmRKnothrow_t, unsigned long size, const nothrow_t *nt) {
  return klee_malloc(size, NULL, NoZeroInit, DontCareAlignment);
}

// operator new[]

DEFINE_MODEL(void*, _Znaj, unsigned size) {
  return klee_malloc(size, NULL, NoZeroInit, DontCareAlignment);
}
DEFINE_MODEL(void*, _ZnajRKnothrow_t, unsigned size, const nothrow_t *nt) {
  return klee_malloc(size, NULL, NoZeroInit, DontCareAlignment);
}
DEFINE_MODEL(void*, _Znam, unsigned long size) {
  return klee_malloc(size, NULL, NoZeroInit, DontCareAlignment);
}
DEFINE_MODEL(void*, _ZnamRKnothrow_t, unsigned long size, const nothrow_t *nt) {
  return klee_malloc(size, NULL, NoZeroInit, DontCareAlignment);
}

// operator delete

DEFINE_MODEL(void, _ZdlPv, void *ptr) {
  klee_free(ptr);
}
DEFINE_MODEL(void, _ZdlPvRKnothrow_t, void *ptr, const nothrow_t *nt) {
  klee_free(ptr);
}

// operator delete[]

DEFINE_MODEL(void, _ZdaPv, void *ptr) {
  klee_free(ptr);
}
DEFINE_MODEL(void, _ZdaPvRKnothrow_t, void *ptr, const nothrow_t *nt) {
  klee_free(ptr);
}

////////////////////////////////////////////////////////////////////////////////
// LibC String/Memory Routines
////////////////////////////////////////////////////////////////////////////////

DEFINE_MODEL(void*, memset, void *p, int c, size_t nbytes) {
  klee_memset(p, (char)c, nbytes);
  return p;
}

DEFINE_MODEL(void, bzero, void *p, size_t nbytes) {
  klee_memset(p, 0, nbytes);
}

DEFINE_MODEL(void*, memcpy, void *dst, const void *src, size_t nbytes) {
  klee_memcpy(dst, src, nbytes);
  return dst;
}

DEFINE_MODEL(void*, mempcpy, void *dst, const void *src, size_t nbytes) {
  klee_memcpy(dst, src, nbytes);
  return (char*)dst + nbytes;
}

DEFINE_MODEL(void*, memmove, void *dst, const void *src, size_t nbytes) {
  klee_memmove(dst, src, nbytes);
  return dst;
}

DEFINE_MODEL(void, bcopy, void *dst, const void *src, size_t nbytes) {
  klee_memcpy(dst, src, nbytes);
}

DEFINE_MODEL(char*, strdup, const char *src) {
  return klee_strdup(src, (size_t)-1);
}

DEFINE_MODEL(char*, strndup, const char *src, size_t nbytes) {
  return klee_strdup(src, nbytes);
}
