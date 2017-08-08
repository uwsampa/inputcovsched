//===-- KModule.cpp -------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// FIXME: This does not belong here.
#include "../Core/Common.h"

#include "klee/Internal/Module/KModule.h"

#include "Passes.h"

#include "klee/Interpreter.h"
#include "klee/Statistics.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Support/ModuleUtil.h"

#include "ics/SearchStrategy.h"

#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Instructions.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/ValueSymbolTable.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/Path.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"

#include <sstream>
#include <fstream>
#include <string>
#include <cstdlib>

using namespace llvm;

using llvm::sys::Path;

namespace {
  enum SwitchImplType {
    eSwitchTypeSimple,
    eSwitchTypeLLVM,
    eSwitchTypeInternal
  };

  cl::list<std::string>
  MergeAtExit("merge-at-exit");
    
  cl::opt<bool>
  NoTruncateSourceLines("no-truncate-source-lines",
                        cl::desc("Don't truncate long lines in the output source"));

  cl::opt<bool>
  OutputSource("output-source",
               cl::desc("Write the assembly for the final transformed source"),
               cl::init(true));

  cl::opt<bool>
  OutputModule("output-module",
               cl::desc("Write the bitcode for the final transformed module"),
               cl::init(false));

  cl::opt<SwitchImplType>
  SwitchType("switch-type", cl::desc("Select the implementation of switch"),
             cl::values(clEnumValN(eSwitchTypeSimple, "simple", 
                                   "lower to ordered branches"),
                        clEnumValN(eSwitchTypeLLVM, "llvm", 
                                   "lower using LLVM"),
                        clEnumValN(eSwitchTypeInternal, "internal", 
                                   "execute switch internally"),
                        clEnumValEnd),
             cl::init(eSwitchTypeInternal));
  
  cl::opt<bool>
  DebugPrintEscapingFunctions("debug-print-escaping-functions",
                              cl::desc("Print functions whose address is taken."));

  cl::opt<std::string>
  VulnerableSites("vulnerable-sites",
      cl::desc("A file describing vulnerable sites in the tested program"));

  cl::opt<std::string>
  CoverableModules("coverable-modules",
      cl::desc("A file containing the list of source files to check for coverage"));

  cl::opt<std::string>
  InitialCoverage("initial-coverage",
      cl::desc("A file containing initial coverage values"));

  cl::list<std::string>
  LoweringIgnores("lowering-ignores");
}

