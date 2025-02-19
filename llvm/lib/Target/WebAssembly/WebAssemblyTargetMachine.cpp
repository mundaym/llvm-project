//===- WebAssemblyTargetMachine.cpp - Define TargetMachine for WebAssembly -==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines the WebAssembly-specific subclass of TargetMachine.
///
//===----------------------------------------------------------------------===//

#include "WebAssemblyTargetMachine.h"
#include "MCTargetDesc/WebAssemblyMCTargetDesc.h"
#include "TargetInfo/WebAssemblyTargetInfo.h"
#include "WebAssembly.h"
#include "WebAssemblyMachineFunctionInfo.h"
#include "WebAssemblyTargetObjectFile.h"
#include "WebAssemblyTargetTransformInfo.h"
#include "llvm/CodeGen/MIRParser/MIParser.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/LowerAtomic.h"
#include "llvm/Transforms/Utils.h"
using namespace llvm;

#define DEBUG_TYPE "wasm"

// Emscripten's asm.js-style exception handling
cl::opt<bool> EnableEmException(
    "enable-emscripten-cxx-exceptions",
    cl::desc("WebAssembly Emscripten-style exception handling"),
    cl::init(false));

// Emscripten's asm.js-style setjmp/longjmp handling
cl::opt<bool> EnableEmSjLj(
    "enable-emscripten-sjlj",
    cl::desc("WebAssembly Emscripten-style setjmp/longjmp handling"),
    cl::init(false));

// A command-line option to keep implicit locals
// for the purpose of testing with lit/llc ONLY.
// This produces output which is not valid WebAssembly, and is not supported
// by assemblers/disassemblers and other MC based tools.
static cl::opt<bool> WasmDisableExplicitLocals(
    "wasm-disable-explicit-locals", cl::Hidden,
    cl::desc("WebAssembly: output implicit locals in"
             " instruction output for test purposes only."),
    cl::init(false));

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeWebAssemblyTarget() {
  // Register the target.
  RegisterTargetMachine<WebAssemblyTargetMachine> X(
      getTheWebAssemblyTarget32());
  RegisterTargetMachine<WebAssemblyTargetMachine> Y(
      getTheWebAssemblyTarget64());

  // Register backend passes
  auto &PR = *PassRegistry::getPassRegistry();
  initializeWebAssemblyAddMissingPrototypesPass(PR);
  initializeWebAssemblyLowerEmscriptenEHSjLjPass(PR);
  initializeLowerGlobalDtorsPass(PR);
  initializeFixFunctionBitcastsPass(PR);
  initializeOptimizeReturnedPass(PR);
  initializeWebAssemblyArgumentMovePass(PR);
  initializeWebAssemblySetP2AlignOperandsPass(PR);
  initializeWebAssemblyReplacePhysRegsPass(PR);
  initializeWebAssemblyPrepareForLiveIntervalsPass(PR);
  initializeWebAssemblyOptimizeLiveIntervalsPass(PR);
  initializeWebAssemblyMemIntrinsicResultsPass(PR);
  initializeWebAssemblyRegStackifyPass(PR);
  initializeWebAssemblyRegColoringPass(PR);
  initializeWebAssemblyFixIrreducibleControlFlowPass(PR);
  initializeWebAssemblyLateEHPreparePass(PR);
  initializeWebAssemblyExceptionInfoPass(PR);
  initializeWebAssemblyCFGSortPass(PR);
  initializeWebAssemblyCFGStackifyPass(PR);
  initializeWebAssemblyExplicitLocalsPass(PR);
  initializeWebAssemblyLowerBrUnlessPass(PR);
  initializeWebAssemblyRegNumberingPass(PR);
  initializeWebAssemblyDebugFixupPass(PR);
  initializeWebAssemblyPeepholePass(PR);
}

//===----------------------------------------------------------------------===//
// WebAssembly Lowering public interface.
//===----------------------------------------------------------------------===//

