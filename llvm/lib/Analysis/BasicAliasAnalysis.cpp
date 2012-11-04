//===- BasicAliasAnalysis.cpp - Local Alias Analysis Impl -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the default implementation of the Alias Analysis interface
// that simply implements a few identities (two different globals cannot alias,
// etc), but otherwise does no analysis.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/GlobalAlias.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/Operator.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/HypotheticalConstantFolder.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Target/TargetData.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include <algorithm>
using namespace llvm;

//===----------------------------------------------------------------------===//
// Useful predicates
//===----------------------------------------------------------------------===//

/// isKnownNonNull - Return true if we know that the specified value is never
/// null.
static bool isKnownNonNull(const Value *V) {
  // Alloca never returns null, malloc might.
  if (isa<AllocaInst>(V)) return true;
  
  // A byval argument is never null.
  if (const Argument *A = dyn_cast<Argument>(V))
    return A->hasByValAttr();

  // Global values are not null unless extern weak.
  if (const GlobalValue *GV = dyn_cast<GlobalValue>(V))
    return !GV->hasExternalWeakLinkage();
  return false;
}

/// isNonEscapingLocalObject - Return true if the pointer is to a function-local
/// object that never escapes from the function.
static bool isNonEscapingLocalObject(const Value *V) {
  // If this is a local allocation, check to see if it escapes.
  if (isa<AllocaInst>(V) || isNoAliasCall(V))
    // Set StoreCaptures to True so that we can assume in our callers that the
    // pointer is not the result of a load instruction. Currently
    // PointerMayBeCaptured doesn't have any special analysis for the
    // StoreCaptures=false case; if it did, our callers could be refined to be
    // more precise.
    return !PointerMayBeCaptured(V, false, /*StoreCaptures=*/true);

  // If this is an argument that corresponds to a byval or noalias argument,
  // then it has not escaped before entering the function.  Check if it escapes
  // inside the function.
  if (const Argument *A = dyn_cast<Argument>(V))
    if (A->hasByValAttr() || A->hasNoAliasAttr()) {
      // Don't bother analyzing arguments already known not to escape.
      if (A->hasNoCaptureAttr())
        return true;
      return !PointerMayBeCaptured(V, false, /*StoreCaptures=*/true);
    }
  return false;
}

/// isEscapeSource - Return true if the pointer is one which would have
/// been considered an escape by isNonEscapingLocalObject.
static bool isEscapeSource(const Value *V) {
  if (isa<CallInst>(V) || isa<InvokeInst>(V) || isa<Argument>(V))
    return true;

  // The load case works because isNonEscapingLocalObject considers all
  // stores to be escapes (it passes true for the StoreCaptures argument
  // to PointerMayBeCaptured).
  if (isa<LoadInst>(V))
    return true;

  return false;
}

/// isObjectSmallerThan - Return true if we can prove that the object specified
/// by V is smaller than Size.
static bool isObjectSmallerThan(const Value *V, unsigned Size,
                                const TargetData &TD) {
  const Type *AccessTy;
  if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(V)) {
    AccessTy = GV->getType()->getElementType();
  } else if (const AllocaInst *AI = dyn_cast<AllocaInst>(V)) {
    if (!AI->isArrayAllocation())
      AccessTy = AI->getType()->getElementType();
    else
      return false;
  } else if (const CallInst* CI = extractMallocCall(V)) {
    if (!isArrayMalloc(V, &TD))
      // The size is the argument to the malloc call.
      if (const ConstantInt* C = dyn_cast<ConstantInt>(CI->getArgOperand(0)))
        return (C->getZExtValue() < Size);
    return false;
  } else if (const Argument *A = dyn_cast<Argument>(V)) {
    if (A->hasByValAttr())
      AccessTy = cast<PointerType>(A->getType())->getElementType();
    else
      return false;
  } else {
    return false;
  }
  
  if (AccessTy->isSized())
    return TD.getTypeAllocSize(AccessTy) < Size;
  return false;
}

//===----------------------------------------------------------------------===//
// NoAA Pass
//===----------------------------------------------------------------------===//

namespace {
  /// NoAA - This class implements the -no-aa pass, which always returns "I
  /// don't know" for alias queries.  NoAA is unlike other alias analysis
  /// implementations, in that it does not chain to a previous analysis.  As
  /// such it doesn't follow many of the rules that other alias analyses must.
  ///
  struct NoAA : public ImmutablePass, public AliasAnalysis {
    static char ID; // Class identification, replacement for typeinfo
    NoAA() : ImmutablePass(ID) {}
    explicit NoAA(char &PID) : ImmutablePass(PID) { }

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    }

    virtual void initializePass() {
      TD = getAnalysisIfAvailable<TargetData>();
    }

    virtual AliasResult alias(const Value *V1, unsigned V1Size,
                              const Value *V2, unsigned V2Size) {
      return MayAlias;
    }

    virtual AliasResult aliasHypothetical(const Value *V1, unsigned V1Size,
					  const Value *V2, unsigned V2Size,
					  IntegrationAttempt* parent,
					  bool usePBKnowledge) {
      return MayAlias;
    }

    virtual AliasResult aliasHypothetical(ValCtx V1, unsigned V1Size,
					  ValCtx V2, unsigned V2Size,
					  bool usePBKnowledge) {
      return MayAlias;
    }

    virtual ModRefBehavior getModRefBehavior(ImmutableCallSite CS) {
      return UnknownModRefBehavior;
    }
    virtual ModRefBehavior getModRefBehavior(const Function *F) {
      return UnknownModRefBehavior;
    }

    virtual bool pointsToConstantMemory(const Value *P) { return false; }
    virtual ModRefResult getModRefInfo(ImmutableCallSite CS,
                                       const Value *P, unsigned Size, IntegrationAttempt* CSCtx = 0, IntegrationAttempt* PCtx = 0, bool usePBKnowledge = true) {
      return ModRef;
    }
    virtual ModRefResult getModRefInfo(ImmutableCallSite CS1, ImmutableCallSite CS2, 
				       IntegrationAttempt* CS1Ctx = 0, IntegrationAttempt* CS2Ctx = 0, bool usePBKnowledge = true) {
      return ModRef;
    }

    virtual void deleteValue(Value *V) {}
    virtual void copyValue(Value *From, Value *To) {}
    
    /// getAdjustedAnalysisPointer - This method is used when a pass implements
    /// an analysis interface through multiple inheritance.  If needed, it
    /// should override this to adjust the this pointer as needed for the
    /// specified pass info.
    virtual void *getAdjustedAnalysisPointer(const void *ID) {
      if (ID == &AliasAnalysis::ID)
        return (AliasAnalysis*)this;
      return this;
    }
  };
}  // End of anonymous namespace

// Register this pass...
char NoAA::ID = 0;
INITIALIZE_AG_PASS(NoAA, AliasAnalysis, "no-aa",
                   "No Alias Analysis (always returns 'may' alias)",
                   true, true, false);

ImmutablePass *llvm::createNoAAPass() { return new NoAA(); }