namespace klee {

void KModule::readVulnerablePoints(std::istream &is) {
  std::string fnName;
  std::string callSite;

  while (!is.eof()) {
    is >> fnName >> callSite;

    if (is.eof() && (fnName.length() == 0 || callSite.length() == 0))
      break;

    size_t splitPoint = callSite.find(':');
    assert(splitPoint != std::string::npos);

    std::string fileName = callSite.substr(0, splitPoint);
    std::string lineNoStr = callSite.substr(splitPoint+1);
    unsigned lineNo = atoi(lineNoStr.c_str());

    if (lineNo == 0) {
      // Skipping this
      continue;

    }

    vulnerablePoints[fnName].insert(std::make_pair(fileName, lineNo));
  }
}

bool KModule::isVulnerablePoint(KInstruction *kinst) {
  Function *target;

  if (CallInst *inst = dyn_cast<CallInst>(kinst->inst)) {
    target = inst->getCalledFunction();
  } else if (InvokeInst *inst = dyn_cast<InvokeInst>(kinst->inst)) {
    target = inst->getCalledFunction();
  } else {
    assert(false);
  }

  if (!target)
    return false;

  std::string targetName = target->getNameStr();

  if (targetName.find("__klee_model_") == 0)
    targetName = targetName.substr(strlen("__klee_model_"));

  if (vulnerablePoints.count(target->getNameStr()) == 0)
    return false;

  Path sourceFile(kinst->info->file);
  program_point_t cpoint = std::make_pair(llvm::sys::path::filename(StringRef(sourceFile.str())), kinst->info->line);

  if (vulnerablePoints[target->getNameStr()].count(cpoint) == 0)
    return false;

  return true;
}

void KModule::readCoverableFiles(std::istream &is) {
  std::string name;

  while (!is.eof()) {
    is >> name;

    if (is.eof() && name.length() == 0)
      break;

    if (name.length() == 1) {
      if (name == "S") {
        is >> name;
        coverableFiles.insert(name);
      } else if (name == "F") {
        is >> name;
        exceptedFunctions.insert(name);
      } else {
        coverableFiles.insert(name);
      }
    } else {
      coverableFiles.insert(name);
    }
  }
}

bool KModule::isFunctionCoverable(KFunction *kf) {
  Path fileName;
  std::string fileNameStr;

  for (unsigned int i = 0; i < kf->numInstructions; i++) {
    if (kf->instructions[i]->info->file.empty())
      continue;

    fileName = Path(kf->instructions[i]->info->file);
  }

  if (fileName.isEmpty())
    return false;

  if (exceptedFunctions.count(kf->function->getNameStr()) > 0)
    return false;

  fileNameStr = std::string(fileName.c_str());

  for (cov_list_t::iterator it = coverableFiles.begin(); it != coverableFiles.end();
      it++) {
    if (fileNameStr.rfind(*it) != std::string::npos) {
      // Found it!
      return true;
    }
  }

  return false;
}

void KModule::readInitialCoverage(std::istream &is) {
  std::string sourceFile;
  int lineNo;
  int execCount;

  while (!is.eof()) {
    is >> sourceFile >> lineNo >> execCount;

    if (is.eof() && sourceFile.length() == 0)
      break;

    coveredLines.insert(std::make_pair(sourceFile, lineNo));
  }
}

KModule::KModule(Module *_module) 
  : module(_module),
    targetData(new TargetData(module)),
    AA(NULL),
    DSAA(NULL),
    dbgStopPointFn(0),
    kleeMergeFn(0),
    infos(0) {
  // We know there is at least one constant per GlobalValue, so this
  // is a very rough approximation.
  constantTable.reserve(module->getValueSymbolTable().size() * 3);
}

KModule::~KModule() {
  ics::FinalizePrecondSlice();

  delete infos;

  for (std::vector<KFunction*>::iterator it = functions.begin(), 
         ie = functions.end(); it != ie; ++it)
    delete *it;

  delete targetData;
  delete module;
}

/***/

extern void Optimize(Module*);

// what a hack
static Function *getStubFunctionForCtorList(Module *m,
                                            GlobalVariable *gv, 
                                            std::string name) {
  assert(!gv->isDeclaration() && !gv->hasInternalLinkage() &&
         "do not support old LLVM style constructor/destructor lists");
  
  std::vector<const Type*> nullary;

  Function *fn = Function::Create(FunctionType::get(Type::getVoidTy(getGlobalContext()), 
                                                    nullary, false),
                                  GlobalVariable::InternalLinkage, 
                                  name,
                                  m);
  BasicBlock *bb = BasicBlock::Create(getGlobalContext(), "entry", fn);
  
  // From lli:
  // Should be an array of '{ int, void ()* }' structs.  The first value is
  // the init priority, which we ignore.
  ConstantArray *arr = dyn_cast<ConstantArray>(gv->getInitializer());
  if (arr) {
    for (unsigned i=0; i<arr->getNumOperands(); i++) {
      ConstantStruct *cs = cast<ConstantStruct>(arr->getOperand(i));
      assert(cs->getNumOperands()==2 && "unexpected element in ctor initializer list");
      
      Constant *fp = cs->getOperand(1);      
      if (fp->isNullValue())
        continue;

      if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(fp))
        fp = ce->getOperand(0);

      if (!isa<Function>(fp)) {
        assert(0 && "unable to get function pointer from ctor initializer list");
        continue;
      }

      Function *f = cast<Function>(fp);
      if (f->getName().find("__klee_force_ics_linkage") != StringRef::npos) {
        llvm::errs() << "KLEE: WARNING: Skipping ics ctor: " << *cs->getOperand(1) << "\n";
        continue;
      }

      CallInst::Create(f, "", bb);
    }
  }
  
  ReturnInst::Create(getGlobalContext(), bb);

  return fn;
}

static void injectStaticConstructorsAndDestructors(Module *m) {
  GlobalVariable *ctors = m->getNamedGlobal("llvm.global_ctors");
  GlobalVariable *dtors = m->getNamedGlobal("llvm.global_dtors");
  
  if (ctors || dtors) {
    Function *mainFn = m->getFunction("main");
    assert(mainFn && "unable to find main function");

    if (ctors) {
      CallInst::Create(getStubFunctionForCtorList(m, ctors, "klee.ctor_stub"),
                       "", mainFn->begin()->begin());
    }
    if (dtors) {
      Function *dtorStub = getStubFunctionForCtorList(m, dtors, "klee.dtor_stub");
      for (Function::iterator it = mainFn->begin(), ie = mainFn->end();
           it != ie; ++it) {
        if (isa<ReturnInst>(it->getTerminator()))
          CallInst::Create(dtorStub, "", it->getTerminator());
      }
    }
  }
}

