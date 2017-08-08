//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Recognize common standard c library functions and generate graphs for them
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "dsa-stdlib"

#include "llvm/ADT/Statistic.h"
#include "dsa/DataStructure.h"
#include "dsa/AllocatorIdentification.h"
#include "dsa/DSGraph.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Timer.h"
#include <iostream>
#include "llvm/Module.h"

using namespace llvm;

static RegisterPass<StdLibDataStructures>
X("dsa-stdlib", "Standard Library Local Data Structure Analysis");

STATISTIC(NumNodesFoldedInStdLib,    "Number of nodes folded in std lib");

char StdLibDataStructures::ID;

namespace {
  static cl::opt<bool> noStdLibFold("dsa-stdlib-no-fold",
         cl::desc("Don't fold nodes in std-lib."),
         cl::Hidden,
         cl::init(false));
  static cl::opt<bool> DisableStdLib("disable-dsa-stdlib",
         cl::desc("Don't use DSA's stdlib pass."),
         cl::Hidden,
         cl::init(false));
  static cl::opt<bool> DisableAllocIdentify("disable-dsa-allocindentify",
         cl::desc("Don't use DSA's allocidentify pass along with stdlib."),
         cl::Hidden,
         cl::init(true));
}


//
// Method: eraseCallsTo()
//
// Description:
//  This method removes the specified function from DSCallsites within the
//  specified function.  We do not do anything with call sites that call this
//  function indirectly (for which there is not much point as we do not yet
//  know the targets of indirect function calls).
//
void
StdLibDataStructures::eraseCallsTo(Function* F) {
  for (Value::use_iterator ii = F->use_begin(), ee = F->use_end();
       ii != ee; ++ii)
    if (isa<CallInst>(*ii) || isa<InvokeInst>(*ii)) {
      CallSite CS(*ii);
      if (CS.getCalledValue() == F) {
        DSGraph* Graph = getDSGraph(*CS.getCaller());
        //delete the call
        DEBUG(errs() << "Removing " << F->getNameStr() << " from "
                     << CS.getCaller()->getNameStr() << "\n");
        Graph->removeFunctionCalls(*F);
        callgraph.erase(CS, F);
      }
    } else if(ConstantExpr *CE = dyn_cast<ConstantExpr>(*ii)) {
      if(CE->isCast()) {
        for (Value::use_iterator ci = CE->use_begin(), ce = CE->use_end();
             ci != ce; ++ci) {
          if (isa<CallInst>(*ci) || isa<InvokeInst>(*ci)) {
            CallSite CS(*ci);
            if(CS.getCalledValue() == CE) {
              DSGraph* Graph = getDSGraph(*CS.getCaller());
              //delete the call
              DEBUG(errs() << "Removing " << F->getNameStr() << " from "
                           << CS.getCaller()->getNameStr() << "\n");
              Graph->removeFunctionCalls(*F);
              callgraph.erase(CS, F);
            }
          }
        }
      }
    }
}