//===----------------------------------------------------------------------===//
// GetElementPtr Instruction Decomposition and Analysis
//===----------------------------------------------------------------------===//

namespace {
  enum ExtensionKind {
    EK_NotExtended,
    EK_SignExt,
    EK_ZeroExt
  };
  
  struct VariableGEPIndex {
    ValCtx VC;
    ExtensionKind Extension;
    int64_t Scale;
  };
}

/// GetIndexDifference - Dest and Src are the variable indices from two
/// decomposed GetElementPtr instructions GEP1 and GEP2 which have common base
/// pointers.  Subtract the GEP2 indices from GEP1 to find the symbolic
/// difference between the two pointers. 
static void GetIndexDifference(SmallVectorImpl<VariableGEPIndex> &Dest,
                               const SmallVectorImpl<VariableGEPIndex> &Src) {
  if (Src.empty()) return;

  for (unsigned i = 0, e = Src.size(); i != e; ++i) {
    const ValCtx VC = Src[i].VC;
    ExtensionKind Extension = Src[i].Extension;
    int64_t Scale = Src[i].Scale;
    
    // Find VC in Dest.  This is N^2, but pointer indices almost never have more
    // than a few variable indexes.
    for (unsigned j = 0, e = Dest.size(); j != e; ++j) {
      if (Dest[j].VC != VC || Dest[j].Extension != Extension) continue;
      
      // If we found it, subtract off Scale VC's from the entry in Dest.  If it
      // goes to zero, remove the entry.
      if (Dest[j].Scale != Scale)
        Dest[j].Scale -= Scale;
      else
        Dest.erase(Dest.begin()+j);
      Scale = 0;
      break;
    }
    
    // If we didn't consume this entry, add it to the end of the Dest list.
    if (Scale) {
      VariableGEPIndex Entry = { VC, Extension, -Scale };
      Dest.push_back(Entry);
    }
  }
}

//===----------------------------------------------------------------------===//
// BasicAliasAnalysis Pass
//===----------------------------------------------------------------------===//

#ifndef NDEBUG
static const Function *getParent(const Value *V) {
  if (const Instruction *inst = dyn_cast<Instruction>(V))
    return inst->getParent()->getParent();

  if (const Argument *arg = dyn_cast<Argument>(V))
    return arg->getParent();

  return NULL;
}

static bool notDifferentParent(const Value *O1, const Value *O2) {

  const Function *F1 = getParent(O1);
  const Function *F2 = getParent(O2);

  return !F1 || !F2 || F1 == F2;
}
#endif

namespace {

  /// BasicAliasAnalysis - This is the default alias analysis implementation.
  /// Because it doesn't chain to a previous alias analysis (like -no-aa), it
  /// derives from the NoAA class.
  struct BasicAliasAnalysis : public NoAA {
    static char ID; // Class identification, replacement for typeinfo
    BasicAliasAnalysis() : NoAA(ID) {}

    virtual AliasResult alias(const Value *V1, unsigned V1Size,
                              const Value *V2, unsigned V2Size) {

      return aliasHypothetical(V1, V1Size, V2, V2Size, 0, true);

    }

    bool getPointerBase(const Value* V, IntegrationAttempt* Ctx, SmallVector<ValCtx, 4>& VCs) {

      ValCtx Repl = Ctx->getReplacement(const_cast<Value*>(V));
      bool ignored;
      ValCtx UO = getUltimateUnderlyingObject(Repl, ignored);
      if(isIdentifiedObject(UO.first)) {
	VCs.push_back(UO);
	return true;
      }

      PointerBase PB;
      if((!Ctx->getPointerBaseFalling(const_cast<Value*>(V), PB)) || (PB.Overdef))
	return false;
      else {
	VCs = PB.Values;
	return true;
      }

    }

    AliasAnalysis::AliasResult tryResolvePointerBases(const Value* V1, IntegrationAttempt* V1Ctx,
						      const Value* V2, IntegrationAttempt* V2Ctx) {

      SmallVector<ValCtx, 4> VCs1, VCs2;
      IntegrationAttempt* QueryCtx = V1Ctx ? V1Ctx : V2Ctx;
      if(getPointerBase(V1, V1Ctx, VCs1) && getPointerBase(V2, V2Ctx, VCs2)) {

	for(unsigned i = 0; i < VCs1.size(); ++i) {
	  for(unsigned j = 0; j < VCs2.size(); ++j) {

	    if(QueryCtx->basesMayAlias(VCs1[i], VCs2[j]))
	      return AliasAnalysis::MayAlias;

	  }
	}

	return AliasAnalysis::NoAlias;

      }
      else {

	return AliasAnalysis::MayAlias;

      }

    }

    virtual AliasResult aliasHypothetical(const Value *V1, unsigned V1Size,
					  const Value *V2, unsigned V2Size,
					  IntegrationAttempt* parent, bool usePBKnowledge) {
      
      assert(Visited.empty() && "Visited must be cleared after use!");
      assert(notDifferentParent(V1, V2) &&
             "BasicAliasAnalysis doesn't support interprocedural queries.");
      AliasResult Alias;
      if(parent) {
	Alias = aliasCheck(parent->getDefaultVC(const_cast<Value*>(V1)), V1Size, parent->getDefaultVC(const_cast<Value*>(V2)), V2Size);
	if(Alias == MayAlias && usePBKnowledge)
	  Alias = tryResolvePointerBases(V1, parent, V2, parent);
      }
      else
	Alias = aliasCheck(make_vc(const_cast<Value*>(V1), 0), V1Size, make_vc(const_cast<Value*>(V2), 0), V2Size);
      Visited.clear();
      return Alias;      

    }

    virtual AliasResult aliasHypothetical(ValCtx V1, unsigned V1Size,
					  ValCtx V2, unsigned V2Size, bool usePBKnowledge) {

      // I think I can ignore the not-different assertion!
      assert(Visited.empty() && "Visited must be cleared after use!");
      AliasResult Alias = aliasCheck(V1, V1Size, V2, V2Size);
      if(Alias == MayAlias && (V1.second || V2.second) && usePBKnowledge)
	Alias = tryResolvePointerBases(V1.first, V1.second, V2.first, V2.second);
      Visited.clear();
      return Alias;

    }

    virtual ModRefResult getModRefInfo(ImmutableCallSite CS,
                                       const Value *P, unsigned Size, 
				       IntegrationAttempt* CSCtx = 0, IntegrationAttempt* PCtx = 0, bool usePBKnowledge = true);

    virtual ModRefResult getModRefInfo(ImmutableCallSite CS1, ImmutableCallSite CS2, 
				       IntegrationAttempt* CS1Ctx = 0, IntegrationAttempt* CS2Ctx = 0, bool usePBKnowledge = true) {
      // The AliasAnalysis base class has some smarts, lets use them.
      return AliasAnalysis::getModRefInfo(CS1, CS2, CS1Ctx, CS2Ctx, usePBKnowledge);
    }