void KModule::prepare(const Interpreter::ModuleOptions &opts,
                      InterpreterHandler *ih) {
  if (!MergeAtExit.empty()) {
    Function *mergeFn = module->getFunction("klee_merge");
    if (!mergeFn) {
      const llvm::FunctionType *Ty = 
        FunctionType::get(Type::getVoidTy(getGlobalContext()), 
                          std::vector<const Type*>(), false);
      mergeFn = Function::Create(Ty, GlobalVariable::ExternalLinkage,
                                 "klee_merge",
                                 module);
    }

    for (cl::list<std::string>::iterator it = MergeAtExit.begin(), 
           ie = MergeAtExit.end(); it != ie; ++it) {
      std::string &name = *it;
      Function *f = module->getFunction(name);
      if (!f) {
        klee_error("cannot insert merge-at-exit for: %s (cannot find)",
                   name.c_str());
      } else if (f->isDeclaration()) {
        klee_error("cannot insert merge-at-exit for: %s (external)",
                   name.c_str());
      }

      BasicBlock *exit = BasicBlock::Create(getGlobalContext(), "exit", f);
      PHINode *result = 0;
      if (f->getReturnType() != Type::getVoidTy(getGlobalContext()))
        result = PHINode::Create(f->getReturnType(), "retval", exit);
      CallInst::Create(mergeFn, "", exit);
      ReturnInst::Create(getGlobalContext(), result, exit);

      llvm::errs() << "KLEE: adding klee_merge at exit of: " << name << "\n";
      for (llvm::Function::iterator bbit = f->begin(), bbie = f->end(); 
           bbit != bbie; ++bbit) {
        if (&*bbit != exit) {
          Instruction *i = bbit->getTerminator();
          if (i->getOpcode()==Instruction::Ret) {
            if (result) {
              result->addIncoming(i->getOperand(0), bbit);
            }
            i->eraseFromParent();
            BranchInst::Create(exit, bbit);
          }
        }
      }
    }
  }

  if (VulnerableSites != "") {
    std::ifstream fs(VulnerableSites.c_str());
    assert(!fs.fail());

    readVulnerablePoints(fs);
  }

  if (CoverableModules != "") {
    std::ifstream fs(CoverableModules.c_str());
    assert(!fs.fail());

    readCoverableFiles(fs);
  }

  if (InitialCoverage != "") {
    std::ifstream fs(InitialCoverage.c_str());
    assert(!fs.fail());

    readInitialCoverage(fs);
  }

  // Inject checks prior to optimization... we also perform the
  // invariant transformations that we will end up doing later so that
  // optimize is seeing what is as close as possible to the final
  // module.
  PassManager pm;
  pm.add(new RaiseAsmPass());
  if (opts.CheckDivZero) pm.add(new DivCheckPass());
  pm.add(new LowerSSEPass());
  // FIXME: This false here is to work around a bug in
  // IntrinsicLowering which caches values which may eventually be
  // deleted (via RAUW). This can be removed once LLVM fixes this
  // issue.
  pm.add(new IntrinsicCleanerPass(*targetData, false));
  pm.run(*module);

  if (opts.Optimize)
    Optimize(module);

  // FIXME: Find a way that we can test programs without requiring
  // this to be linked in, it makes low level debugging much more
  // annoying.  (NOTE: we'd still need the klee intrinsics: klee_int, etc.)
  llvm::sys::Path path(opts.LibraryDir);
  path.appendComponent("libkleeRuntimeIntrinsic.bca");
  module = linkWithLibrary(module, path.c_str());

  // Needs to happen after linking (since ctors/dtors can be modified)
  // and optimization (since global optimization can rewrite lists).
  injectStaticConstructorsAndDestructors(module);

  // Finally, run the passes that maintain invariants we expect during
  // interpretation. We run the intrinsic cleaner just in case we
  // linked in something with intrinsics but any external calls are
  // going to be unresolved. We really need to handle the intrinsics
  // directly I think?
  PassManager pm3;
  pm3.add(createCFGSimplificationPass());
  switch(SwitchType) {
  case eSwitchTypeInternal: break;
  case eSwitchTypeSimple: pm3.add(new LowerSwitchPass()); break;
  case eSwitchTypeLLVM:  pm3.add(createLowerSwitchPass()); break;
  default: klee_error("invalid --switch-type");
  }
  pm3.add(new IntrinsicCleanerPass(*targetData));
  pm3.add(createUnifyFunctionExitNodesPass());
  pm3.add(new SplitUnreachablesPass());
  pm3.add(new PhiCleanerPass());
  pm3.add(createVerifierPass());
  pm3.run(*module);

  // For cleanliness see if we can discard any of the functions we
  // forced to import.
  Function *f;
  f = module->getFunction("memcpy");
  if (f && f->use_empty()) f->eraseFromParent();
  f = module->getFunction("memmove");
  if (f && f->use_empty()) f->eraseFromParent();
  f = module->getFunction("memset");
  if (f && f->use_empty()) f->eraseFromParent();

  // This is the last step which may modify the source code
  ics::InitializePrecondSlice(this);

  // Write out the .ll assembly file. We truncate long lines to work
  // around a kcachegrind parsing bug (it puts them on new lines), so
  // that source browsing works.
  if (OutputSource) {
    std::ostream *os = ih->openOutputFile("assembly.ll");
    assert(os && os->good() && "unable to open source output");

    llvm::raw_os_ostream *ros = new llvm::raw_os_ostream(*os);

    // We have an option for this in case the user wants a .ll they
    // can compile.
    if (NoTruncateSourceLines) {
      *ros << *module;
    } else {
      bool truncated = false;
      std::string string;
      llvm::raw_string_ostream rss(string);
      rss << *module;
      rss.flush();
      const char *position = string.c_str();

      for (;;) {
        const char *end = index(position, '\n');
        if (!end) {
          *ros << position;
          break;
        } else {
          unsigned count = (end - position) + 1;
          if (count<255) {
            ros->write(position, count);
          } else {
            ros->write(position, 254);
            *ros << "\n";
            truncated = true;
          }
          position = end+1;
        }
      }
    }
    delete ros;

    delete os;
  }

  if (OutputModule) {
    std::ostream *f = ih->openOutputFile("assembly.bc");
    llvm::raw_os_ostream* rfs = new llvm::raw_os_ostream(*f);
    WriteBitcodeToFile(module, *rfs);
    delete rfs;
    delete f;
  }

  dbgStopPointFn = module->getFunction("llvm.dbg.stoppoint");
  kleeMergeFn = module->getFunction("klee_merge");

  /* Build shadow structures */

  infos = new InstructionInfoTable(module);

  std::map<std::string, Function*> fnList;
  
  for (Module::iterator it = module->begin(), ie = module->end();
         it != ie; ++it) {
    if (it->isDeclaration())
      continue;

    fnList[it->getNameStr()] = it;
  }

  for (std::map<std::string, Function*>::iterator it = fnList.begin();
      it != fnList.end(); it++) {
    Function *fn = it->second;
    
    KFunction *kf = new KFunction(fn, this);

    for (unsigned i=0; i<kf->numInstructions; ++i) {
      KInstruction *ki = kf->instructions[i];
      ki->info = &infos->getInfo(ki->inst);

      if (ki->inst->getOpcode() == Instruction::Call) {
        KCallInstruction* kCallI = dyn_cast<KCallInstruction>(ki);
        kCallI->vulnerable = isVulnerablePoint(ki);
      }

      Path sourceFile(ki->info->file);
      program_point_t pPoint = std::make_pair(llvm::sys::path::filename(StringRef(sourceFile.str())),
          ki->info->line);

      ki->originallyCovered = coveredLines.count(pPoint) > 0;
    }

    kf->trackCoverage = isFunctionCoverable(kf);

    functions.push_back(kf);
    functionMap.insert(std::make_pair(fn, kf));
  }

  /* Compute various interesting properties */

  for (std::vector<KFunction*>::iterator it = functions.begin(), 
         ie = functions.end(); it != ie; ++it) {
    KFunction *kf = *it;
    if (functionEscapes(kf->function))
      escapingFunctions.insert(kf->function);
  }

  if (DebugPrintEscapingFunctions && !escapingFunctions.empty()) {
    llvm::errs() << "KLEE: escaping functions: [";
    for (std::set<Function*>::iterator it = escapingFunctions.begin(), 
         ie = escapingFunctions.end(); it != ie; ++it) {
      llvm::errs() << (*it)->getName() << ", ";
    }
    llvm::errs() << "]\n";
  }
}

