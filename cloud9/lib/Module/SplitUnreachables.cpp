//===-- SplitUnreachables.cpp ---------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Passes.h"

#include <set>

using namespace llvm;

char klee::SplitUnreachablesPass::ID = 0;

bool klee::SplitUnreachablesPass::runOnFunction(Function &f) {
  bool changed = false;
  std::set<BasicBlock*> unreachables;
  
  for (Function::iterator bb = f.begin(); bb != f.end(); ++bb) {
    BranchInst *br = dyn_cast<BranchInst>(bb->getTerminator());

    if (br && br->isUnconditional()) {
      BasicBlock *succ = br->getSuccessor(0);
      if (succ->size() == 1 && isa<UnreachableInst>(succ->getTerminator())) {
        unreachables.insert(succ);
        br->eraseFromParent();
        new UnreachableInst(succ->getContext(), bb);
        changed = true;
      }
    }
  }

  for (std::set<BasicBlock*>::iterator
       it = unreachables.begin(); it != unreachables.end(); ++it) {
    if ((*it)->use_empty()) {
      (*it)->eraseFromParent();
      changed = true;
    }
  }

  return changed;
}
