//cl: --ics-do-instrumentor=false
//

#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include "klee/klee.h"

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

enum { NKids = 10 };

struct TREE {
  int data;
  TREE* kids[NKids];
};

__attribute__((noinline))
static void traverse(TREE *n, int depth, int index) {
  for (int i = 0; i < depth; ++i) {
    pthread_mutex_lock(&lock);
    pthread_mutex_unlock(&lock);

    if (n == NULL) {
      klee_debug("DEPTH %d: NULL\n", i);
      return;
    }

    klee_debug("DEPTH %d:\n", i);
    //klee_print_expr_only(" .. POINTER: ", n);
    //klee_print_expr_only(" .. DATA:    ", n->data);
    n = n->kids[index];
  }

  klee_debug("DEPTH %d: END\n", depth);
}

int main(int argc, char **argv) {
  TREE root;
  int index;

  klee_make_symbolic(&root, sizeof root, "Root");
  index = klee_range32(0, NKids, "Index");
  klee_ics_begin();

  traverse(&root, 3, index);

  // Don't return from the last stack frame so that
  // "root" isn't deallocated when the program exits
  exit(0);
}
