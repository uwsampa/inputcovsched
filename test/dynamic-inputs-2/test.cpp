#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include "klee/klee.h"

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Steps of execution:
//   region marker
//   schedule depending on stackvar
//   mutate the stackvar
//   read input
//   schedule depending on input+stackvar
// FIXME:
//   the second schedule fragment depends on input only,
//   not on input+stack var: this is intended to test
//   double-buffering in the runtime system

static int StuffCalls = 0;

__attribute__((noinline))
static void stuff(const int x) {
  if (StuffCalls == 0) {
    klee_ics_region_marker();
    klee_debug("STARTING REGION OF INTEREST\n");
    StuffCalls = 0;
  }
  StuffCalls++;

  klee_print_expr_only("X:", x);
  if (x) {
    pthread_mutex_lock(&lock);
    klee_debug("SYNC\n");
    pthread_mutex_unlock(&lock);
  } else {
    klee_debug("SKIPPED\n");
  }

  if (StuffCalls == 2) {
    klee_debug("EXITING\n");
    exit(0);
  }
}

int main(int argc, char **argv) {
  // Construct the value "2" in a way that fools LLVM's
  // optimizer into thinking that "two" has an unknown
  // value.  This is to prevent loop unrolling.
  int two = klee_range32(2, 3, "two");
  klee_ics_begin();

  for (int i = 0; i < two; ++i) {
    klee_print_expr_only("CUR:", i);
    klee_debug("Reading input X\n");
    int x = klee_int32("x");
    stuff(x);
    // Force the loop to have two iterations
    if (klee_is_symbolic(i))
      i = 0;
  }

  return 0;
}
