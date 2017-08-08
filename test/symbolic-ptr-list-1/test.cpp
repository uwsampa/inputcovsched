//cl: --ics-opt-shortest-path-first
//

#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include "klee/klee.h"

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct LIST {
  int data;
  LIST *next;
};

__attribute__((noinline))
static void traverse(LIST *n, int depth) {
  for (int i = 0; i < depth; ++i) {
    pthread_mutex_lock(&lock);
    pthread_mutex_unlock(&lock);

    if (n == NULL) {
      klee_debug("DEPTH %d: NULL\n", i);
      return;
    }

    klee_debug("DEPTH %d:\n", i);
    klee_print_expr_only(" .. POINTER: ", n);
    klee_print_expr_only(" .. DATA:    ", n->data);
    n = n->next;
    klee_print_expr_only(" .. NEXT:    ", n);
  }

  klee_debug("DEPTH %d: END\n", depth);
}

int main(int argc, char **argv) {
  LIST root;
  klee_make_symbolic(&root, sizeof root, "Root");
  klee_ics_begin();

  traverse(&root, 3);

  // Don't return from the last stack frame so that
  // "root" isn't deallocated when the program exits
  exit(0);
}