    /// pointsToConstantMemory - Chase pointers until we find a (constant
    /// global) or not.
    virtual bool pointsToConstantMemory(const Value *P);

    /// getModRefBehavior - Return the behavior when calling the given
    /// call site.
    virtual ModRefBehavior getModRefBehavior(ImmutableCallSite CS);

    /// getModRefBehavior - Return the behavior when calling the given function.
    /// For use when the call site is not known.
    virtual ModRefBehavior getModRefBehavior(const Function *F);

    /// getAdjustedAnalysisPointer - This method is used when a pass implements
    /// an analysis interface through multiple inheritance.  If needed, it
    /// should override this to adjust the this pointer as needed for the
    /// specified pass info.
    virtual void *getAdjustedAnalysisPointer(const void *ID) {
      if (ID == &AliasAnalysis::ID)
        return (AliasAnalysis*)this;
      return this;
    }
    
  private:
    // Visited - Track instructions visited by a aliasPHI, aliasSelect(), and aliasGEP().
    SmallPtrSet<const Value*, 16> Visited;
    
    // aliasGEP - Provide a bunch of ad-hoc rules to disambiguate a GEP
    // instruction against another.
    AliasResult aliasGEP(const ValCtx V1, unsigned V1Size,
                         const ValCtx V2, unsigned V2Size,
                         const ValCtx UnderlyingV1, const ValCtx UnderlyingV2);

    // aliasPHI - Provide a bunch of ad-hoc rules to disambiguate a PHI
    // instruction against another.
    AliasResult aliasPHI(const ValCtx PN, unsigned PNSize,
                         const ValCtx V2, unsigned V2Size);

    /// aliasSelect - Disambiguate a Select instruction against another value.
    AliasResult aliasSelect(const ValCtx SI, unsigned SISize,
                            const ValCtx V2, unsigned V2Size);

    AliasResult aliasCheck(ValCtx V1, unsigned V1Size,
                           ValCtx V2, unsigned V2Size);

    const ValCtx DecomposeGEPExpression(const ValCtx V, int64_t &BaseOffs,
					SmallVectorImpl<VariableGEPIndex> &VarIndices,
					const TargetData *TD);

    bool GEPHasAllZeroIndices(ValCtx GEPOp);

    // == getUltimateUnderlyingObject(VC, _, true)
    ValCtx getFirstOffset(ValCtx);
		  
    // idOnly means that we won't walk through GEP instructions that offset the pointer.
    ValCtx getUltimateUnderlyingObject(ValCtx, bool& isOffset, bool idOnly = false);
    Value* getUnderlyingObject(ValCtx VIn, bool& isOffset, bool idOnly = false);

    ValCtx getReplacement(const ValCtx);
    Constant* getConstReplacement(const ValCtx);
    ValCtx tryConstReplacement(const ValCtx);

    Value* GetLinearExpression(Value *V, APInt &Scale, APInt &Offset,
			       ExtensionKind &Extension,
			       const TargetData &TD, unsigned Depth, IntegrationAttempt* Ctx);

  };
}  // End of anonymous namespace

// Register this pass...
char BasicAliasAnalysis::ID = 0;
INITIALIZE_AG_PASS(BasicAliasAnalysis, AliasAnalysis, "basicaa",
                   "Basic Alias Analysis (default AA impl)",
                   false, true, true);

ImmutablePass *llvm::createBasicAliasAnalysisPass() {
  return new BasicAliasAnalysis();
}

bool BasicAliasAnalysis::GEPHasAllZeroIndices(ValCtx GEPOp) {
  const GEPOperator* GEP = cast<GEPOperator>(GEPOp.first);
  for (unsigned i = 1, e = GEP->getNumOperands(); i != e; ++i) {
    ValCtx V = tryConstReplacement(make_vc(GEP->getOperand(i), GEPOp.second));
    if (ConstantInt *CI = dyn_cast<ConstantInt>(V.first)) {
      if (!CI->isZero()) return false;
    } 
    else {
      return false;
    }
  }
  return true;
}

/*
ValCtx BasicAliasAnalysis::getReplacement(const Value* VConst) {

  Value* V = const_cast<Value*>(VConst);

  if(!parent)
    return make_vc(V, 0);

  return parent->getReplacement(V);

}
*/

ValCtx BasicAliasAnalysis::getReplacement(const ValCtx VC) {

  if(!VC.second)
    return VC;
  else
    return VC.second->getReplacement(VC.first);

}

/*
Constant* BasicAliasAnalysis::getConstReplacement(const Value* VConst) {

  ValCtx VC = getReplacement(VConst);
  return dyn_cast<Constant>(VC.first);

}
*/

Constant* BasicAliasAnalysis::getConstReplacement(const ValCtx VC) {

  ValCtx VC2 = getReplacement(VC);
  return dyn_cast<Constant>(VC2.first);

}

/*
Value* BasicAliasAnalysis::tryConstReplacement(const Value* VConst) {

  Constant* Ret = getConstReplacement(VConst);
  if(Ret)
    return Ret;
  else
    return const_cast<Value*>(VConst);

}
*/

ValCtx BasicAliasAnalysis::tryConstReplacement(const ValCtx VC) {

  Constant* Ret = getConstReplacement(VC);
  if(Ret)
    return const_vc(Ret);
  else
    return VC;

}

/// GetLinearExpression - Analyze the specified value as a linear expression:
/// "A*V + B", where A and B are constant integers.  Return the scale and offset
/// values as APInts and return V as a Value*, and return whether we looked
/// through any sign or zero extends.  The incoming Value is known to have
/// IntegerType and it may already be sign or zero extended.
///
/// Note that this looks through extends, so the high bits may not be
/// represented in the result.
Value* BasicAliasAnalysis::GetLinearExpression(Value *V, APInt &Scale, APInt &Offset,
					       ExtensionKind &Extension,
					       const TargetData &TD, unsigned Depth, IntegrationAttempt* ctx) {
  assert(V->getType()->isIntegerTy() && "Not an integer value");

  // Limit our recursion depth.
  if (Depth == 6) {
    Scale = 1;
    Offset = 0;
    return V;
  }
  
  if (BinaryOperator *BOp = dyn_cast<BinaryOperator>(V)) {
    ValCtx VC = make_vc(BOp->getOperand(1), ctx);
    if (ConstantInt *RHSC = dyn_cast<ConstantInt>(tryConstReplacement(VC).first)) {
      switch (BOp->getOpcode()) {
      default: break;
      case Instruction::Or:
        // X|C == X+C if all the bits in C are unset in X.  Otherwise we can't
        // analyze it.
        if (!MaskedValueIsZero(BOp->getOperand(0), RHSC->getValue(), &TD))
          break;
        // FALL THROUGH.
      case Instruction::Add:
        V = GetLinearExpression(BOp->getOperand(0), Scale, Offset, Extension,
                                TD, Depth+1, ctx);
        Offset += RHSC->getValue();
        return V;
      case Instruction::Mul:
        V = GetLinearExpression(BOp->getOperand(0), Scale, Offset, Extension,
                                TD, Depth+1, ctx);
        Offset *= RHSC->getValue();
        Scale *= RHSC->getValue();
        return V;
      case Instruction::Shl:
        V = GetLinearExpression(BOp->getOperand(0), Scale, Offset, Extension,
                                TD, Depth+1, ctx);
        Offset <<= RHSC->getValue().getLimitedValue();
        Scale <<= RHSC->getValue().getLimitedValue();
        return V;
      }
    }
  }
  
  // Since GEP indices are sign extended anyway, we don't care about the high
  // bits of a sign or zero extended value - just scales and offsets.  The
  // extensions have to be consistent though.
  if ((isa<SExtInst>(V) && Extension != EK_ZeroExt) ||
      (isa<ZExtInst>(V) && Extension != EK_SignExt)) {
    Value *CastOp = cast<CastInst>(V)->getOperand(0);
    unsigned OldWidth = Scale.getBitWidth();
    unsigned SmallWidth = CastOp->getType()->getPrimitiveSizeInBits();
    Scale.trunc(SmallWidth);
    Offset.trunc(SmallWidth);
    Extension = isa<SExtInst>(V) ? EK_SignExt : EK_ZeroExt;

    Value *Result = GetLinearExpression(CastOp, Scale, Offset, Extension,
                                        TD, Depth+1, ctx);
    Scale.zext(OldWidth);
    Offset.zext(OldWidth);
    
    return Result;
  }
  
  Scale = 1;
  Offset = 0;
  return V;
}

