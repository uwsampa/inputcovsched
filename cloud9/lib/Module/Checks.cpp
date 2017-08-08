//===-- Checks.cpp --------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Passes.h"
#include "klee/Internal/Module/KModule.h"

#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/InstrTypes.h"
#include "llvm/Instruction.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/Type.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace klee;

namespace {
  cl::opt<bool>
  DivZeroCheckForFp("div-zero-check-for-fp", cl::init(false));
}

char DivCheckPass::ID;

bool DivCheckPass::runOnModule(Module &M) { 
  Function *divZeroCheckFunction = 0;

  bool moduleChanged = false;
  
  for (Module::iterator f = M.begin(), fe = M.end(); f != fe; ++f) {
    if (ShouldLoweringPassesIgnoreFunction(f))
      continue;
    for (Function::iterator b = f->begin(), be = f->end(); b != be; ++b) {
      for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i) {     
        if (BinaryOperator* binOp = dyn_cast<BinaryOperator>(i)) {
          // find all [s|u|f][div|mod] instructions
          Instruction::BinaryOps opcode = binOp->getOpcode();
          if (opcode != Instruction::SDiv && opcode != Instruction::SRem &&
              opcode != Instruction::UDiv && opcode != Instruction::URem &&
              (!DivZeroCheckForFp ||
               (opcode != Instruction::FDiv && opcode != Instruction::FRem))) {
            continue;
          }

          // skip when denominator is a non-zero constant
          if (Constant *c = dyn_cast<Constant>(i->getOperand(1))) {
            if (!c->isNullValue() && !c->isNegativeZeroValue())
              continue;
          }

          const Type *VoidTy = Type::getVoidTy(getGlobalContext());
          const Type *IntTy = Type::getInt64Ty(getGlobalContext());
          Value *denominator = i->getOperand(1);
          if (denominator->getType() != IntTy) {
            denominator =      // N.B.: signedness doesn't matter
              CastInst::Create(CastInst::getCastOpcode(denominator, false, IntTy, false),
                               denominator,
                               IntTy,
                               "cast_for_div_zero_check",
                               i);
          }

          // Lazily bind the function to avoid always importing it.
          if (!divZeroCheckFunction) {
            Constant *fc = M.getOrInsertFunction("klee_div_zero_check", VoidTy, IntTy, NULL);
            divZeroCheckFunction = cast<Function>(fc);
          }

          CallInst::Create(divZeroCheckFunction, denominator, "", &*i);
          moduleChanged = true;
        }
      }
    }
  }
  return moduleChanged;
}
