// Functions to describe the hierarchy of peel and inline attempts in DOT format for easy review.

#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
#include "llvm/Instruction.h"

#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Analysis/HypotheticalConstantFolder.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <string>

using namespace llvm;

std::string IntegrationAttempt::getValueColour(ShadowValue SV, std::string& textColour) {

  // How should the instruction be coloured:
  // Bright green: defined here, i.e. it's a loop invariant.
  // Red: killed here or as an invariant (including dead memops)
  // Yellow: Expanded call instruction
  // Pink: Unexpanded call instruction
  // Lime green: Invariant defined above.
  // Dark green: Pointer base known
  // Grey: part of a dead block.

  InstArgImprovement* IAI = SV.getIAI();
  ShadowInstruction* SI = SV.getInst();
  
  if(!IAI)
    return "#aaaaaa";

  if(SI && (inst_is<LoadInst>(SI) || SI->isCopyInst())) {
    if(SI->isThreadLocal == TLS_MUSTCHECK)
      return "orangered";
  }

  if(willBeDeleted(SV))
    return "red";

  if(ShadowInstruction* SI = SV.getInst()) {

    if(GlobalIHP->barrierInstructions.count(SI)) {
      textColour = "white";
      return "black";
    }

  }

  if(val_is<CallInst>(SV)) {
    if(inlineChildren.find(SV.getInst()) != inlineChildren.end())
      return "yellow";
    else
      return "pink";
  }

  if(getConstReplacement(SV))
    return "green";

  if(IAI->PB) {
    ImprovedValSetSingle* IVS = dyn_cast<ImprovedValSetSingle>(IAI->PB);
    if((!IVS) || (IVS->Values.size() != 0 && !IVS->Overdef))
      return "darkgreen";
  }

  return "white";

}

static std::string TruncStr(std::string str, unsigned maxlen) {

  if(str.size() > maxlen) {

    str.resize(maxlen);
    str.append(" ...");

  }

  return str;

}

static std::string escapeHTML(std::string Str) {

  for (unsigned i = 0; i != Str.length(); ++i) {
    switch (Str[i]) {
    case '&':
      Str.replace(i, 1, "&amp;");
      i += 4;
      break;
    case '\\':
      Str.insert(Str.begin()+i, '\\');
      ++i;
      break;
    case '\t':
      Str.insert(Str.begin()+i, ' ');  // Convert to two spaces
      ++i;
      Str[i] = ' ';
      break;
    case '<': 
      Str.replace(i, 1, "&lt;");
      i += 3;
      break;
    case '>':
      Str.replace(i, 1, "&gt;");
      i += 3;
      break;
    case '"':
      Str.replace(i, 1, "&quot;");
      i += 5;
      break;
    }
  }

  return Str;

}

static std::string escapeHTMLValue(Value* V, IntegrationAttempt* IA, bool brief=false) {

  std::string Esc;
  raw_string_ostream RSO(Esc);
  IA->printWithCache(V, RSO, brief);
  return escapeHTML(TruncStr(RSO.str(), 500));

}