/// DecomposeGEPExpression - If V is a symbolic pointer expression, decompose it
/// into a base pointer with a constant offset and a number of scaled symbolic
/// offsets.
///
/// The scaled symbolic offsets (represented by pairs of a Value* and a scale in
/// the VarIndices vector) are Value*'s that are known to be scaled by the
/// specified amount, but which may have other unrepresented high bits. As such,
/// the gep cannot necessarily be reconstructed from its decomposed form.
///
/// When TargetData is around, this function is capable of analyzing everything
/// that Value::getUnderlyingObject() can look through.  When not, it just looks
/// through pointer casts.
///
const ValCtx BasicAliasAnalysis::DecomposeGEPExpression(const ValCtx FirstV, int64_t &BaseOffs,
							SmallVectorImpl<VariableGEPIndex> &VarIndices,
							const TargetData *TD) {
  // Limit recursion depth to limit compile time in crazy cases.
  unsigned MaxLookup = 1000;
  
  ValCtx V = FirstV;
  BaseOffs = 0;
  do {
    // See if this is a bitcast or GEP.
    const Operator *Op = dyn_cast<Operator>(V.first);
    if (Op == 0) {
      // The only non-operator case we can handle are GlobalAliases.
      if (GlobalAlias *GA = dyn_cast<GlobalAlias>(V.first)) {
        if (!GA->mayBeOverridden()) {
          V = make_vc(GA->getAliasee(), V.second);
          continue;
        }
      }
    }
    else if (Op->getOpcode() == Instruction::BitCast) {
      V = make_vc(Op->getOperand(0), V.second);
      continue;
    }
    
    const GEPOperator *GEPOp = dyn_cast_or_null<GEPOperator>(Op);
    if (GEPOp == 0) {
      ValCtx NewV = getReplacement(V);
      // Look through a resolved pointer if our parent has that information
      if(NewV == V)
	return V;
      else {
	V = NewV;
	continue;
      }
    }
    
    // Don't attempt to analyze GEPs over unsized objects.
    if (!cast<PointerType>(GEPOp->getOperand(0)->getType())
        ->getElementType()->isSized())
      return V;
    
    // If we are lacking TargetData information, we can't compute the offets of
    // elements computed by GEPs.  However, we can handle bitcast equivalent
    // GEPs.
    if (TD == 0) {
      if (!GEPHasAllZeroIndices(make_vc(const_cast<Value*>(cast<Value>(GEPOp)), V.second)))
        return V;
      V = make_vc(GEPOp->getOperand(0), V.second);
      continue;
    }
    
    // Walk the indices of the GEP, accumulating them into BaseOff/VarIndices.
    gep_type_iterator GTI = gep_type_begin(GEPOp);
    for (User::const_op_iterator I = GEPOp->op_begin()+1,
         E = GEPOp->op_end(); I != E; ++I) {
      ValCtx Index = tryConstReplacement(make_vc(*I, V.second));
      
      // Compute the (potentially symbolic) offset in bytes for this index.
      if (const StructType *STy = dyn_cast<StructType>(*GTI++)) {
        // For a struct, add the member offset.
        unsigned FieldNo = cast<ConstantInt>(Index.first)->getZExtValue();
        if (FieldNo == 0) continue;
        
        BaseOffs += TD->getStructLayout(STy)->getElementOffset(FieldNo);
        continue;
      }

      // For an array/pointer, add the element offset, explicitly scaled.

      if (ConstantInt* CIdx = dyn_cast<ConstantInt>(Index.first)) {
        if (CIdx->isZero()) continue;
        BaseOffs += TD->getTypeAllocSize(*GTI)*CIdx->getSExtValue();
        continue;
      }
      
      uint64_t Scale = TD->getTypeAllocSize(*GTI);
      ExtensionKind Extension = EK_NotExtended;
      
      // If the integer type is smaller than the pointer size, it is implicitly
      // sign extended to pointer size.
      unsigned Width = cast<IntegerType>(Index.first->getType())->getBitWidth();
      if (TD->getPointerSizeInBits() > Width)
        Extension = EK_SignExt;
      
      // Use GetLinearExpression to decompose the index into a C1*V+C2 form.
      APInt IndexScale(Width, 0), IndexOffset(Width, 0);
      Index = make_vc(GetLinearExpression(Index.first, IndexScale, IndexOffset, Extension, *TD, 0, V.second), V.second);
      
      // The GEP index scale ("Scale") scales C1*V+C2, yielding (C1*V+C2)*Scale.
      // This gives us an aggregate computation of (C1*Scale)*V + C2*Scale.
      BaseOffs += IndexOffset.getZExtValue()*Scale;
      Scale *= IndexScale.getZExtValue();
      
      
      // If we already had an occurrance of this index variable, merge this
      // scale into it.  For example, we want to handle:
      //   A[x][x] -> x*16 + x*4 -> x*20
      // This also ensures that 'x' only appears in the index list once.
      for (unsigned i = 0, e = VarIndices.size(); i != e; ++i) {
        if (VarIndices[i].VC == Index &&
            VarIndices[i].Extension == Extension) {
          Scale += VarIndices[i].Scale;
          VarIndices.erase(VarIndices.begin()+i);
          break;
        }
      }
      
      // Make sure that we have a scale that makes sense for this target's
      // pointer size.
      if (unsigned ShiftBits = 64-TD->getPointerSizeInBits()) {
        Scale <<= ShiftBits;
        Scale >>= ShiftBits;
      }
      
      if (Scale) {
        VariableGEPIndex Entry = {Index, Extension, Scale};
        VarIndices.push_back(Entry);
      }
    }
    
    // Analyze the base pointer next.
    V = make_vc(GEPOp->getOperand(0), V.second);
  } while (--MaxLookup);
  
  // If the chain of expressions is too deep, just return early.
  return V;
}

