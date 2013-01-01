
#include "llvm/Analysis/HypotheticalConstantFolder.h"
#include "llvm/BasicBlock.h"

_BIC::_BIC(Instruction* I, IntegrationAttempt* _ctx) : it(BasicBlock::iterator(I)), BB(I->getParent(), ctx(_ctx) { }

//// Implement the backward walker:

BackwardIAWalker::BackwardIAWalker(Instruction* I, IntegrationAttempt* Ctx, bool skipFirst) {

  PList = &Worklist1;
  CList = &Worklist2;
  BasicBlock::iterator startit(I);
  if(!skipFirst)
    --startit;
  PList->push_back(BIC(startit, I->getParent(), Ctx));
  Visited.insert(BIC(startit, I->getParent(), Ctx));

}

void IntegrationAttempt::queueLoopExitingBlocksBW(BasicBlock* ExitedBB, BasicBlock* ExitingBB, const Loop* ExitingBBL, BackwardIAWalker* Walker) {

  const Loop* MyL = getLoopContext();
  if(MyL == ExitingBBL) {

    if(!edgeIsDead(ExitingBB, ExitedBB))
      Walker->queueWalkFrom(BIC(ExitingBB->end(), ExitingBB, this));

  }
  else {

    const Loop* ChildL = immediateChildLoop(MyL, ExitingBBL);
    if(PeelAttempt* LPA = ThisStart.ctx->getPeelAttempt(ChildL)) {

      for(unsigned i = 0; i < LPA->Iterations.size(); ++i)
	LPA->Iterations[i]->queueLoopExitingBlocksBW(ExitedBB, ExitingBB, ExitingBBL, Walker);

    }
    else {

      Walker->queueWalkFrom(BIC(ExitingBB->end(), ExitingBB, this));

    }

  }

}

void InlineAttempt::queuePredecessorsBW(BasicBlock* FromBB, BackwardIAWalker* Walker) {

  if(FromBB == F.getEntryBlock() && parent) {

    Walker->queueWalkFrom(BIC(BasicBlock::iterator(CI), CI->getParent(), parent));

  }
  else {

    queueNormalPredecessorsBW(FromBB, Walker);

  }

}

void PeelIteration::queuePredecessorsBW(BasicBlock* FromBB, BackwardIAWalker* Walker) {

  if(FromBB == L->getHeader()) {

    if(iterationCount == 0) {

      Walker->queueWalkFrom(BIC(L->getLoopPreheader()->end(), L->getLoopPreheader(), parent));

    }
    else {

      Walker->queueWalkFrom(BIC(L->getLoopLatch()->end(), L->getLoopLatch(), parentPA->Iterations[iterationCount - 1]));

    }

  }
  else {

    queueNormalPredecessorsBW(FromBB, Walker);

  }

}

void IntegrationAttempt::queueNormalPredecessorsBW(BasicBlock* FromBB, BackwardIAWalker* Walker) {

  // This isn't the function entry block and isn't our loop header. Queue all predecessors.

  const Loop* CtxLoop = getLoopContext();
  const Loop* FromBBLoop = getBlockScopeVariant(FromBB);

  for(pred_iterator PI = pred_begin(FromBB), PE = pred_end(FromBB), PI != PE; ++PI) {

    bool queueHere = false;

    BasicBlock* BB = *PI;
    // CtxLoop != FromBBLoop indicates we're looking at loop blocks in an invariant context,
    // which in turn implies there's no point trying to climb into FromBBLoop or any of its
    // children.
    if(CtxLoop != FromBBLoop) {

      queueHere = true;
    
    }
    else {

      const Loop* BBLoop = ThisStart.ctx->getBlockScopeVariant(BB);
      if(BBLoop == CtxLoop) {

	queueHere = true;

      }
      else {

	// Must be a child loop; could be several loops deep however.
	ThisStart.ctx->queueLoopExitingBlocksBW(ThisStart.BB, BB, BBLoop, this);
	      
      }

    }

    if(queueHere) {

      if(edgeIsDead(BB, FromBB))
	continue;
      Walker->queueWalkFrom(BIC(BB->end(), BB, this));

    }

  }

}

void IAWalker::queueWalkFrom(BIC bic) {

  if(Visited.insert(bic))
    PList->push_back(bic);

}

BackwardIAWalker::BackwardIAWalker(Instruction* I, IntegrationAttempt* IA, bool skipFirst) {

  BasicBlock::iterator it(I);
  if(!skipFirst)
    ++it;

  BIC bic(it, I->getParent(), IA);
  Visited.insert(bic);
  PList->push_back(bic);

}

void BackwardIAWalker::walk() {

  while(PList->size() || CList->size()) {

    for(unsigned i = 0; i < CList->size(); ++i) {

      BIC ThisStart = (*CList)[i];
      CallInst* StoppedCI = 0;
      WalkInstructionResult thisBlockResult = walkFromInst(ThisStart, StoppedCI);

      if(thisBlockResult == WIRStopThisPath)
	continue;
      else if(thisBlockResult == WIRStopWholeWalk)
	return;

      // Else we walked up to either a call instruction or the top of the block
      // and should consider the predecessors.

      if(StoppedCI) {

	// Enter this call instruction from its return blocks:
	if(InlineAttempt* IA = ThisStart.ctx->getInlineAttempt(StoppedCI)) {

	  for(Function::iterator FI = IA->F.begin(), FE = IA->F.end(); FI != FE; ++FI) {

	    BasicBlock* BB = FI;
	    if(isa<ReturnInst>(BB->getTerminator()) && !IA->blockIsDead(BB)) {

	      queueWalkFrom(BIC(BB->end(), BB, IA));

	    }

	  }

	}
	else {

	  // Return value = should we abort?
	  if(blockedByUnexpandedCall(StoppedCI, ThisStart.ctx))
	    return;

	}

      }
      else {

	// Else we've hit the top of a block. Figure out what to do with each predecessor:
	ThisStart.ctx->queuePredecessorsBW(ThisStart.BB, this);

      }

    }

    CList->clear();
    std::swap(PList, CList);

  }

}

WalkInstructionResult BackwardIAWalker::walkFromInst(BIC bic, CallInst*& StoppedCI) {

  BasicBlock::iterator it = bic.it, itend = bic.BB->begin();
  
  while(1) {
    
    --it;
    if(it == itend)
      break;

    Instruction* I = it;
    WalkInstructionResult WIR = walkInstruction(I, bic.ctx);
    if(WIR != WIRContinue)
      return WIR;

    if(CallInst* CI = dyn_cast<CallInst>(CI)) {

      if(!shouldEnterCall(CI, bic.ctx))
	continue;

      StoppedCI = CI;
      break;

    }

  }

  return WIRContinue;

}

//// End backward walker.

//// Implement the forward walker:

ForwardIAWalker::ForwardIAWalker(Instruction* I, IntegrationAttempt* IA, bool skipFirst) {

  BasicBlock::iterator it(I);
  if(skipFirst)
    ++it;

  BIC bic(it, I->getParent(), IA);
  Visited.insert(bic);
  PList->push_back(bic);
  
}

void ForwardIAWalker::walk() {

  while(PList->size() || CList->size()) {

    for(unsigned i = 0; i < CList->size(); ++i) {

      BIC ThisStart = (*CList)[i];
      CallInst* StoppedCI = 0;
      WalkInstructionResult thisBlockResult = walkFromInst(ThisStart, StoppedCI);

      if(thisBlockResult == WIRStopThisPath)
	continue;
      else if(thisBlockResult == WIRStopWholeWalk)
	return;

      // Else we walked to either a call instruction or the bottom of the block
      // and should consider the successors.

      if(StoppedCI) {

	// Enter this call instruction from its entry block:
	if(InlineAttempt* IA = ThisStart.ctx->getInlineAttempt(StoppedCI)) {

	  BasicBlock* BB = IA->F.getEntryBlock();
	  queueWalkFrom(BIC(BB->begin(), BB, IA));

	}
	else {

	  // Return value = should we abort?
	  if(blockedByUnexpandedCall(StoppedCI, ThisStart.ctx))
	    return;

	}

      }
      else {

	// Else we've hit the bottom of a block. Figure out what to do with each successor:
	ThisStart.ctx->queueSuccessorsFW(ThisStart.BB, this);

      }

    }

    CList->clear();
    std::swap(PList, CList);

  }

}

WalkInstructionResult ForwardIAWalker::walkFromInst(BIC bic, CallInst*& StoppedCI) {

  for(BasicBlock::iterator it = bic.it, itend = bic.BB->end(); it != itend; ++it) {
    
    Instruction* I = it;
    WalkInstructionResult WIR = walkInstruction(I, bic.ctx);
    if(WIR != WIRContinue)
      return WIR;

    if(CallInst* CI = dyn_cast<CallInst>(CI)) {

      if(!shouldEnterCall(CI, bic.ctx))
	continue;

      StoppedCI = CI;
      break;

    }

  }

  return WIRContinue;

}

void IntegrationAttempt::queueSuccessorsFWFalling(BasicBlock* BB, const Loop* SuccLoop, ForwardIAWalker* Walker) {

  if(SuccLoop == getLoopContext()) {

    Walker->queueWalkFrom(BB->begin(), BB, this);

  }
  else {

    parent->queueSuccessorFWFalling(BB, SuccLoop, Walker);

  }

}

void InlineAttempt::queueSuccessorsFW(BasicBlock* BB, ForwardIAWalker* Walker) {

  if(isa<ReturnInst>(BB->getTerminator())) {

    if(parent) {

      BasicBlock::iterator CallIt(CI);
      ++CallIt;
      Walker->queueWalkFrom(CallIt, CI->getParent(), parent);

    }

    return;

  }

  IntegrationAttempt::queueSuccessorsFW(BB, Walker);

}

// Note here that the forward IA walker, when confronted with an unterminated loop, will first walk
// through all iterations which have been analysed seperately, then if we run off the end, through the
// loop in parent context, representing the general case.
// This gives maximum precision: if we analysed the first 3 iterations and we can show some property
// along all live paths without reaching the 4th, we can use that knowledge. Only if we find a live
// edge leading into the 4th do we consider it and all future iterations.
bool PeelIteration::queueNextLoopIterationFW(BasicBlock* PresentBlock, BasicBlock* NextBlock, ForwardIAWalker* Walker) {

  if(PresentBlock == L->getLoopLatch() && NextBlock == L->getHeader()) {

    PeelIteration* nextIter = getNextIteration();
    if(!nextIter) {

      LPDEBUG("FIAW: Analysing loop in parent context because loop " << L->getHeader()->getName() << " does not yet have iteration " << iterationCount+1 << "\n");
      Walker->queueWalkFrom(NextBlock->begin(), NextBlock, parent);

    }
    else {

      Walker->queueWalkFrom(NextBlock->begin(), NextBlock, nextIter);

    }

    return true;

  }

  return false;

}

bool InlineAttempt::queueNextLoopIterationFW(BasicBlock* PresentBlock, BasicBlock* NextBlock, ForwardIAWalker* Walker) {

  return false;
  
}

void IntegrationAttempt::queueSuccessorsFW(BasicBlock* BB, ForwardIAWalker* Walker) {

  const Loop* MyLoop = getLoopContext();
  const Loop* BBLoop = getBlockScopeVariant(BB);

  for(succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE; ++SI) {

    BasicBlock* SB = *SI;

    if(edgeIsDead(BB, SB))
      continue;

    if(queueNextLoopIterationFW(BB, SB, Walker))
      continue;

    bool queueHere = false;

    if(MyLoop != BBLoop) {

      // Already running in "wrong" context, don't rise.
      queueHere = true;

    }
    else {

      const Loop* SuccLoop = getBlockScopeVariant(SB);
      if(SuccLoop != MyLoop) {

	if((!MyLoop) || MyLoop->contains(SuccLoop)) {

	  if(PeelAttempt* LPA = getPeelAttempt(SuccLoop)) {

	    assert(SuccLoop->getHeader() == SB);
	    Walker->queueWalkFrom(SB->begin(), SB, LPA->Iterations[0]);

	  }

	}
	else {

	  // Loop exit edge. Find the context for the outside block:
	  queueSuccessorsFWFalling(SB->begin(), SuccLoop);

	}

      }
      else {

	queueHere = true;

      }

    }

    if(queueHere)
      Walker->queueWalkFrom(SB->begin(), SB, this);

  }

}