bool
StdLibDataStructures::runOnModule(Module &M) {
  stdLibInfo.initialize(M);

  //
  // Get the results from the local pass.
  //
  init (&getAnalysis<LocalDataStructures>(), true, true, false, false);

  if (!DisableAllocIdentify)
    AllocWrappersAnalysis = &getAnalysis<AllocIdentify>();

  //
  // Fetch the DSGraphs for all defined functions within the module.
  //
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) 
    if (!I->isDeclaration())
      getOrCreateGraph(&*I);

  //
  // Erase direct calls to functions that don't return a pointer and are marked
  // with the readnone annotation.
  //
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) 
    if (I->isDeclaration() && I->doesNotAccessMemory() &&
        !isa<PointerType>(I->getReturnType()))
      eraseCallsTo(I);

  //
  // Erase direct calls to external functions that are not varargs, do not
  // return a pointer, and do not take pointers.
  //
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) 
    if (I->isDeclaration() && !I->isVarArg() &&
        !isa<PointerType>(I->getReturnType())) {
      bool hasPtr = false;
      for (Function::arg_iterator ii = I->arg_begin(), ee = I->arg_end();
           ii != ee;
           ++ii)
        if (isa<PointerType>(ii->getType())) {
          hasPtr = true;
          break;
        }
      if (!hasPtr)
        eraseCallsTo(I);
    }

  if (!DisableStdLib) {
    //
    // Scan through the function summaries and process functions by summary.
    //
    for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
      if (I->isDeclaration()) {
        const StdLibInfo::LibAction* action = stdLibInfo.getLibActionForFunction(I);
        if (action) 
          processFunction(I, *action);
      }
    }
  }

  if (!DisableStdLib && !DisableAllocIdentify) {
    std::set<std::string>::iterator ai = AllocWrappersAnalysis->alloc_begin();
    std::set<std::string>::iterator ae = AllocWrappersAnalysis->alloc_end();

    const StdLibInfo::LibAction *mallocAction =
      stdLibInfo.getLibActionForFunctionName("malloc");
    assert(mallocAction);

    for (; ai != ae; ++ai) {
      if (Function* F = M.getFunction(*ai))
        processFunction(F, *mallocAction);
    }

    ai = AllocWrappersAnalysis->dealloc_begin();
    ae = AllocWrappersAnalysis->dealloc_end();

    const StdLibInfo::LibAction *freeAction =
      stdLibInfo.getLibActionForFunctionName("free");
    assert(freeAction);

    for (; ai != ae; ++ai) {
      if (Function* F = M.getFunction(*ai))
        processFunction(F, *freeAction);
    }
  }

  //
  // In the Local DSA Pass, we marked nodes passed to/returned from 'StdLib'
  // functions as External because, at that point, they were.  However, they no
  // longer are necessarily External, and we need to update accordingly.
  //
  GlobalsGraph->maskIncompleteMarkers();

  GlobalsGraph->computeExternalFlags(DSGraph::ResetExternal);
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
    if (!I->isDeclaration()) {
      DSGraph * G = getDSGraph(*I);
      unsigned EFlags = 0
        | DSGraph::ResetExternal
        | DSGraph::DontMarkFormalsExternal
        | DSGraph::ProcessCallSites;
      G->maskIncompleteMarkers();
      G->markIncompleteNodes(DSGraph::MarkFormalArgs
                             |DSGraph::IgnoreGlobals);
      G->computeExternalFlags(EFlags);
      DEBUG(G->AssertGraphOK());
    }

  GlobalsGraph->markIncompleteNodes(DSGraph::MarkFormalArgs
                                    |DSGraph::IgnoreGlobals);
  GlobalsGraph->computeExternalFlags(DSGraph::ProcessCallSites);
  DEBUG(GlobalsGraph->AssertGraphOK());

  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
    if (!I->isDeclaration()) {
      DSGraph *Graph = getOrCreateGraph(I);
      Graph->maskIncompleteMarkers();
      cloneGlobalsInto(Graph, DSGraph::DontCloneCallNodes |
                       DSGraph::DontCloneAuxCallNodes);
      Graph->markIncompleteNodes(DSGraph::MarkFormalArgs
                                 |DSGraph::IgnoreGlobals);
    }

  return false;
}

void StdLibDataStructures::processFunction(Function *F,
                                           const StdLibInfo::LibAction &action) {
  //
  // Process all call sites.
  //
  for (Value::use_iterator ii = F->use_begin(); ii != F->use_end(); ++ii) {
    if (isa<CallInst>(*ii) || isa<InvokeInst>(*ii)) {
      CallSite CS(*ii);
      if (CS.getCalledValue() == F) {
        processCallSite(CS, action);
      }
    } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(*ii)) {
      if (CE->isCast()) {
        for (Value::use_iterator ci = CE->use_begin(); ci != CE->use_end(); ++ci) {
          if (isa<CallInst>(*ci) || isa<InvokeInst>(*ci)) {
            CallSite CS(*ci);
            if(CS.getCalledValue() == CE)
              processCallSite(CS, action);
          }
        }
      }
    }
  }

  //
  // Pretend that calls to this function do not exist anymore.
  //
  eraseCallsTo(F);
}