/// pointsToConstantMemory - Chase pointers until we find a (constant
/// global) or not.
bool BasicAliasAnalysis::pointsToConstantMemory(const Value *P) {
  if (const GlobalVariable *GV = 
        dyn_cast<GlobalVariable>(P->getUnderlyingObject()))
    // Note: this doesn't require GV to be "ODR" because it isn't legal for a
    // global to be marked constant in some modules and non-constant in others.
    // GV may even be a declaration, not a definition.
    return GV->isConstant();

  return NoAA::pointsToConstantMemory(P);
}

/// getModRefBehavior - Return the behavior when calling the given call site.
AliasAnalysis::ModRefBehavior
BasicAliasAnalysis::getModRefBehavior(ImmutableCallSite CS) {
  if (CS.doesNotAccessMemory())
    // Can't do better than this.
    return DoesNotAccessMemory;

  ModRefBehavior Min = UnknownModRefBehavior;

  // If the callsite knows it only reads memory, don't return worse
  // than that.
  if (CS.onlyReadsMemory())
    Min = OnlyReadsMemory;

  // The AliasAnalysis base class has some smarts, lets use them.
  return std::min(AliasAnalysis::getModRefBehavior(CS), Min);
}

/// getModRefBehavior - Return the behavior when calling the given function.
/// For use when the call site is not known.
AliasAnalysis::ModRefBehavior
BasicAliasAnalysis::getModRefBehavior(const Function *F) {
  if (F->doesNotAccessMemory())
    // Can't do better than this.
    return DoesNotAccessMemory;
  if (F->onlyReadsMemory())
    return OnlyReadsMemory;
  if (unsigned id = F->getIntrinsicID())
    return getIntrinsicModRefBehavior(id);

  return NoAA::getModRefBehavior(F);
}

/// getModRefInfo - Check to see if the specified callsite can clobber the
/// specified memory object.  Since we only look at local properties of this
/// function, we really can't say much about this query.  We do, however, use
/// simple "address taken" analysis on local objects.
AliasAnalysis::ModRefResult
BasicAliasAnalysis::getModRefInfo(ImmutableCallSite CS,
                                  const Value *P, unsigned Size, 
				  IntegrationAttempt* CSCtx, IntegrationAttempt* PCtx, bool usePBKnowledge) {
  assert((notDifferentParent(CS.getInstruction(), P) || (CSCtx && PCtx)) &&
         "AliasAnalysis query involving multiple functions!");

  // Either both values have a context or neither one does.
  assert(!!CSCtx == !!PCtx);

  if(CSCtx == PCtx) {

    const Value *Object = P->getUnderlyingObject();
  
    // If this is a tail call and P points to a stack location, we know that
    // the tail call cannot access or modify the local stack.
    // We cannot exclude byval arguments here; these belong to the caller of
    // the current function not to the current function, and a tail callee
    // may reference them.
    if (isa<AllocaInst>(Object))
      if (const CallInst *CI = dyn_cast<CallInst>(CS.getInstruction()))
	if (CI->isTailCall())
	  return NoModRef;
  
    // If the pointer is to a locally allocated object that does not escape,
    // then the call can not mod/ref the pointer unless the call takes the pointer
    // as an argument, and itself doesn't capture it.
    if (!isa<Constant>(Object) && CS.getInstruction() != Object &&
	isNonEscapingLocalObject(Object)) {
      bool PassedAsArg = false;
      unsigned ArgNo = 0;
      for (ImmutableCallSite::arg_iterator CI = CS.arg_begin(), CE = CS.arg_end();
	   CI != CE; ++CI, ++ArgNo) {
	// Only look at the no-capture pointer arguments.
	if (!(*CI)->getType()->isPointerTy() ||
	    !CS.paramHasAttr(ArgNo+1, Attribute::NoCapture))
	  continue;
      
	// If  this is a no-capture pointer argument, see if we can tell that it
	// is impossible to alias the pointer we're checking.  If not, we have to
	// assume that the call could touch the pointer, even though it doesn't
	// escape.
	if (!isNoAlias(make_vc(const_cast<Value*>(cast<Value>(CI)), CSCtx), UnknownSize, make_vc(const_cast<Value*>(P), PCtx), UnknownSize, usePBKnowledge)) {
	  PassedAsArg = true;
	  break;
	}
      }
    
      if (!PassedAsArg)
	return NoModRef;
    }

  }

  // Finally, handle specific knowledge of intrinsics.
  const IntrinsicInst *II = dyn_cast<IntrinsicInst>(CS.getInstruction());
  if (II != 0)
    switch (II->getIntrinsicID()) {
    default: break;
    case Intrinsic::memcpy:
    case Intrinsic::memmove: {
      unsigned Len = UnknownSize;
      if (ConstantInt *LenCI = cast_or_null<ConstantInt>(getConstReplacement(make_vc(II->getArgOperand(2), CSCtx))))
        Len = LenCI->getZExtValue();
      Value *Dest = II->getArgOperand(0);
      Value *Src = II->getArgOperand(1);
      if (isNoAlias(make_vc(Dest, CSCtx), Len, make_vc(const_cast<Value*>(P), PCtx), Size, usePBKnowledge)) {
        if (isNoAlias(make_vc(Src, CSCtx), Len, make_vc(const_cast<Value*>(P), PCtx), Size, usePBKnowledge))
          return NoModRef;
        return Ref;
      }
      break;
    }
    case Intrinsic::memset:
      // Since memset is 'accesses arguments' only, the AliasAnalysis base class
      // will handle it for the variable length case.
      if (ConstantInt *LenCI = cast_or_null<ConstantInt>(getConstReplacement(make_vc(II->getArgOperand(2), CSCtx)))) {
        unsigned Len = LenCI->getZExtValue();
        Value *Dest = II->getArgOperand(0);
        if (isNoAlias(make_vc(Dest, CSCtx), Len, make_vc(const_cast<Value*>(P), PCtx), Size, usePBKnowledge))
          return NoModRef;
      }
      break;
    case Intrinsic::atomic_cmp_swap:
    case Intrinsic::atomic_swap:
    case Intrinsic::atomic_load_add:
    case Intrinsic::atomic_load_sub:
    case Intrinsic::atomic_load_and:
    case Intrinsic::atomic_load_nand:
    case Intrinsic::atomic_load_or:
    case Intrinsic::atomic_load_xor:
    case Intrinsic::atomic_load_max:
    case Intrinsic::atomic_load_min:
    case Intrinsic::atomic_load_umax:
    case Intrinsic::atomic_load_umin:
      if (TD) {
        Value *Op1 = II->getArgOperand(0);
        unsigned Op1Size = TD->getTypeStoreSize(Op1->getType());
        if (isNoAlias(make_vc(Op1, CSCtx), Op1Size, make_vc(const_cast<Value*>(P), PCtx), Size, usePBKnowledge))
          return NoModRef;
      }
      break;
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::invariant_start: {
      unsigned PtrSize =
        cast<ConstantInt>(II->getArgOperand(0))->getZExtValue();
      if (isNoAlias(make_vc(II->getArgOperand(1), CSCtx), PtrSize, make_vc(const_cast<Value*>(P), PCtx), Size, usePBKnowledge))
        return NoModRef;
      break;
    }
    case Intrinsic::invariant_end: {
      unsigned PtrSize =
        cast<ConstantInt>(II->getArgOperand(1))->getZExtValue();
      if (isNoAlias(make_vc(II->getArgOperand(2), CSCtx), PtrSize, make_vc(const_cast<Value*>(P), PCtx), Size, usePBKnowledge))
        return NoModRef;
      break;
    }
    }

  // The AliasAnalysis base class has some smarts, lets use them.
  return AliasAnalysis::getModRefInfo(CS, P, Size, CSCtx, PCtx, usePBKnowledge);
}