void IntegrationAttempt::printRHS(ShadowValue SV, raw_ostream& Out) {
  
  if(SV.isVal())
    return;

  InstArgImprovement* IAI = SV.getIAI();

  ShadowInstruction* SI = SV.getInst();

  if(Constant* C = getConstReplacement(SV)) {
    if(isa<Function>(C))
      Out << "@" << C->getName();
    else
      Out << (*C);
    return;
  }
  /*
  if(IAI->dieStatus != INSTSTATUS_ALIVE) {
    if(isInvariant)
      Out << "(invar) ";
    Out << "DEAD";
    return;
  }
  */
  bool PBPrinted = false;
  if(IAI->PB) {
    ImprovedValSetSingle* IVS = dyn_cast<ImprovedValSetSingle>(IAI->PB);
    if((!IVS) || (IVS->Values.size() > 0 && !IVS->Overdef)) {
      IAI->PB->print(Out, true);
      PBPrinted = true;
    }
  }

  if(!SI)
    return;

  DenseMap<Instruction*, std::string>::iterator optit = optimisticForwardStatus.find(SI->invar->I);
  if(!PBPrinted) {
    if(optit != optimisticForwardStatus.end()) {
      Out << "OPT (" << optit->second << "), ";
    }
  }
  if(LoadInst* LI = dyn_cast_inst<LoadInst>(SI)) {

    DenseMap<LoadInst*, std::string>::iterator it = normalLFFailures.find(LI);

    if(it != normalLFFailures.end()) {
      Out << "NORM (" <<  it->second << ")";
    }
  }
  else if(CallInst* CI = dyn_cast_inst<CallInst>(SI)) {
    DenseMap<CallInst*, OpenStatus*>::iterator it = forwardableOpenCalls.find(CI);
    if(it != forwardableOpenCalls.end()) {
      Out << it->second->Name << "(" << (it->second->success ? "success" : "not found") << ")";
    }
    else {
      DenseMap<CallInst*, ReadFile>::iterator it = resolvedReadCalls.find(CI);
      if(it != resolvedReadCalls.end())
	Out << it->second.openArg->Name << " (" << it->second.incomingOffset << "-" << it->second.incomingOffset + (it->second.readSize - 1) << ")";
    }
  }

}

bool InlineAttempt::getSpecialEdgeDescription(ShadowBBInvar* FromBB, ShadowBBInvar* ToBB, raw_ostream& Out) {

  return false;

}

bool PeelIteration::getSpecialEdgeDescription(ShadowBBInvar* FromBB, ShadowBBInvar* ToBB, raw_ostream& Out) {

  if(FromBB->BB == L->getLoopLatch() && ToBB->BB == L->getHeader()) {

    Out << "\"Next iteration header\"";
    return true;

  }
  else if(!L->contains(ToBB->naturalScope)) {

    Out << "\"Exit block " << escapeHTML(ToBB->BB->getName()) << "\"";
    return true;

  }

  return false;

}

void IntegrationAttempt::printOutgoingEdge(ShadowBBInvar* BBI, ShadowBB* BB, ShadowBBInvar* SBI, ShadowBB* SB, uint32_t i, bool useLabels, const Loop* deferEdgesOutside, SmallVector<std::string, 4>* deferredEdges, raw_ostream& Out, bool brief) {

  if(brief && ((!SB) || shouldIgnoreEdge(BBI, SBI)))
    return;

  std::string edgeString;
  raw_string_ostream rso(edgeString);

  rso << "Node" << BBI->BB;
  if(useLabels) {
    rso << ":s" << i;
  }

  rso << " -> ";

  // Handle exits from this loop / this loop's latch specially:
  if(!getSpecialEdgeDescription(BBI, SBI, rso))
    rso << "Node" << SBI->BB;

  if(edgeIsDead(BBI, SBI)) {
    rso << "[color=gray]";
  }
  else if(shouldIgnoreEdge(BBI, SBI)) {
    rso << "[color=red]";
  }

  rso << ";\n";

  if(deferEdgesOutside && !deferEdgesOutside->contains(SBI->naturalScope)) {
    deferredEdges->push_back(rso.str());
  }
  else {
    Out << rso.str();
  }
	
}