static Reloc::Model getEffectiveRelocModel(Optional<Reloc::Model> RM,
                                           const Triple &TT) {
  if (!RM.hasValue()) {
    // Default to static relocation model.  This should always be more optimial
    // than PIC since the static linker can determine all global addresses and
    // assume direct function calls.
    return Reloc::Static;
  }

  if (!TT.isOSEmscripten()) {
    // Relocation modes other than static are currently implemented in a way
    // that only works for Emscripten, so disable them if we aren't targeting
    // Emscripten.
    return Reloc::Static;
  }

  return *RM;
}

/// Create an WebAssembly architecture model.
///
WebAssemblyTargetMachine::WebAssemblyTargetMachine(
    const Target &T, const Triple &TT, StringRef CPU, StringRef FS,
    const TargetOptions &Options, Optional<Reloc::Model> RM,
    Optional<CodeModel::Model> CM, CodeGenOpt::Level OL, bool JIT)
    : LLVMTargetMachine(T,
                        TT.isArch64Bit() ? "e-m:e-p:64:64-i64:64-n32:64-S128"
                                         : "e-m:e-p:32:32-i64:64-n32:64-S128",
                        TT, CPU, FS, Options, getEffectiveRelocModel(RM, TT),
                        getEffectiveCodeModel(CM, CodeModel::Large), OL),
      TLOF(new WebAssemblyTargetObjectFile()) {
  // WebAssembly type-checks instructions, but a noreturn function with a return
  // type that doesn't match the context will cause a check failure. So we lower
  // LLVM 'unreachable' to ISD::TRAP and then lower that to WebAssembly's
  // 'unreachable' instructions which is meant for that case.
  this->Options.TrapUnreachable = true;

  // WebAssembly treats each function as an independent unit. Force
  // -ffunction-sections, effectively, so that we can emit them independently.
  this->Options.FunctionSections = true;
  this->Options.DataSections = true;
  this->Options.UniqueSectionNames = true;

  initAsmInfo();

  // Note that we don't use setRequiresStructuredCFG(true). It disables
  // optimizations than we're ok with, and want, such as critical edge
  // splitting and tail merging.
}

WebAssemblyTargetMachine::~WebAssemblyTargetMachine() = default; // anchor.

const WebAssemblySubtarget *WebAssemblyTargetMachine::getSubtargetImpl() const {
  return getSubtargetImpl(std::string(getTargetCPU()),
                          std::string(getTargetFeatureString()));
}

const WebAssemblySubtarget *
WebAssemblyTargetMachine::getSubtargetImpl(std::string CPU,
                                           std::string FS) const {
  auto &I = SubtargetMap[CPU + FS];
  if (!I) {
    I = std::make_unique<WebAssemblySubtarget>(TargetTriple, CPU, FS, *this);
  }
  return I.get();
}

const WebAssemblySubtarget *
WebAssemblyTargetMachine::getSubtargetImpl(const Function &F) const {
  Attribute CPUAttr = F.getFnAttribute("target-cpu");
  Attribute FSAttr = F.getFnAttribute("target-features");

  std::string CPU =
      CPUAttr.isValid() ? CPUAttr.getValueAsString().str() : TargetCPU;
  std::string FS =
      FSAttr.isValid() ? FSAttr.getValueAsString().str() : TargetFS;

  // This needs to be done before we create a new subtarget since any
  // creation will depend on the TM and the code generation flags on the
  // function that reside in TargetOptions.
  resetTargetOptions(F);

  return getSubtargetImpl(CPU, FS);
}

namespace {

class CoalesceFeaturesAndStripAtomics final : public ModulePass {
  // Take the union of all features used in the module and use it for each
  // function individually, since having multiple feature sets in one module
  // currently does not make sense for WebAssembly. If atomics are not enabled,
  // also strip atomic operations and thread local storage.
  static char ID;
  WebAssemblyTargetMachine *WasmTM;

public:
  CoalesceFeaturesAndStripAtomics(WebAssemblyTargetMachine *WasmTM)
      : ModulePass(ID), WasmTM(WasmTM) {}

