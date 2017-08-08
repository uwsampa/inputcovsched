//===-- IntrinsicCleaner.cpp ----------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Passes.h"
#include "klee/Internal/Module/KModule.h"

#include "../Core/Common.h"
#include "klee/Config/config.h"
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
#include "llvm/ValueSymbolTable.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Target/TargetData.h"

using namespace llvm;

namespace {
  cl::opt<bool>
  UseManualMemFns("intrinsic-cleaner-use-manual-mem-fns", cl::init(false));
}

namespace klee {

char IntrinsicCleanerPass::ID;

/// ReplaceCallWith - This function is used when we want to lower an intrinsic
/// call to a call of an external function.  This handles hard cases such as
/// when there was already a prototype for the external function, and if that
/// prototype doesn't match the arguments we expect to pass in.
template <class ArgIt>
static CallInst *ReplaceCallWith(const char *NewFn, CallInst *CI,
                                 ArgIt ArgBegin, ArgIt ArgEnd,
                                 const Type *RetTy) {
  // If we haven't already looked up this function, check to see if the
  // program already contains a function with this name.
  Module *M = CI->getParent()->getParent()->getParent();
  // Get or insert the definition now.
  std::vector<const Type *> ParamTys;
  for (ArgIt I = ArgBegin; I != ArgEnd; ++I)
    ParamTys.push_back((*I)->getType());
  Constant* FCache = M->getOrInsertFunction(NewFn,
                                  FunctionType::get(RetTy, ParamTys, false));

  IRBuilder<> Builder(CI->getParent(), CI);
  SmallVector<Value *, 8> Args(ArgBegin, ArgEnd);
  CallInst *NewCI = Builder.CreateCall(FCache, Args.begin(), Args.end());
  NewCI->setName(CI->getName());
  if (!CI->use_empty())
    CI->replaceAllUsesWith(NewCI);
  CI->eraseFromParent();
  return NewCI;
}

static void ReplaceFPIntrinsicWithCall(CallInst *CI, const char *Fname,
                                       const char *Dname,
                                       const char *LDname) {
  CallSite CS(CI);
  switch (CI->getArgOperand(0)->getType()->getTypeID()) {
  default: klee_error("Invalid type in intrinsic");
  case Type::FloatTyID:
    ReplaceCallWith(Fname, CI, CS.arg_begin(), CS.arg_end(),
                  Type::getFloatTy(CI->getContext()));
    break;
  case Type::DoubleTyID:
    ReplaceCallWith(Dname, CI, CS.arg_begin(), CS.arg_end(),
                  Type::getDoubleTy(CI->getContext()));
    break;
  case Type::X86_FP80TyID:
  case Type::FP128TyID:
  case Type::PPC_FP128TyID:
    ReplaceCallWith(LDname, CI, CS.arg_begin(), CS.arg_end(),
                  CI->getArgOperand(0)->getType());
    break;
  }
}

static void ReplaceIntIntrinsicWithCall(CallInst *CI, const char *Fname,
                                       const char *Dname,
                                       const char *LDname) {
  CallSite CS(CI);
  switch (CI->getArgOperand(0)->getType()->getTypeID()) {
  default: klee_error("Invalid type in intrinsic");
  case Type::IntegerTyID:
    const IntegerType* itype = (const IntegerType*)CI->getArgOperand(0)->getType();
    const IntegerType* btype = IntegerType::get(CI->getContext(), 1);
    switch(itype->getBitWidth()) {
      case 16:
        ReplaceCallWith(Fname, CI, CS.arg_begin(), CS.arg_end(),
                  StructType::get(CI->getContext(), itype, btype, NULL));
        break;
      case 32:
        ReplaceCallWith(Dname, CI, CS.arg_begin(), CS.arg_end(),
                  StructType::get(CI->getContext(), itype, btype, NULL));
        break;
      case 64:
        ReplaceCallWith(LDname, CI, CS.arg_begin(), CS.arg_end(),
                  StructType::get(CI->getContext(), itype, btype, NULL));
        break;
    }
    break;
  }
}

static void ReplaceMemoryIntrinsicManually(llvm::IntrinsicInst *ii,
                                           BasicBlock::iterator &i,
                                           BasicBlock::iterator &ie) {
  llvm::errs() << "===================== WARNING ==================================\n"
               << "MANUAL MEMORY INTRINSIC!:\n"
               << *ii << "\n";

  LLVMContext &Ctx = ii->getContext();

  Value *dst = ii->getArgOperand(0);
  Value *src = ii->getArgOperand(1);
  Value *len = ii->getArgOperand(2);

  BasicBlock *BB = ii->getParent();
  Function *F = BB->getParent();

  BasicBlock *exitBB = BB->splitBasicBlock(ii);
  BasicBlock *headerBB = BasicBlock::Create(Ctx, Twine(), F, exitBB);
  BasicBlock *bodyBB  = BasicBlock::Create(Ctx, Twine(), F, exitBB);

  // Enter the loop header
  BB->getTerminator()->eraseFromParent();
  BranchInst::Create(headerBB, BB);

  // Create loop index
  PHINode *idx = PHINode::Create(len->getType(), Twine(), headerBB);
  idx->addIncoming(ConstantInt::get(len->getType(), 0), BB);

  // Check loop condition, then move to the loop body or exit the loop
  Value *loopCond = ICmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_ULT,
                                     idx, len, Twine(), headerBB);
  BranchInst::Create(bodyBB, exitBB, loopCond, headerBB);