void StdLibDataStructures::processCallSite(CallSite CS,
                                           const StdLibInfo::LibAction &action) {
  Instruction* CI = CS.getInstruction();
  DSGraph* Graph = getDSGraph(*CS.getCaller());

  CallSite::arg_iterator arg;
  unsigned y;

  //
  // Set the read, write, and heap markers on the return value
  // as appropriate.
  //
  if (isa<PointerType>(CI->getType()) && Graph->hasNodeForValue(CI)) {
    if (action.read[0])
      Graph->getNodeForValue(CI).getNode()->setReadMarker();
    if (action.write[0])
      Graph->getNodeForValue(CI).getNode()->setModifiedMarker();
    if (action.heap[0])
      Graph->getNodeForValue(CI).getNode()->setHeapMarker();
  }

  //
  // Set the read, write, and heap markers on the actual arguments
  // as appropriate.
  //
  for (y = 1, arg = CS.arg_begin(); arg != CS.arg_end(); ++arg, ++y) {
    if (isa<PointerType>((*arg)->getType()) && Graph->hasNodeForValue(*arg)) {
      if (action.read[y])
        Graph->getNodeForValue(*arg).getNode()->setReadMarker();
      if (action.write[y])
        Graph->getNodeForValue(*arg).getNode()->setModifiedMarker();
      if (action.heap[y])
        Graph->getNodeForValue(*arg).getNode()->setHeapMarker();
    }
  }

  //
  // Merge the DSNoes for return values and parameters as
  // appropriate.
  //
  std::vector<DSNodeHandle> toMerge;
  if (action.mergeNodes[0]) {
    if (isa<PointerType>(CI->getType()) && Graph->hasNodeForValue(CI))
      toMerge.push_back(Graph->getNodeForValue(CI));
  }

  for (y = 1, arg = CS.arg_begin(); arg != CS.arg_end(); ++arg, ++y) {
    if (action.mergeNodes[y]) {
      if (isa<PointerType>((*arg)->getType()) && Graph->hasNodeForValue(*arg))
        toMerge.push_back(Graph->getNodeForValue(*arg));
    }
  }

  for (unsigned y = 1; y < toMerge.size(); ++y)
    toMerge[0].mergeWith(toMerge[y]);

  //
  // Collapse (fold) the DSNode of the return value and the actual
  // arguments if directed to do so.
  //
  if (!noStdLibFold && action.collapse) {
    if (isa<PointerType>(CI->getType()) && Graph->hasNodeForValue(CI)) {
      Graph->getNodeForValue(CI).getNode()->foldNodeCompletely();
      NumNodesFoldedInStdLib++;
    }
    for (y = 1, arg = CS.arg_begin(); arg != CS.arg_end(); ++arg, ++y) {
      if (isa<PointerType>((*arg)->getType()) && Graph->hasNodeForValue(*arg)) {
        Graph->getNodeForValue(*arg).getNode()->foldNodeCompletely();
        NumNodesFoldedInStdLib++;
      }
    }
  }

  //
  // Process special thread creation functions
  // For each call, we create a dummy callsite for the new thread's start routine
  //
  if (action.threadFnArg >= 0) {
    assert(action.threadFnParamArg >= 0);

    DEBUG(errs() << "Thread creation site: " << *CS.getInstruction() << "\n");
    DSNodeHandle RunFnNode = Graph->getNodeForValue(CS.getArgument(action.threadFnArg));
    DSNodeHandle RunArgNode = Graph->getNodeForValue(CS.getArgument(action.threadFnParamArg));

    std::vector<DSNodeHandle> args;
    args.push_back(RunArgNode);
    Graph->getCreateThreadCalls().push_back(
        DSCallSite(CS, NULL /* no retval */, NULL /* no varargs */,
                   RunFnNode.getNode(), args));

  }
}