  bool runOnModule(Module &M) override {
    FeatureBitset Features = coalesceFeatures(M);

    std::string FeatureStr = getFeatureString(Features);
    WasmTM->setTargetFeatureString(FeatureStr);
    for (auto &F : M)
      replaceFeatures(F, FeatureStr);

    bool StrippedAtomics = false;
    bool StrippedTLS = false;

    if (!Features[WebAssembly::FeatureAtomics])
      StrippedAtomics = stripAtomics(M);

    if (!Features[WebAssembly::FeatureBulkMemory])
      StrippedTLS = stripThreadLocals(M);

    if (StrippedAtomics && !StrippedTLS)
      stripThreadLocals(M);
    else if (StrippedTLS && !StrippedAtomics)
      stripAtomics(M);

    recordFeatures(M, Features, StrippedAtomics || StrippedTLS);

    // Conservatively assume we have made some change
    return true;
  }

private:
  FeatureBitset coalesceFeatures(const Module &M) {
    FeatureBitset Features =
        WasmTM
            ->getSubtargetImpl(std::string(WasmTM->getTargetCPU()),
                               std::string(WasmTM->getTargetFeatureString()))
            ->getFeatureBits();
    for (auto &F : M)
      Features |= WasmTM->getSubtargetImpl(F)->getFeatureBits();
    return Features;
  }

  std::string getFeatureString(const FeatureBitset &Features) {
    std::string Ret;
    for (const SubtargetFeatureKV &KV : WebAssemblyFeatureKV) {
      if (Features[KV.Value])
        Ret += (StringRef("+") + KV.Key + ",").str();
    }
    return Ret;
  }

  void replaceFeatures(Function &F, const std::string &Features) {
    F.removeFnAttr("target-features");
    F.removeFnAttr("target-cpu");
    F.addFnAttr("target-features", Features);
  }

  bool stripAtomics(Module &M) {
    // Detect whether any atomics will be lowered, since there is no way to tell
    // whether the LowerAtomic pass lowers e.g. stores.
    bool Stripped = false;
    for (auto &F : M) {
      for (auto &B : F) {
        for (auto &I : B) {
          if (I.isAtomic()) {
            Stripped = true;
            goto done;
          }
        }
      }
    }

  done:
    if (!Stripped)
      return false;

    LowerAtomicPass Lowerer;
    FunctionAnalysisManager FAM;
    for (auto &F : M)
      Lowerer.run(F, FAM);

    return true;
  }

  bool stripThreadLocals(Module &M) {
    bool Stripped = false;
    for (auto &GV : M.globals()) {
      if (GV.isThreadLocal()) {
        Stripped = true;
        GV.setThreadLocal(false);
      }
    }
    return Stripped;
  }

  void recordFeatures(Module &M, const FeatureBitset &Features, bool Stripped) {
    for (const SubtargetFeatureKV &KV : WebAssemblyFeatureKV) {
      if (Features[KV.Value]) {
        // Mark features as used
        std::string MDKey = (StringRef("wasm-feature-") + KV.Key).str();
        M.addModuleFlag(Module::ModFlagBehavior::Error, MDKey,
                        wasm::WASM_FEATURE_PREFIX_USED);
      }
    }
    // Code compiled without atomics or bulk-memory may have had its atomics or
    // thread-local data lowered to nonatomic operations or non-thread-local
    // data. In that case, we mark the pseudo-feature "shared-mem" as disallowed
    // to tell the linker that it would be unsafe to allow this code ot be used
    // in a module with shared memory.
    if (Stripped) {
      M.addModuleFlag(Module::ModFlagBehavior::Error, "wasm-feature-shared-mem",
                      wasm::WASM_FEATURE_PREFIX_DISALLOWED);
    }
  }
};
char CoalesceFeaturesAndStripAtomics::ID = 0;

/// WebAssembly Code Generator Pass Configuration Options.
class WebAssemblyPassConfig final : public TargetPassConfig {
public:
  WebAssemblyPassConfig(WebAssemblyTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  WebAssemblyTargetMachine &getWebAssemblyTargetMachine() const {
    return getTM<WebAssemblyTargetMachine>();
  }

  FunctionPass *createTargetRegisterAllocator(bool) override;

  void addIRPasses() override;
  bool addInstSelector() override;
  void addPostRegAlloc() override;
  bool addGCPasses() override { return false; }
  void addPreEmitPass() override;

  // No reg alloc
  bool addRegAssignAndRewriteFast() override { return false; }

  // No reg alloc
  bool addRegAssignAndRewriteOptimized() override { return false; }
};
} // end anonymous namespace

TargetTransformInfo
WebAssemblyTargetMachine::getTargetTransformInfo(const Function &F) {
  return TargetTransformInfo(WebAssemblyTTIImpl(this, F));
}

TargetPassConfig *
WebAssemblyTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new WebAssemblyPassConfig(*this, PM);
}