  // Get value to store
  Value *val;
  if (ii->getIntrinsicID() == Intrinsic::memset) {
    val = src;
  } else {
    Value *srcPtr = GetElementPtrInst::Create(src, idx, Twine(), bodyBB);
    val = new LoadInst(srcPtr, Twine(), bodyBB);
  }

  // Store the value
  Value* dstPtr = GetElementPtrInst::Create(dst, idx, Twine(), bodyBB);
  new StoreInst(val, dstPtr, bodyBB);

  // Update index and branch back
  Value* newIdx = BinaryOperator::Create(Instruction::Add,
              idx, ConstantInt::get(len->getType(), 1), Twine(), bodyBB);
  BranchInst::Create(headerBB, bodyBB);
  idx->addIncoming(newIdx, bodyBB);

  ii->eraseFromParent();

  // Update iterators to continue in the next BB
  i = exitBB->begin();
  ie = exitBB->end();
}

bool IntrinsicCleanerPass::runOnModule(Module &M) {
  bool dirty = false;
  for (Module::iterator f = M.begin(), fe = M.end(); f != fe; ++f) {
    if (ShouldLoweringPassesIgnoreFunction(f))
      continue;
    for (Function::iterator b = f->begin(), be = f->end(); b != be;) {
      dirty |= runOnBasicBlock(*(b++));
    }
  }
  return dirty;
}