KConstant* KModule::getKConstant(Constant *c) {
  std::map<llvm::Constant*, KConstant*>::iterator it = constantMap.find(c);
  if (it != constantMap.end())
    return it->second;
  return NULL;
}

unsigned KModule::getConstantID(Constant *c, KInstruction* ki) {
  KConstant *kc = getKConstant(c);
  if (kc)
    return kc->id;  

  unsigned id = constantTable.size();
  kc = new KConstant(c, id, ki);
  constantMap[c] = kc;
  constantTable.push_back(std::make_pair(kc, Cell()));
  return id;
}

/***/

KConstant::KConstant(llvm::Constant* _ct, unsigned _id, KInstruction* _ki) {
  ct = _ct;
  id = _id;
  ki = _ki;
}

/***/

static int getOperandNum(Value *v,
                         std::map<Instruction*, unsigned> &registerMap,
                         KModule *km,
                         KInstruction *ki) {
  if (Instruction *inst = dyn_cast<Instruction>(v)) {
    return registerMap[inst];
  } else if (Argument *a = dyn_cast<Argument>(v)) {
    return a->getArgNo();
  } else if (isa<BasicBlock>(v) || isa<InlineAsm>(v) ||
             isa<MDNode>(v)) {
    return -1;
  } else {
    assert(isa<Constant>(v));
    Constant *c = cast<Constant>(v);
    return -(km->getConstantID(c, ki) + 2);
  }
}