FunctionPass *WebAssemblyPassConfig::createTargetRegisterAllocator(bool) {
  return nullptr; // No reg alloc
}

//===----------------------------------------------------------------------===//
// The following functions are called from lib/CodeGen/Passes.cpp to modify
// the CodeGen pass sequence.
//===----------------------------------------------------------------------===//

void WebAssemblyPassConfig::addIRPasses() {
  // Lower atomics and TLS if necessary
  addPass(new CoalesceFeaturesAndStripAtomics(&getWebAssemblyTargetMachine()));

  // This is a no-op if atomics are not used in the module
  addPass(createAtomicExpandPass());

  // Add signatures to prototype-less function declarations
  addPass(createWebAssemblyAddMissingPrototypes());

  // Lower .llvm.global_dtors into .llvm_global_ctors with __cxa_atexit calls.
  addPass(createWebAssemblyLowerGlobalDtors());

  // Fix function bitcasts, as WebAssembly requires caller and callee signatures
  // to match.
  addPass(createWebAssemblyFixFunctionBitcasts());

  // Optimize "returned" function attributes.
  if (getOptLevel() != CodeGenOpt::None)
    addPass(createWebAssemblyOptimizeReturned());

  // If exception handling is not enabled and setjmp/longjmp handling is
  // enabled, we lower invokes into calls and delete unreachable landingpad
  // blocks. Lowering invokes when there is no EH support is done in
  // TargetPassConfig::addPassesToHandleExceptions, but this runs after this
  // function and SjLj handling expects all invokes to be lowered before.
  if (!EnableEmException &&
      TM->Options.ExceptionModel == ExceptionHandling::None) {
    addPass(createLowerInvokePass());
    // The lower invoke pass may create unreachable code. Remove it in order not
    // to process dead blocks in setjmp/longjmp handling.
    addPass(createUnreachableBlockEliminationPass());
  }

  // Handle exceptions and setjmp/longjmp if enabled.
  if (EnableEmException || EnableEmSjLj)
    addPass(createWebAssemblyLowerEmscriptenEHSjLj(EnableEmException,
                                                   EnableEmSjLj));

  // Expand indirectbr instructions to switches.
  addPass(createIndirectBrExpandPass());

  TargetPassConfig::addIRPasses();
}

bool WebAssemblyPassConfig::addInstSelector() {
  (void)TargetPassConfig::addInstSelector();
  addPass(
      createWebAssemblyISelDag(getWebAssemblyTargetMachine(), getOptLevel()));
  // Run the argument-move pass immediately after the ScheduleDAG scheduler
  // so that we can fix up the ARGUMENT instructions before anything else
  // sees them in the wrong place.
  addPass(createWebAssemblyArgumentMove());
  // Set the p2align operands. This information is present during ISel, however
  // it's inconvenient to collect. Collect it now, and update the immediate
  // operands.
  addPass(createWebAssemblySetP2AlignOperands());

  // Eliminate range checks and add default targets to br_table instructions.
  addPass(createWebAssemblyFixBrTableDefaults());

  return false;
}

