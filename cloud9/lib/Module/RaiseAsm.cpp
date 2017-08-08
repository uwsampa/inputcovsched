//===-- RaiseAsm.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Passes.h"
#include "klee/Internal/Module/KModule.h"

#include "llvm/InlineAsm.h"
#include "llvm/LLVMContext.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetRegistry.h"

using namespace llvm;
using namespace klee;

namespace {
  cl::opt<bool>
  WarnRaiseAsm("warn-raise-asm",
               cl::desc("Warn each time an asm stmt is raised to an intrinsic."));

}
char RaiseAsmPass::ID = 0;

Function *RaiseAsmPass::getIntrinsic(llvm::Module &M,
                                     unsigned IID,
                                     const Type **Tys,
                                     unsigned NumTys) {  
  return Intrinsic::getDeclaration(&M, (llvm::Intrinsic::ID) IID, Tys, NumTys);
}

// FIXME: This should just be implemented as a patch to
// X86TargetAsmInfo.cpp, then everyone will benefit.
bool RaiseAsmPass::runOnInstruction(Module &M, Instruction *I) {
  if (CallInst *ci = dyn_cast<CallInst>(I)) {
    if (InlineAsm *ia = dyn_cast<InlineAsm>(ci->getCalledValue())) {
      // Added verification for empty assemply line
      // Used as memory barrier in LLVM, not needed in Klee
      // XXX: This is broken, as not all empty strings are nops
      // XXX: e.g., the libc syscall() functions set the values
      // XXX: of registers to pass args to the actual syscall.
#if 0
      const std::string &as = ia->getAsmString();
      if (as.length() == 0) {
        if (WarnRaiseAsm) {
          llvm::errs() << "Removing empty asm stmt\n"
                       << "  Func: " << I->getParent()->getParent()->getName() << "\n"
                       << "  Inst: " << *I << "\n";
        }
        I->eraseFromParent();
        return true;
      }
#endif
      BasicBlock *bb = I->getParent();
      if (WarnRaiseAsm) {
        std::string str = ia->getAsmString() + " :: " + ia->getConstraintString();
        std::remove_if(str.begin(), str.end(), std::bind1st(std::equal_to<char>(), '\n'));
        AsmStrings.insert(str);
        llvm::errs() << "Expanding inline asm stmt\n"
                     << "  Func: " << I->getParent()->getParent()->getName() << "\n"
                     << "  Inst: " << *I << "\n";
      }
      bool changed = TLI && TLI->ExpandInlineAsm(ci);
      if (WarnRaiseAsm) {
        if (changed) {
          llvm::errs() << "  NewBB:\n" << *bb << "\n";
        } else {
          llvm::errs() << "  No Change\n";
        }
      }
      return changed;
    }
  }

  return false;
}

bool RaiseAsmPass::runOnModule(Module &M) {
  bool changed = false;
  
  std::string Err;
  std::string HostTriple = llvm::sys::getHostTriple();
  const Target *NativeTarget = TargetRegistry::lookupTarget(HostTriple, Err);
  if (NativeTarget == 0) {
    llvm::errs() << "Warning: unable to select native target: " << Err << "\n";
    TLI = 0;
  } else {
    TargetMachine *TM = NativeTarget->createTargetMachine(HostTriple, "");
    TLI = TM->getTargetLowering();
  }

  for (Module::iterator fi = M.begin(), fe = M.end(); fi != fe; ++fi) {
    if (ShouldLoweringPassesIgnoreFunction(fi))
      continue;
    for (Function::iterator bi = fi->begin(), be = fi->end(); bi != be; ++bi) {
      for (BasicBlock::iterator ii = bi->begin(), ie = bi->end(); ii != ie;) {
        Instruction *i = ii;
        ++ii;  
        changed |= runOnInstruction(M, i);
      }
    }
  }

  if (WarnRaiseAsm && !AsmStrings.empty()) {
    llvm::errs() << "RaiseAsm found these strings:\n";
    for (std::set<std::string>::iterator it = AsmStrings.begin(); it != AsmStrings.end(); ++it) {
      llvm::errs() << "  " << *it << "\n";
    }
  }

  return changed;
}