KFunction::KFunction(llvm::Function *_function,
                     KModule *km) 
  : function(_function),
    numArgs(function->arg_size()),
    numInstructions(0),
    trackCoverage(true) {
  for (llvm::Function::iterator bbit = function->begin(), 
         bbie = function->end(); bbit != bbie; ++bbit) {
    BasicBlock *bb = bbit;
    basicBlockEntry[bb] = numInstructions;
    numInstructions += bb->size();
  }

  instructions = new KInstruction*[numInstructions];

  std::map<Instruction*, unsigned> registerMap;

  // The first arg_size() registers are reserved for formals.
  unsigned rnum = numArgs;
  for (llvm::Function::iterator bbit = function->begin(), 
         bbie = function->end(); bbit != bbie; ++bbit) {
    for (llvm::BasicBlock::iterator it = bbit->begin(), ie = bbit->end();
         it != ie; ++it)
      registerMap[it] = rnum++;
  }
  numRegisters = rnum;
  
  unsigned i = 0;
  for (llvm::Function::iterator bbit = function->begin(), 
         bbie = function->end(); bbit != bbie; ++bbit) {
    for (llvm::BasicBlock::iterator it = bbit->begin(), ie = bbit->end();
         it != ie; ++it) {
      KInstruction *ki;

      switch(it->getOpcode()) {
      case Instruction::GetElementPtr:
      case Instruction::InsertValue:
      case Instruction::ExtractValue:
        ki = new KGEPInstruction(); break;
      case Instruction::Call:
      case Instruction::Invoke:
        ki = new KCallInstruction();
        break;
      default:
        ki = new KInstruction(); break;
      }

      ki->inst = it;      
      ki->dest = registerMap[it];

      if (isa<CallInst>(it) || isa<InvokeInst>(it)) {
        CallSite cs(it);
        unsigned numArgs = cs.arg_size();
        ki->operands = new int[numArgs+1];
        ki->operands[0] = getOperandNum(cs.getCalledValue(), registerMap, km,
                                        ki);
        for (unsigned j=0; j<numArgs; j++) {
          Value *v = cs.getArgument(j);
          ki->operands[j+1] = getOperandNum(v, registerMap, km, ki);
        }
      }
      else {
        unsigned numOperands = it->getNumOperands(); 
        ki->operands = new int[numOperands];
        for (unsigned j=0; j<numOperands; j++) {
          Value *v = it->getOperand(j);
        
          if (Instruction *inst = dyn_cast<Instruction>(v)) {
            ki->operands[j] = registerMap[inst];
          } else if (Argument *a = dyn_cast<Argument>(v)) {
            ki->operands[j] = a->getArgNo();
          } else if (isa<BasicBlock>(v) || isa<InlineAsm>(v) ||
                      isa<MDNode>(v)) {
            ki->operands[j] = -1;
          } else {
            assert(isa<Constant>(v));
            Constant *c = cast<Constant>(v);
            ki->operands[j] = -(km->getConstantID(c, ki) + 2);
          }
        }
      }

      instructions[i++] = ki;
    }
  }
}

