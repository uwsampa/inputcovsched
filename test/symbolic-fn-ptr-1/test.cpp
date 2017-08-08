#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include "klee/klee.h"

pthread_mutex_t L[4] = { PTHREAD_MUTEX_INITIALIZER,
                         PTHREAD_MUTEX_INITIALIZER,
                         PTHREAD_MUTEX_INITIALIZER,
                         PTHREAD_MUTEX_INITIALIZER };

typedef void (*FnPtr)(int);

static FnPtr Ptr;
static FnPtr OtherPtr;

__attribute__((noinline))
static void A(int x) {
  klee_debug("ARRIVED @ A: %d\n", x);
  pthread_mutex_lock(&L[0]);
  pthread_mutex_unlock(&L[0]);
}

__attribute__((noinline))
static void B(int x) {
  klee_debug("ARRIVED @ B: %d\n", x);
  pthread_mutex_lock(&L[1]);
  pthread_mutex_unlock(&L[1]);
}

__attribute__((noinline))
static void C(int x) {
  klee_debug("ARRIVED @ C: %d\n", x);
  pthread_mutex_lock(&L[2]);
  pthread_mutex_unlock(&L[2]);
}

__attribute__((noinline))
static void Untouched(int x) {
  klee_debug("ARRIVED @ Untouched: %d\n", x);
  pthread_mutex_lock(&L[3]);
  pthread_mutex_unlock(&L[3]);
}

__attribute__((noinline))
static void Init() {
  Ptr = A;
  klee_debug("A = %p\n", Ptr);

  Ptr = B;
  klee_debug("B = %p\n", Ptr);

  Ptr = C;
  klee_debug("C = %p\n", Ptr);

  OtherPtr = Untouched;
  klee_debug("Untouched = %p\n", OtherPtr);
}

int main(int argc, char **argv) {
  Init();
  klee_make_symbolic(&Ptr, sizeof Ptr, "Ptr");
  klee_ics_begin();

  klee_debug("Calling Ptr(42)\n");
  (*Ptr)(42);
}