static void printPathConditions(std::vector<PathCondition>& conds, PathConditionTypes t, raw_ostream& Out, ShadowBBInvar* BBI, ShadowBB* BB) {

  for(std::vector<PathCondition>::iterator it = conds.begin(), itend = conds.end(); it != itend; ++it) {

    if(it->fromBB == BBI->BB) {

      Out << "<tr><td colspan=\"2\" border=\"0\" align=\"left\">  ";
      switch(t) {
      case PathConditionTypeInt:
	Out << "Int";
	break;
      case PathConditionTypeString:
	Out << "String";
	break;
      case PathConditionTypeIntmem:
	Out << "Intmem";
	break;
      }

      Out << " PC: ";

      if(t == PathConditionTypeString) {

	GlobalVariable* GV = cast<GlobalVariable>(it->val);
	ConstantDataArray* CDA = cast<ConstantDataArray>(GV->getInitializer());
	Out << "\"" << CDA->getAsCString() << "\"";

      }
      else {

	Out << *it->val;
	
      }

      if(!it->instBB) {

	ShadowGV* GV = &GlobalIHP->shadowGlobals[it->instIdx];
	Out << " -&gt; " << itcache(GV);

      }
      else if(it->instBB == (BasicBlock*)ULONG_MAX) {

	IntegrationAttempt* ArgIA = getIAWithTargetStackDepth(BB->IA->getFunctionRoot(), it->instStackIdx);

	Function::arg_iterator AI = ArgIA->F.arg_begin();
	std::advance(AI, it->instIdx);
	Out << " -&gt; " << itcache((Argument*)AI, true);

      }
      else {
	
	BasicBlock::iterator BI = it->instBB->begin();
	std::advance(BI, it->instIdx);

	Out << " -&gt; " << it->instBB->getName() << " / " << itcache((Instruction*)BI, true);

      }

      if(it->offset != 0)
	Out << " + " << it->offset;

      Out << "</td></tr>\n";

    }

  }

}