/// aliasGEP - Provide a bunch of ad-hoc rules to disambiguate a GEP instruction
/// against another pointer.  We know that V1 is a GEP, but we don't know
/// anything about V2.  UnderlyingV1 is GEP1->getUnderlyingObject(),
/// UnderlyingV2 is the same for V2.
///
AliasAnalysis::AliasResult
BasicAliasAnalysis::aliasGEP(const ValCtx V1, unsigned V1Size,
                             const ValCtx V2, unsigned V2Size,
                             const ValCtx UnderlyingV1,
                             const ValCtx UnderlyingV2) {

  const GEPOperator* GEP1 = cast<GEPOperator>(V1.first);
  // If this GEP has been visited before, we're on a use-def cycle.
  // Such cycles are only valid when PHI nodes are involved or in unreachable
  // code. The visitPHI function catches cycles containing PHIs, but there
  // could still be a cycle without PHIs in unreachable code.
  if (!Visited.insert(GEP1))
    return MayAlias;

  int64_t GEP1BaseOffset;
  SmallVector<VariableGEPIndex, 4> GEP1VariableIndices;

  // If we have two gep instructions with must-alias'ing base pointers, figure
  // out if the indexes to the GEP tell us anything about the derived pointer.
  if (isa<GEPOperator>(V2.first)) {
    // Do the base pointers alias?
    AliasResult BaseAlias = aliasCheck(UnderlyingV1, UnknownSize,
                                       UnderlyingV2, UnknownSize);
    
    // If we get a No or May, then return it immediately, no amount of analysis
    // will improve this situation.
    if (BaseAlias != MustAlias) return BaseAlias;
    
    // Otherwise, we have a MustAlias.  Since the base pointers alias each other
    // exactly, see if the computed offset from the common pointer tells us
    // about the relation of the resulting pointer.
    const ValCtx GEP1BasePtr =
      DecomposeGEPExpression(V1, GEP1BaseOffset, GEP1VariableIndices, TD);
    
    int64_t GEP2BaseOffset;
    SmallVector<VariableGEPIndex, 4> GEP2VariableIndices;
    const ValCtx GEP2BasePtr =
      DecomposeGEPExpression(V2, GEP2BaseOffset, GEP2VariableIndices, TD);
    
    // If DecomposeGEPExpression isn't able to look all the way through the
    // addressing operation, we must not have TD and this is too complex for us
    // to handle without it.
    if (GEP1BasePtr != UnderlyingV1 || GEP2BasePtr != UnderlyingV2) {
      assert(TD == 0 &&
             "DecomposeGEPExpression and getUnderlyingObject disagree!");
      return MayAlias;
    }
    
    // Subtract the GEP2 pointer from the GEP1 pointer to find out their
    // symbolic difference.
    GEP1BaseOffset -= GEP2BaseOffset;
    GetIndexDifference(GEP1VariableIndices, GEP2VariableIndices);
    
  } else {
    // Check to see if these two pointers are related by the getelementptr
    // instruction.  If one pointer is a GEP with a non-zero index of the other
    // pointer, we know they cannot alias.

    // If both accesses are unknown size, we can't do anything useful here.
    if (V1Size == UnknownSize && V2Size == UnknownSize)
      return MayAlias;

    AliasResult R = aliasCheck(UnderlyingV1, UnknownSize, V2, V2Size);
    if (R != MustAlias)
      // If V2 may alias GEP base pointer, conservatively returns MayAlias.
      // If V2 is known not to alias GEP base pointer, then the two values
      // cannot alias per GEP semantics: "A pointer value formed from a
      // getelementptr instruction is associated with the addresses associated
      // with the first operand of the getelementptr".
      return R;

    const ValCtx GEP1BasePtr =
      DecomposeGEPExpression(V1, GEP1BaseOffset, GEP1VariableIndices, TD);
    
    // If DecomposeGEPExpression isn't able to look all the way through the
    // addressing operation, we must not have TD and this is too complex for us
    // to handle without it.
    if (GEP1BasePtr != UnderlyingV1) {
      assert(TD == 0 &&
             "DecomposeGEPExpression and getUnderlyingObject disagree!");
      return MayAlias;
    }
  }
  
  // In the two GEP Case, if there is no difference in the offsets of the
  // computed pointers, the resultant pointers are a must alias.  This
  // hapens when we have two lexically identical GEP's (for example).
  //
  // In the other case, if we have getelementptr <ptr>, 0, 0, 0, 0, ... and V2
  // must aliases the GEP, the end result is a must alias also.
  if (GEP1BaseOffset == 0 && GEP1VariableIndices.empty())
    return MustAlias;

  // If we have a known constant offset, see if this offset is larger than the
  // access size being queried.  If so, and if no variable indices can remove
  // pieces of this constant, then we know we have a no-alias.  For example,
  //   &A[100] != &A.
  
  // In order to handle cases like &A[100][i] where i is an out of range
  // subscript, we have to ignore all constant offset pieces that are a multiple
  // of a scaled index.  Do this by removing constant offsets that are a
  // multiple of any of our variable indices.  This allows us to transform
  // things like &A[i][1] because i has a stride of (e.g.) 8 bytes but the 1
  // provides an offset of 4 bytes (assuming a <= 4 byte access).
  for (unsigned i = 0, e = GEP1VariableIndices.size();
       i != e && GEP1BaseOffset;++i)
    if (int64_t RemovedOffset = GEP1BaseOffset/GEP1VariableIndices[i].Scale)
      GEP1BaseOffset -= RemovedOffset*GEP1VariableIndices[i].Scale;
  
  // If our known offset is bigger than the access size, we know we don't have
  // an alias.
  if (GEP1BaseOffset) {
    if (GEP1BaseOffset >= (int64_t)V2Size ||
        GEP1BaseOffset <= -(int64_t)V1Size)
      return NoAlias;
  }
  
  return MayAlias;
}