void WebAssemblyPassConfig::addPostRegAlloc() {
  // TODO: The following CodeGen passes don't currently support code containing
  // virtual registers. Consider removing their restrictions and re-enabling
  // them.

  // These functions all require the NoVRegs property.
  disablePass(&MachineCopyPropagationID);
  disablePass(&PostRAMachineSinkingID);
  disablePass(&PostRASchedulerID);
  disablePass(&FuncletLayoutID);
  disablePass(&StackMapLivenessID);
  disablePass(&LiveDebugValuesID);
  disablePass(&PatchableFunctionID);
  disablePass(&ShrinkWrapID);

  // This pass hurts code size for wasm because it can generate irreducible
  // control flow.
  disablePass(&MachineBlockPlacementID);

  TargetPassConfig::addPostRegAlloc();
}

void WebAssemblyPassConfig::addPreEmitPass() {
  TargetPassConfig::addPreEmitPass();

  // Eliminate multiple-entry loops.
  addPass(createWebAssemblyFixIrreducibleControlFlow());

  // Do various transformations for exception handling.
  // Every CFG-changing optimizations should come before this.
  addPass(createWebAssemblyLateEHPrepare());

  // Now that we have a prologue and epilogue and all frame indices are
  // rewritten, eliminate SP and FP. This allows them to be stackified,
  // colored, and numbered with the rest of the registers.
  addPass(createWebAssemblyReplacePhysRegs());

  // Preparations and optimizations related to register stackification.
  if (getOptLevel() != CodeGenOpt::None) {
    // LiveIntervals isn't commonly run this late. Re-establish preconditions.
    addPass(createWebAssemblyPrepareForLiveIntervals());

    // Depend on LiveIntervals and perform some optimizations on it.
    addPass(createWebAssemblyOptimizeLiveIntervals());

    // Prepare memory intrinsic calls for register stackifying.
    addPass(createWebAssemblyMemIntrinsicResults());

    // Mark registers as representing wasm's value stack. This is a key
    // code-compression technique in WebAssembly. We run this pass (and
    // MemIntrinsicResults above) very late, so that it sees as much code as
    // possible, including code emitted by PEI and expanded by late tail
    // duplication.
    addPass(createWebAssemblyRegStackify());

    // Run the register coloring pass to reduce the total number of registers.
    // This runs after stackification so that it doesn't consider registers
    // that become stackified.
    addPass(createWebAssemblyRegColoring());
  }

  // Sort the blocks of the CFG into topological order, a prerequisite for
  // BLOCK and LOOP markers.
  addPass(createWebAssemblyCFGSort());

  // Insert BLOCK and LOOP markers.
  addPass(createWebAssemblyCFGStackify());

  // Insert explicit local.get and local.set operators.
  if (!WasmDisableExplicitLocals)
    addPass(createWebAssemblyExplicitLocals());

  // Lower br_unless into br_if.
  addPass(createWebAssemblyLowerBrUnless());

  // Perform the very last peephole optimizations on the code.
  if (getOptLevel() != CodeGenOpt::None)
    addPass(createWebAssemblyPeephole());

  // Create a mapping from LLVM CodeGen virtual registers to wasm registers.
  addPass(createWebAssemblyRegNumbering());

  // Fix debug_values whose defs have been stackified.
  if (!WasmDisableExplicitLocals)
    addPass(createWebAssemblyDebugFixup());
}

yaml::MachineFunctionInfo *
WebAssemblyTargetMachine::createDefaultFuncInfoYAML() const {
  return new yaml::WebAssemblyFunctionInfo();
}

yaml::MachineFunctionInfo *WebAssemblyTargetMachine::convertFuncInfoToYAML(
    const MachineFunction &MF) const {
  const auto *MFI = MF.getInfo<WebAssemblyFunctionInfo>();
  return new yaml::WebAssemblyFunctionInfo(*MFI);
}

bool WebAssemblyTargetMachine::parseMachineFunctionInfo(
    const yaml::MachineFunctionInfo &MFI, PerFunctionMIParsingState &PFS,
    SMDiagnostic &Error, SMRange &SourceRange) const {
  const auto &YamlMFI =
      reinterpret_cast<const yaml::WebAssemblyFunctionInfo &>(MFI);
  MachineFunction &MF = PFS.MF;
  MF.getInfo<WebAssemblyFunctionInfo>()->initializeBaseYamlFields(YamlMFI);
  return false;
}