void IntegrationAttempt::describeBlockAsDOT(ShadowBBInvar* BBI, ShadowBB* BB, const Loop* deferEdgesOutside, SmallVector<std::string, 4>* deferredEdges, raw_ostream& Out, SmallVector<ShadowBBInvar*, 4>* forceSuccessors, bool brief) {

  if(brief && !BB)
    return;

  TerminatorInst* TI = BBI->BB->getTerminator();
  bool useLabels = false;
  if(!forceSuccessors) {
    if(BranchInst* BI = dyn_cast<BranchInst>(TI))
      useLabels = BI->isConditional();
    else if(isa<SwitchInst>(TI))
      useLabels = true;
  }
  unsigned numSuccessors = 1;
  if(useLabels)
    numSuccessors = TI->getNumSuccessors();

  Out << "Node" << BBI->BB << " [shape=plaintext,fontsize=10,label=<<table cellspacing=\"0\" border=\"0\"><tr><td colspan=\"" << numSuccessors << "\" border=\"1\"><table border=\"0\">\n";

  Out << "<tr><td border=\"0\" align=\"left\" colspan=\"2\"";
  
  if(BB && BB->useSpecialVarargMerge) {
    Out << " bgcolor=\"lightblue\"";
  }
  else if(BB && BB->status == BBSTATUS_CERTAIN) {
    if(!BB->inAnyLoop)
      Out << " bgcolor=\"green\"";
    else
      Out << " bgcolor=\"yellow\"";
  }
  else if(BB && BB->status == BBSTATUS_ASSUMED) {
    Out << " bgcolor=\"orange\"";
  }

  Out << "><font point-size=\"14\">";
  if(BBI->BB == getEntryBlock())
    Out << "Entry block: ";
  Out << escapeHTML(BBI->BB->getName()) << "</font></td></tr>\n";

  bool isFunctionHeader = (!L) && (BBI->BB == &(F.getEntryBlock()));

  if(BB && (!L) && BB->IA->getFunctionRoot()->targetCallInfo) {

    // Mention if there are symbolic path conditions or functions here:
    printPathConditions(pass->rootIntPathConditions, PathConditionTypeInt, Out, BBI, BB);
    printPathConditions(pass->rootIntmemPathConditions, PathConditionTypeIntmem, Out, BBI, BB);
    printPathConditions(pass->rootStringPathConditions, PathConditionTypeString, Out, BBI, BB);

    for(std::vector<PathFunc>::iterator it = pass->rootFuncPathConditions.begin(),
	  itend = pass->rootFuncPathConditions.end(); it != itend; ++it) {

      if(it->BB == BBI->BB) {

	Out << "<tr><td colspan=\"2\" border=\"0\" align=\"left\">  Call PC: ";
	Out << it->F->getName();
	Out << "</td></tr>\n";

      }

    }

  }

  size_t ValSize = BBI->BB->size();
  if(isFunctionHeader)
    ValSize += F.arg_size();

  std::vector<ShadowValue> Vals;
  Vals.reserve(ValSize);

  if(isFunctionHeader) {
    InlineAttempt* self = getFunctionRoot();
    for(uint32_t i = 0; i < F.arg_size(); ++i)
      Vals.push_back(ShadowValue(&(self->argShadows[i])));
  }

  BasicBlock::iterator BI, BE;
  uint32_t i;
  for(BI = BBI->BB->begin(), BE = BBI->BB->end(), i = 0; BI != BE; ++BI, ++i) {
    if(!BB)
      Vals.push_back(ShadowValue(BI));
    else
      Vals.push_back(ShadowValue(&(BB->insts[i])));
  }

  for(std::vector<ShadowValue>::iterator VI = Vals.begin(), VE = Vals.end(); VI != VE; ++VI) {

    std::string textColour;
    Out << "<tr><td border=\"0\" align=\"left\" bgcolor=\"" << getValueColour(*VI, textColour) << "\">";
    if(!textColour.empty())
      Out << "<font color=\"" << textColour << "\">";
    Out << escapeHTMLValue(VI->getBareVal(), this);
    if(!textColour.empty())
      Out << "</font>";
    Out << "</td><td>";
    std::string RHSStr;
    raw_string_ostream RSO(RHSStr);
    printRHS(*VI, RSO);
    RSO.flush();
    Out << escapeHTML(TruncStr(RSO.str(), 400));
    Out << "</td></tr>\n";

  }

  Out << "</table></td></tr>";

  // Print ports for branch / switch statements, borrowed from the DOT printer.

  if(useLabels) {

    Out << "<tr>\n";
    unsigned i = 0;
    for(succ_const_iterator SI = succ_begin(const_cast<const BasicBlock*>(BBI->BB)), SE = succ_end(const_cast<const BasicBlock*>(BBI->BB)); SI != SE; ++SI, ++i) {
      Out << "<td port=\"s" << i << "\" border=\"1\">" << DOTGraphTraits<const Function*>::getEdgeSourceLabel(BBI->BB, SI) << "</td>\n";
    }
    Out << "</tr>\n";

  }

  Out << "</table>>];\n";

  if(forceSuccessors) {

    for(SmallVector<ShadowBBInvar*, 4>::iterator it = forceSuccessors->begin(), it2 = forceSuccessors->end(); it != it2; ++it) {

      ShadowBBInvar* SuccBBI = getBBInvar((*it)->idx);
      IntegrationAttempt* IA = getIAForScope(SuccBBI->naturalScope);
      ShadowBB* SuccBB = IA->getBB(*SuccBBI);
      printOutgoingEdge(BBI, BB, SuccBBI, SuccBB, 0, false, deferEdgesOutside, deferredEdges, Out, brief);

    }

  }
  else {

    // Print the successor edges *except* any loop exit edges, since those must occur in parent context.
    for(uint32_t i = 0; i < BBI->succIdxs.size(); ++i) {

      ShadowBBInvar* SuccBBI = getBBInvar(BBI->succIdxs[i]);
      IntegrationAttempt* IA = getIAForScope(SuccBBI->naturalScope);
      ShadowBB* SuccBB = IA->getBB(*SuccBBI);

      printOutgoingEdge(BBI, BB, SuccBBI, SuccBB, i, useLabels, deferEdgesOutside, deferredEdges, Out, brief);

    }

  }
 
}

bool IntegrationAttempt::blockLiveInAnyScope(ShadowBBInvar* BB) {

  if(!getBB(*BB))
    return false;

  if(BB->naturalScope != L) {

    const Loop* enterL = immediateChildLoop(L, BB->naturalScope);
    if(PeelAttempt* LPA = getPeelAttempt(enterL)) {

      if(LPA->Iterations.back()->iterStatus == IterationStatusFinal) {

	for(unsigned i = 0; i < LPA->Iterations.size(); ++i) {
	  
	  if(LPA->Iterations[i]->blockLiveInAnyScope(BB))
	    return true;
	  
	}

	return false;

      }

    }

  }

  // Live here and not in a child loop or in an unexpanded or unterminated loop.
  return true;

}