/// aliasSelect - Provide a bunch of ad-hoc rules to disambiguate a Select
/// instruction against another.
AliasAnalysis::AliasResult
BasicAliasAnalysis::aliasSelect(const ValCtx V1, unsigned SISize,
                                const ValCtx V2, unsigned V2Size) {
  
  SelectInst* SI = cast<SelectInst>(V1.first);
  // If this select has been visited before, we're on a use-def cycle.
  // Such cycles are only valid when PHI nodes are involved or in unreachable
  // code. The visitPHI function catches cycles containing PHIs, but there
  // could still be a cycle without PHIs in unreachable code.
  if (!Visited.insert(SI))
    return MayAlias;

  ConstantInt* SICond = cast_or_null<ConstantInt>(getConstReplacement(make_vc(SI->getCondition(), V1.second)));
  if(SICond) {
    if(SICond == ConstantInt::getTrue(SI->getContext()))
      return aliasCheck(make_vc(SI->getTrueValue(), V1.second), SISize, V2, V2Size);
    else
      return aliasCheck(make_vc(SI->getFalseValue(), V1.second), SISize, V2, V2Size);
  }

  // If the values are Selects with the same condition, we can do a more precise
  // check: just check for aliases between the values on corresponding arms.
  if (SelectInst *SI2 = dyn_cast<SelectInst>(V2.first))
    if (getReplacement(make_vc(SI->getCondition(), V1.second)) == getReplacement(make_vc(SI2->getCondition(), V2.second))) {
      AliasResult Alias =
        aliasCheck(make_vc(SI->getTrueValue(), V1.second), SISize,
                   make_vc(SI2->getTrueValue(), V2.second), V2Size);
      if (Alias == MayAlias)
        return MayAlias;
      AliasResult ThisAlias =
        aliasCheck(make_vc(SI->getFalseValue(), V1.second), SISize,
                   make_vc(SI2->getFalseValue(), V2.second), V2Size);
      if (ThisAlias != Alias)
        return MayAlias;
      return Alias;
    }

  // If both arms of the Select node NoAlias or MustAlias V2, then returns
  // NoAlias / MustAlias. Otherwise, returns MayAlias.
  AliasResult Alias =
    aliasCheck(V2, V2Size, make_vc(SI->getTrueValue(), V1.second), SISize);
  if (Alias == MayAlias)
    return MayAlias;

  // If V2 is visited, the recursive case will have been caught in the
  // above aliasCheck call, so these subsequent calls to aliasCheck
  // don't need to assume that V2 is being visited recursively.
  Visited.erase(V2.first);

  AliasResult ThisAlias =
    aliasCheck(V2, V2Size, make_vc(SI->getFalseValue(), V1.second), SISize);
  if (ThisAlias != Alias)
    return MayAlias;
  return Alias;
}

// aliasPHI - Provide a bunch of ad-hoc rules to disambiguate a PHI instruction
// against another.
AliasAnalysis::AliasResult
BasicAliasAnalysis::aliasPHI(const ValCtx V1, unsigned PNSize,
                             const ValCtx V2, unsigned V2Size) {
  
  PHINode* PN = cast<PHINode>(V1.first);
  // The PHI node has already been visited, avoid recursion any further.
  if (!Visited.insert(PN))
    return MayAlias;

  // If the values are PHIs in the same block, we can do a more precise
  // as well as efficient check: just check for aliases between the values
  // on corresponding edges.

  const BasicBlock* PNParent = PN->getParent();

  if (const PHINode *PN2 = dyn_cast<PHINode>(V2.first))
    if (PN2->getParent() == PNParent && V1.second == V2.second) {
      bool AliasValid = false;
      AliasAnalysis::AliasResult Alias = MayAlias;
      for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
	BasicBlock* PredBB = PN->getIncomingBlock(i);
	if(V1.second && V1.second->edgeIsDead(PredBB, const_cast<BasicBlock*>(PNParent)))
	  continue;
        AliasResult ThisAlias =
          aliasCheck(make_vc(PN->getIncomingValue(i), V1.second), PNSize,
                     make_vc(PN2->getIncomingValueForBlock(PN->getIncomingBlock(i)), V2.second /* == V1.second */), V2Size);
	if(!AliasValid) {
	  AliasValid = true;
	  Alias = ThisAlias;
	}
        else if (ThisAlias != Alias)
          return MayAlias;
      }
      return Alias;
    }

  SmallPtrSet<Value*, 4> UniqueSrc;
  SmallVector<Value*, 4> V1Srcs;
  for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
    Value *PV1 = PN->getIncomingValue(i);
    if (isa<PHINode>(PV1))
      // If any of the source itself is a PHI, return MayAlias conservatively
      // to avoid compile time explosion. The worst possible case is if both
      // sides are PHI nodes. In which case, this is O(m x n) time where 'm'
      // and 'n' are the number of PHI sources.
      return MayAlias;
    if (UniqueSrc.insert(PV1))
      V1Srcs.push_back(PV1);
  }

  AliasResult Alias = aliasCheck(V2, V2Size, make_vc(V1Srcs[0], V1.second), PNSize);
  // Early exit if the check of the first PHI source against V2 is MayAlias.
  // Other results are not possible.
  if (Alias == MayAlias)
    return MayAlias;

  // If all sources of the PHI node NoAlias or MustAlias V2, then returns
  // NoAlias / MustAlias. Otherwise, returns MayAlias.
  for (unsigned i = 1, e = V1Srcs.size(); i != e; ++i) {
    Value *V = V1Srcs[i];

    // If V2 is visited, the recursive case will have been caught in the
    // above aliasCheck call, so these subsequent calls to aliasCheck
    // don't need to assume that V2 is being visited recursively.
    Visited.erase(V2.first);

    AliasResult ThisAlias = aliasCheck(V2, V2Size, make_vc(V, V1.second), PNSize);
    if (ThisAlias != Alias || ThisAlias == MayAlias)
      return MayAlias;
  }

  return Alias;
}

// A cowardly duplication of Value::getUnderlyingObject, to avoid potentially screwups
// in modifying Value, which is used throughout LLVM.
Value* BasicAliasAnalysis::getUnderlyingObject(ValCtx VIn, bool& isOffset, bool idOnly) {
  
  unsigned MaxLookup = 1000;
  if (!VIn.first->getType()->isPointerTy())
    return VIn.first;
  Value *V = VIn.first;
  for (unsigned Count = 0; MaxLookup == 0 || Count < MaxLookup; ++Count) {
    if (GEPOperator *GEP = dyn_cast<GEPOperator>(V)) {

      // This check turns out to be important: otherwise we might conclude that we need to 
      // check the aliasGEP path but then strip the all-zero GEP using Value::stripPointerCasts()
      // which regards such pointless GEPs as casts.
      // Then we end up with two non-GEP, non-PHI, non-Select instructions and fall through to MayAlias.
      // In summary: if we're going to conclude that two things Must-Alias due to referring 
      // to the same object without an offset, we must do so NOW.
      if(!GEPHasAllZeroIndices(make_vc(GEP, VIn.second))) {
	isOffset = true;
	if(idOnly)
	  return V;
      }

      V = GEP->getPointerOperand();

    } else if (Operator::getOpcode(V) == Instruction::BitCast) {
      V = cast<Operator>(V)->getOperand(0);
    } else if (GlobalAlias *GA = dyn_cast<GlobalAlias>(V)) {
      if (GA->mayBeOverridden())
        return V;
      V = GA->getAliasee();
    } else {
      return V;
    }
    assert(V->getType()->isPointerTy() && "Unexpected operand type!");
  }
  return V;  

}

