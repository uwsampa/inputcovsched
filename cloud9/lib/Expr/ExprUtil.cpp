//===-- ExprUtil.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/util/ExprUtil.h"
#include "klee/util/ExprHashMap.h"

#include "klee/Expr.h"

#include "klee/util/ExprVisitor.h"

#include "llvm/ADT/DenseSet.h"
#include <set>

namespace klee {

namespace {
typedef std::vector< ref<Expr> > ExprStack;
inline void addToStack(ExprStack *stack, ExprHashSet *visited, const ref<Expr> &expr) {
  if (expr.get() && !isa<ConstantExpr>(expr) && visited->insert(expr).second)
    stack->push_back(expr);
}
inline void addToStack(ExprStack *stack, ExprHashSet *visited, const Array *array, const ref<Expr> &index) {
  if (array->type == Array::ConstrainedInitialValues)
    addToStack(stack, visited, array->getValueAtIndex(index));
}
}

void findReads(ref<Expr> expr,
               bool visitUpdates,
               Expr::CachedReadsTy &results) {
  // Invariant: \forall_{i \in stack} !i.isConstant() && i \in visited
  ExprStack stack;
  ExprHashSet visited;
  std::set<const UpdateNode *> updates;

  if (isa<ConstantExpr>(expr))
    return;

  Expr::CachedReadsTy *collector = NULL;

  // Use Expr::cachedReads only when visiting updates
  if (visitUpdates) {
    if (expr->cachedReads) {
      results.insert(results.end(), expr->cachedReads->begin(), expr->cachedReads->end());
      return;
    }
    expr->cachedReads = new Expr::CachedReadsTy;
    collector = expr->cachedReads;
  } else {
    collector = &results;
  }

  // Uncached: recompute
  visited.insert(expr);
  stack.push_back(expr);

  while (!stack.empty()) {
    ref<Expr> top = stack.back();
    stack.pop_back();
    // XXX: can probably check top->cachedReads here when visitUpdate && top != expr

    if (ReadExpr *re = dyn_cast<ReadExpr>(top)) {
      // We memoized so can just add to list without worrying about repeats.
      collector->push_back(re);
      addToStack(&stack, &visited, re->index);

      if (visitUpdates) {
        // Each read of a "constrained" Array might involve an implicit
        // read of other arrays (to eval the constraint), so add those here.
        addToStack(&stack, &visited, re->updates.root, re->index);
        // XXX this is probably suboptimal. We want to avoid a potential
        // explosion traversing update lists which can be quite
        // long. However, it seems silly to hash all of the update nodes
        // especially since we memoize all the expr results anyway. So
        // we take a simple approach of memoizing the results for the
        // head, which often will be shared among multiple nodes.
        if (updates.insert(re->updates.head).second) {
          for (const UpdateNode *un=re->updates.head; un; un=un->next) {
            addToStack(&stack, &visited, un->getGuard());
            if (un->isSingleByte()) {
              addToStack(&stack, &visited, un->getByteIndex());
              addToStack(&stack, &visited, un->getByteValue());
            } else {
              addToStack(&stack, &visited, un->getSrcArray(), re->index);
            }
          }
        }
      }
    } else if (!isa<ConstantExpr>(top)) {
      Expr *e = top.get();
      for (unsigned i=0; i<e->getNumKids(); i++)
        addToStack(&stack, &visited, e->getKid(i));
    }
  }

  // Output results
  if (collector != &results) {
    results.insert(results.end(), collector->begin(), collector->end());
  }
}

///

template<typename InputIterator>
void findSymbolicObjects(InputIterator begin,
                         InputIterator end,
                         std::set<const Array*> &results) {
  // Use findReads() since it is cached
  for (; begin!=end; ++begin) {
    const ref<Expr> &expr = *begin;
    if (isa<ConstantExpr>(expr))
      continue;
    if (!expr->cachedReads) {
      Expr::CachedReadsTy tmp;
      findReads(*begin, true, tmp);
      assert(expr->cachedReads);
    }
    for (size_t k = 0; k < expr->cachedReads->size(); ++k) {
      const Array *array = (*expr->cachedReads)[k]->updates.root;
      if (array->isSymbolicArray())
        results.insert(array);
    }
  }
}

template<typename InputIterator>
void findSymbolicObjects(InputIterator begin,
                         InputIterator end,
                         std::vector<const Array*> &results) {
  std::set<const Array*> tmp;
  findSymbolicObjects(begin, end, tmp);

  results.reserve(tmp.size());
  for (std::set<const Array*>::iterator it = tmp.begin(); it != tmp.end(); ++it)
    results.push_back(*it);
}

template<typename OutputSet>
void findSymbolicObjects(ref<Expr> e,
                         OutputSet &results) {
  findSymbolicObjects(&e, &e+1, results);
}

typedef std::vector< ref<Expr> >::iterator A;
template void findSymbolicObjects<A>(A, A, std::set<const Array*> &);
template void findSymbolicObjects<A>(A, A, std::vector<const Array*> &);

typedef std::set< ref<Expr> >::iterator B;
template void findSymbolicObjects<B>(B, B, std::set<const Array*> &);
template void findSymbolicObjects<B>(B, B, std::vector<const Array*> &);

template void findSymbolicObjects(ref<Expr> e, std::set<const Array*> &results);
template void findSymbolicObjects(ref<Expr> e, std::vector<const Array*> &results);

template<typename InputIterator>
bool usesSymbolicObjects(InputIterator begin,
                         InputIterator end,
                         const std::set<const Array*> &used) {
  // Use findReads() since it is cached
  for (; begin!=end; ++begin) {
    const ref<Expr> &expr = *begin;
    if (isa<ConstantExpr>(expr))
      continue;
    if (!expr->cachedReads) {
      Expr::CachedReadsTy tmp;
      findReads(*begin, true, tmp);
      assert(expr->cachedReads);
    }
    for (size_t k = 0; k < expr->cachedReads->size(); ++k) {
      const Array *array = (*expr->cachedReads)[k]->updates.root;
      if (used.count(array))
        return true;
    }
  }
  return false;
}

bool usesSymbolicObjects(ref<Expr> e,
                         const std::set<const Array*> &used) {
  return usesSymbolicObjects(&e, &e+1, used);
}

}  // namespace klee