void IntegrationAttempt::describeLoopAsDOT(const Loop* DescribeL, uint32_t headerIdx, raw_ostream& Out, bool brief) {

  SmallVector<std::string, 4> deferredEdges;

  if(brief && !BBs[headerIdx])
    return;

  ShadowLoopInvar& LInfo = *(invarInfo->LInfo[DescribeL]);

  Out << "subgraph \"cluster_" << DOT::EscapeString(DescribeL->getHeader()->getName()) << "\" {";

  bool loopIsIgnored = pass->shouldIgnoreLoop(DescribeL->getHeader()->getParent(), DescribeL->getHeader());

  if(loopIsIgnored) {

    // Print the loop blocks including sub-clustering:
    describeScopeAsDOT(DescribeL, headerIdx, Out, brief, &deferredEdges);

  }
  else if(brief) {

    // Draw the header branching to all exiting blocks, to each exit block.
    std::vector<uint32_t>& exitingIdxs = LInfo.exitingBlocks;

    SmallVector<ShadowBBInvar*, 4> liveExitingBlocks;

    for(unsigned i = 0; i < exitingIdxs.size(); ++i) {

      ShadowBBInvar* BBI = getBBInvar(exitingIdxs[i]);
      if(blockLiveInAnyScope(BBI)) {

	liveExitingBlocks.push_back(BBI);

      }

    }

    describeBlockAsDOT(getBBInvar(headerIdx + BBsOffset), getBB(headerIdx + BBsOffset), 0, 0, Out, &liveExitingBlocks, brief);

    std::vector<std::pair<uint32_t, uint32_t> >& exitEdges = LInfo.exitEdges;

    for(SmallVector<ShadowBBInvar*, 4>::iterator it = liveExitingBlocks.begin(), it2 = liveExitingBlocks.end(); it != it2; ++it) {
      
      ShadowBBInvar* BBI = *it;
      SmallVector<ShadowBBInvar*, 4> Targets;

      for(std::vector<std::pair<uint32_t, uint32_t> >::iterator it3 = exitEdges.begin(), it4 = exitEdges.end(); it3 != it4; ++it3) {

	if(it3->first == BBI->idx) {

	  Targets.push_back(getBBInvar(it3->second));

	}

      }

      describeBlockAsDOT(BBI, getBB(*BBI), DescribeL, &deferredEdges, Out, &Targets, brief);      

    }

  }
  else {

    ShadowBBInvar* BBInvar;
    uint32_t idx;

    for(idx = headerIdx, BBInvar = getBBInvar(headerIdx + BBsOffset); DescribeL->contains(BBInvar->naturalScope); ++idx, BBInvar = getBBInvar(idx + BBsOffset)) {

      ShadowBB* BB = getBB(*BBInvar);
      describeBlockAsDOT(BBInvar, BB, DescribeL, &deferredEdges, Out, 0, brief);

    }

  }
						     
  Out << "label = \"Loop " << DOT::EscapeString(DescribeL->getHeader()->getName()) << " (";

  DenseMap<const Loop*, PeelAttempt*>::iterator InlIt = peelChildren.find(DescribeL);
  if(loopIsIgnored) {

    Out << "Ignored";

  }
  else if(InlIt == peelChildren.end()) {

    Out << "Not explored";

  }
  else {

    PeelIteration* LastIter = InlIt->second->Iterations.back();
    if(LastIter->iterStatus == IterationStatusFinal) {
      Out << "Terminated";
    }
    else {
      Out << "Not terminated";
    }

    Out << ", " << InlIt->second->Iterations.size() << " iterations";

  }

  Out << ")\";\n}\n";

  for(SmallVector<std::string, 4>::iterator it = deferredEdges.begin(), it2 = deferredEdges.end(); it != it2; ++it) {

    Out << *it;

  }

}