bool IntrinsicCleanerPass::runOnBasicBlock(BasicBlock &b) {
  bool dirty = false;

  unsigned WordSize = TargetData.getPointerSizeInBits() / 8;
  for (BasicBlock::iterator i = b.begin(), ie = b.end(); i != ie;) {
    IntrinsicInst *ii = dyn_cast<IntrinsicInst>(&*i);
    // increment now since LowerIntrinsic deletion makes iterator invalid.
    ++i;
    if (ii) {
      CallSite CS(ii);
      switch (ii->getIntrinsicID()) {
      case Intrinsic::vastart:
      case Intrinsic::vaend:
        break;

      // Lower vacopy so that object resolution etc is handled by normal instructions.
      // FIXME: This is much more target dependent than just the word size,
      // however this works for x86-32 and x86-64.
      case Intrinsic::vacopy: { // (dst, src) -> *((i8**) dst) = *((i8**) src)
        Value *dst = ii->getArgOperand(0);
        Value *src = ii->getArgOperand(1);

        if (WordSize == 4) {
          Type *i8pp = PointerType::getUnqual(PointerType::getUnqual(Type::getInt8Ty(getGlobalContext())));
          Value *castedDst = CastInst::CreatePointerCast(dst, i8pp, "vacopy.cast.dst", ii);
          Value *castedSrc = CastInst::CreatePointerCast(src, i8pp, "vacopy.cast.src", ii);
          Value *load = new LoadInst(castedSrc, "vacopy.read", ii);
          new StoreInst(load, castedDst, false, ii);
        } else {
          assert(WordSize == 8 && "Invalid word size!");
          Type *i64p = PointerType::getUnqual(Type::getInt64Ty(getGlobalContext()));
          Value *pDst = CastInst::CreatePointerCast(dst, i64p, "vacopy.cast.dst", ii);
          Value *pSrc = CastInst::CreatePointerCast(src, i64p, "vacopy.cast.src", ii);
          Value *val = new LoadInst(pSrc, std::string(), ii); new StoreInst(val, pDst, ii);
          Value *off = ConstantInt::get(Type::getInt64Ty(getGlobalContext()), 1);
          pDst = GetElementPtrInst::Create(pDst, off, std::string(), ii);
          pSrc = GetElementPtrInst::Create(pSrc, off, std::string(), ii);
          val = new LoadInst(pSrc, std::string(), ii); new StoreInst(val, pDst, ii);
          pDst = GetElementPtrInst::Create(pDst, off, std::string(), ii);
          pSrc = GetElementPtrInst::Create(pSrc, off, std::string(), ii);
          val = new LoadInst(pSrc, std::string(), ii); new StoreInst(val, pDst, ii);
        }
        ii->removeFromParent();
        delete ii;
        dirty = true;
        break;
      }

      // Ignored
      case Intrinsic::dbg_value:
      case Intrinsic::dbg_declare:
      case Intrinsic::memory_barrier:
        break;

      // TODO: don't ignore these
      case Intrinsic::atomic_swap:
      case Intrinsic::atomic_cmp_swap:
      case Intrinsic::atomic_load_add:
      case Intrinsic::atomic_load_and:
      case Intrinsic::atomic_load_max:
      case Intrinsic::atomic_load_min:
      case Intrinsic::atomic_load_nand:
      case Intrinsic::atomic_load_or:
      case Intrinsic::atomic_load_sub:
      case Intrinsic::atomic_load_umax:
      case Intrinsic::atomic_load_umin:
      case Intrinsic::atomic_load_xor:
        llvm::errs() << "WARNING: skipping atomic in " << b.getParent()->getName() << "\n";
        break;

      case Intrinsic::powi:
        ReplaceFPIntrinsicWithCall(ii, "powif", "powi", "powil");
        dirty = true;
        break;

      case Intrinsic::uadd_with_overflow:
        ReplaceIntIntrinsicWithCall(ii, "uadds", "uadd", "uaddl");
        dirty = true;
        break;

      case Intrinsic::trap:
        // Link with klee_abort
        // N.B.: This takes no args so we pass an empty arg list
        ReplaceCallWith("klee_abort", ii, CS.arg_end(), CS.arg_end(),
                        Type::getVoidTy(getGlobalContext()));
        dirty = true;
        break;

      case Intrinsic::memset:
      case Intrinsic::memcpy:
      case Intrinsic::memmove:
        if (UseManualMemFns) {
          ReplaceMemoryIntrinsicManually(ii, i, ie);
        } else {
          switch (ii->getIntrinsicID()) {
          case Intrinsic::memset:
            ReplaceCallWith("klee_memset", ii, CS.arg_begin(), CS.arg_begin()+3,
                            Type::getVoidTy(getGlobalContext()));
            break;
          case Intrinsic::memcpy:
          case Intrinsic::memmove:
            ReplaceCallWith("klee_memcpy", ii, CS.arg_begin(), CS.arg_begin()+3,
                            Type::getVoidTy(getGlobalContext()));
            break;
          default:
            assert(0);
            break;
          }
        }
        dirty = true;
        break;

      default:
        if (UseLlvmLowerIntrinsics)
          IL->LowerIntrinsicCall(ii);
        dirty = true;
        break;
      }
    }
  }

  return dirty;
}

}  // namespace klee