ValCtx
BasicAliasAnalysis::getUltimateUnderlyingObject(ValCtx V, bool& isOffset, bool idOnly) {

  isOffset = false;
  ValCtx CurrentV = V;
  while(1) {

    // This might set isOffset if we look through a GEP.
    Value* O = getUnderlyingObject(CurrentV, isOffset, idOnly);
    // Note here: getUnderlyingObject might take us out a scope, e.g. by a loop-variant GEP referencing a loop-invariant load instruction!
    // This is okay, because Loop iterations are already expected to resolve invariants using the appropriate parent scope, so "inst at scope X"
    // is transparently proxied as "inst at scope X + n".
    ValCtx NewV = getReplacement(make_vc(O, CurrentV.second));
    if(NewV == make_vc(O, CurrentV.second))
      return NewV;
    CurrentV = NewV;

  }

}

ValCtx
BasicAliasAnalysis::getFirstOffset(ValCtx V) {

  bool ignored;
  return getUltimateUnderlyingObject(V, ignored, true);

}

// aliasCheck - Provide a bunch of ad-hoc rules to disambiguate in common cases,
// such as array references.
//
AliasAnalysis::AliasResult
BasicAliasAnalysis::aliasCheck(ValCtx V1, unsigned V1Size,
                               ValCtx V2, unsigned V2Size) {
  // If either of the memory references is empty, it doesn't matter what the
  // pointer values are.
  if (V1Size == 0 || V2Size == 0)
    return NoAlias;

  if (!V1.first->getType()->isPointerTy() || !V2.first->getType()->isPointerTy())
    return NoAlias;  // Scalars cannot alias each other

  // Figure out what objects these things are pointing to if we can.
  bool UO1Offset = false, UO2Offset = false;
  ValCtx UO1 = getUltimateUnderlyingObject(V1, UO1Offset);
  Value* O1 = UO1.first;
  ValCtx UO2 = getUltimateUnderlyingObject(V2, UO2Offset);
  Value* O2 = UO2.first;

  // Are we checking for alias of the same value?
  if (UO1 == UO2 && (!UO1Offset) && (!UO2Offset)) return MustAlias;

  // Otherwise either the pointers are based off potentially different objects,
  // or else they're potentially different derived pointers off the same base.

  // Strip off any casts and other identity operations if they exist.
  V1 = getFirstOffset(V1);
  V2 = getFirstOffset(V2);

  // Null values in the default address space don't point to any object, so they
  // don't alias any other pointer.
  if (const ConstantPointerNull *CPN = dyn_cast<ConstantPointerNull>(O1))
    if (CPN->getType()->getAddressSpace() == 0)
      return NoAlias;
  if (const ConstantPointerNull *CPN = dyn_cast<ConstantPointerNull>(O2))
    if (CPN->getType()->getAddressSpace() == 0)
      return NoAlias;

  if (UO1 != UO2) {
    // If V1/V2 point to two different objects we know that we have no alias.
    if (isIdentifiedObject(O1) && isIdentifiedObject(O2))
      return NoAlias;

    // Constant pointers can't alias with non-const isIdentifiedObject objects.
    if ((isa<Constant>(O1) && isIdentifiedObject(O2) && !isa<Constant>(O2)) ||
        (isa<Constant>(O2) && isIdentifiedObject(O1) && !isa<Constant>(O1)))
      return NoAlias;

    // Arguments can't alias with local allocations or noalias calls
    // in the same function.
    if (((isa<Argument>(O1) && (isa<AllocaInst>(O2) || isNoAliasCall(O2))) ||
         (isa<Argument>(O2) && (isa<AllocaInst>(O1) || isNoAliasCall(O1))))
	&& (UO1.second == UO2.second))
      return NoAlias;

    // Most objects can't alias null.
    if ((isa<ConstantPointerNull>(O2) && isKnownNonNull(O1)) ||
        (isa<ConstantPointerNull>(O1) && isKnownNonNull(O2)))
      return NoAlias;
  
    // If one pointer is the result of a call/invoke or load and the other is a
    // non-escaping local object within the same function, then we know the
    // object couldn't escape to a point where the call could return it.
    //
    // Note that if the pointers are in different functions, there are a
    // variety of complications. A call with a nocapture argument may still
    // temporary store the nocapture argument's value in a temporary memory
    // location if that memory location doesn't escape. Or it may pass a
    // nocapture value to other functions as long as they don't capture it.
    if (isEscapeSource(O1) && isNonEscapingLocalObject(O2) && UO1.second == UO2.second)
      return NoAlias;
    if (isEscapeSource(O2) && isNonEscapingLocalObject(O1) && UO1.second == UO2.second)
      return NoAlias;
  }

  // If the size of one access is larger than the entire object on the other
  // side, then we know such behavior is undefined and can assume no alias.
  if (TD)
    if ((V1Size != UnknownSize && isObjectSmallerThan(O2, V1Size, *TD)) ||
        (V2Size != UnknownSize && isObjectSmallerThan(O1, V2Size, *TD)))
      return NoAlias;
  
  // FIXME: This isn't aggressively handling alias(GEP, PHI) for example: if the
  // GEP can't simplify, we don't even look at the PHI cases.
  if (!isa<GEPOperator>(V1.first) && isa<GEPOperator>(V2.first)) {
    std::swap(V1, V2);
    std::swap(V1Size, V2Size);
    std::swap(UO1, UO2);
  }
  if (isa<GEPOperator>(V1.first))
    return aliasGEP(V1, V1Size, V2, V2Size, UO1, UO2);

  if (isa<PHINode>(V2.first) && !isa<PHINode>(V1.first)) {
    std::swap(V1, V2);
    std::swap(V1Size, V2Size);
  }
  if (isa<PHINode>(V1.first))
    return aliasPHI(V1, V1Size, V2, V2Size);

  if (isa<SelectInst>(V2.first) && !isa<SelectInst>(V1.first)) {
    std::swap(V1, V2);
    std::swap(V1Size, V2Size);
  }
  if (isa<SelectInst>(V1.first))
    return aliasSelect(V1, V1Size, V2, V2Size);

  // This throws away important information, but it doesn't matter as NoAA just returns MayAlias.
  return NoAA::alias(V1.first, V1Size, V2.first, V2Size);
}

// Make sure that anything that uses AliasAnalysis pulls in this file.
DEFINING_FILE_FOR(BasicAliasAnalysis)