void IntegrationAttempt::describeScopeAsDOT(const Loop* DescribeL, uint32_t headerIdx, raw_ostream& Out, bool brief, SmallVector<std::string, 4>* deferredEdges) {

  ShadowBBInvar* BBI;
  uint32_t i;

  for(i = headerIdx, BBI = getBBInvar(headerIdx + BBsOffset); 
      i < nBBs && ((!DescribeL) || DescribeL->contains(BBI->naturalScope)); 
      ++i, BBI = getBBInvar(i + BBsOffset)) {

    ShadowBBInvar* BBI = getBBInvar(i + BBsOffset);
    ShadowBB* BB = BBs[i];
    
    if(BBI->naturalScope != DescribeL) {

      describeLoopAsDOT(BBI->naturalScope, i, Out, brief);
	
      // Advance past the loop:
      while(i < nBBs && BBI->naturalScope->contains(getBBInvar(i + BBsOffset)->naturalScope))
	++i;
      --i;
      continue;

    }

    describeBlockAsDOT(BBI, BB, deferredEdges ? DescribeL : 0, deferredEdges, Out, 0, brief);

  }

}

void IntegrationAttempt::describeAsDOT(raw_ostream& Out, bool brief) {

  std::string escapedName;
  raw_string_ostream RSO(escapedName);
  printHeader(RSO);
  Out << "digraph \"Toplevel\" {\n\tlabel = \"" << DOT::EscapeString(RSO.str()) << "\"\n";

  describeScopeAsDOT(L, 0, Out, brief, 0);

  // Finally terminate the block.
  Out << "}\n";

}

std::string IntegrationAttempt::getGraphPath(std::string prefix) {

  std::string Ret;
  raw_string_ostream RSO(Ret);
  RSO << prefix << "/out.dot";
  return RSO.str();

}

void PeelAttempt::describeTreeAsDOT(std::string path) {

  unsigned i = 0;
  for(std::vector<PeelIteration*>::iterator it = Iterations.begin(), it2 = Iterations.end(); it != it2; ++it, ++i) {

    std::string newPath;
    raw_string_ostream RSO(newPath);
    RSO << path << "/iter_" << i;
    mkdir(RSO.str().c_str(), 0777);
    (*it)->describeTreeAsDOT(RSO.str());

  }

}

void IntegrationAttempt::describeTreeAsDOT(std::string path) {

  std::string graphPath = getGraphPath(path);

  std::string error;
  raw_fd_ostream os(graphPath.c_str(), error);

  if(!error.empty()) {

    errs() << "Failed to open " << graphPath << ": " << error << "\n";
    return;

  }

  describeAsDOT(os, false);

  for(DenseMap<const Loop*, PeelAttempt*>::iterator it = peelChildren.begin(), it2 = peelChildren.end(); it != it2; ++it) {

    std::string newPath;
    raw_string_ostream RSO(newPath);
    RSO << path << "/loop_" << it->first->getHeader()->getName();
    mkdir(RSO.str().c_str(), 0777);
    it->second->describeTreeAsDOT(RSO.str());

  }

  for(DenseMap<ShadowInstruction*, InlineAttempt*>::iterator it = inlineChildren.begin(), it2 = inlineChildren.end(); it != it2; ++it) {

    std::string newPath;
    raw_string_ostream RSO(newPath);
    RSO << path << "/call_";

    if(it->first->getType()->isVoidTy()) {
      // Name the call after a BB plus offset
      Instruction* I = it->first->invar->I;
      BasicBlock::iterator BI(I);
      int j;
      for(j = 0; BI != I->getParent()->begin(); --BI, ++j) { }
      RSO << I->getParent()->getName() << "+" << j;
    }
    else {
      // Use the call's given name (pull it out of the full call printout)
      std::string callDesc;
      raw_string_ostream callRSO(callDesc);
      callRSO << it->first;
      callRSO.flush();
      RSO << callDesc.substr(2, callDesc.find_first_of('=') - 3);
    }

    mkdir(RSO.str().c_str(), 0777);
    it->second->describeTreeAsDOT(RSO.str());

  }

}
