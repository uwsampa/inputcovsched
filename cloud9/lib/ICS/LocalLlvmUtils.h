/*
 * This is dumb:
 * Stuff in this file is copied from LLVM's src because, for
 * some unknown reason, LLVM has these hidden in .cpp files
 * rather than exporting them to the user.
 */

#include "llvm/Analysis/CFGPrinter.h"

// Copied from: llvm-src/lib/Analysis/DomPrinter.cpp
namespace llvm {
template<>
struct DOTGraphTraits<DomTreeNode*> : public DefaultDOTGraphTraits {

  DOTGraphTraits (bool isSimple=false)
    : DefaultDOTGraphTraits(isSimple) {}

  std::string getNodeLabel(DomTreeNode *Node, DomTreeNode *Graph) {

    BasicBlock *BB = Node->getBlock();

    if (!BB)
      return "Post dominance root node";


    if (isSimple())
      return DOTGraphTraits<const Function*>
        ::getSimpleNodeLabel(BB, BB->getParent());
    else
      return DOTGraphTraits<const Function*>
        ::getCompleteNodeLabel(BB, BB->getParent());
  }
};
}