KFunction::~KFunction() {
  for (unsigned i=0; i<numInstructions; ++i)
    delete instructions[i];
  delete[] instructions;
}

/***/

bool ShouldLoweringPassesIgnoreFunction(const llvm::Function *f) {
  for (size_t k = 0; k < LoweringIgnores.size(); ++k)
    if (f->getName().startswith(LoweringIgnores[k]))
      return true;
  return false;
}

/***/

bool DbgDeleteGlobals = false;

void DeleteGlobal(llvm::GlobalValue *G, const bool eraseFromParent /*= true*/) {
  if (DbgDeleteGlobals) {
    llvm::errs() << "Deleting global: " << G->getName() << "\n";
  }

  G->removeDeadConstantUsers();
  if (!G->use_empty()) {
    llvm::errs() << "ERROR: deleting global with remaining uses\n"
                 << "  global :: " << G->getName() << "\n";
    for (Value::use_iterator U = G->use_begin(); U != G->use_end(); ++U)
      llvm::errs() << "  use :: " << **U << "\n";
    assert(0);
  }
  if (eraseFromParent) {
    G->eraseFromParent();
  } else {
    delete G;
  }
}

void DeleteGlobals(llvm::Module *module, const StringMatcher pattern) {
  // GlobalVars
  for (llvm::Module::global_iterator
       G = module->global_begin(); G != module->global_end(); /* nop */) {
    llvm::Module::global_iterator tmp = G;
    ++G;
    if (pattern.matches(tmp->getName()))
      DeleteGlobal(tmp);
  }
  // Functions
  for (llvm::Module::iterator G = module->begin(); G != module->end(); /* nop */) {
    llvm::Module::iterator tmp = G;
    ++G;
    if (pattern.matches(tmp->getName()))
      DeleteGlobal(tmp);
  }
}

static void UpdateUsedList(llvm::GlobalVariable *used, const std::vector<Constant*> &newList) {
  const ConstantArray *oldList = cast<ConstantArray>(used->getInitializer());
  const Type *elemTy = oldList->getType()->getElementType();

  // Need to delete and re-add because the type may have changed
  const ArrayType *newListTy = ArrayType::get(elemTy, newList.size());
  Module *module = used->getParent();
  used->removeFromParent();
  llvm::GlobalVariable *newUsed =
    new llvm::GlobalVariable(*module, newListTy, used->isConstant(), used->getLinkage(),
                             ConstantArray::get(newListTy, newList), used->getName());
  newUsed->setAlignment(used->getAlignment());
  newUsed->setUnnamedAddr(used->hasUnnamedAddr());
  newUsed->setVisibility(used->getVisibility());
  newUsed->setSection(used->getSection());
  DeleteGlobal(used, false);
}

void RemoveFromUsedList(llvm::Module *module, const StringMatcher pattern) {
  // This should exist
  llvm::GlobalVariable *used = module->getNamedGlobal("llvm.used");
  assert(used);
  const ConstantArray *oldList = cast<ConstantArray>(used->getInitializer());

  // Rebuild initializer ignoring things that match the given pattern
  std::vector<Constant*> newList;
  for (unsigned i = 0; i < oldList->getNumOperands(); ++i) {
    Constant *op = cast<Constant>(oldList->getOperand(i));
    if (!pattern.matches(op->stripPointerCasts()->getName()))
      newList.push_back(op);
  }

  UpdateUsedList(used, newList);
}

void AddToUsedList(llvm::Module *module, const std::set<Constant*> &newItems) {
  // This should exist
  llvm::GlobalVariable *used = module->getNamedGlobal("llvm.used");
  assert(used);
  const ConstantArray *oldList = cast<ConstantArray>(used->getInitializer());
  const Type *elemTy = oldList->getType()->getElementType();

  // Rebuild initializer
  std::vector<Constant*> newList;
  for (unsigned i = 0; i < oldList->getNumOperands(); ++i) {
    Constant *op = oldList->getOperand(i);
    if (!newItems.count(cast<Constant>(op->stripPointerCasts())))
      newList.push_back(llvm::ConstantExpr::getPointerCast(op, elemTy));
  }

  // Append the new items to the initializer
  for (std::set<Constant*>::const_iterator it = newItems.begin(); it != newItems.end(); ++it) {
    newList.push_back(llvm::ConstantExpr::getPointerCast(*it, elemTy));
  }

  UpdateUsedList(used, newList);
}

}  // namespace klee
