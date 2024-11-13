// <legal>
// DMC Tool
// Copyright 2023 Carnegie Mellon University.
// 
// NO WARRANTY. THIS CARNEGIE MELLON UNIVERSITY AND SOFTWARE ENGINEERING INSTITUTE
// MATERIAL IS FURNISHED ON AN "AS-IS" BASIS. CARNEGIE MELLON UNIVERSITY MAKES NO
// WARRANTIES OF ANY KIND, EITHER EXPRESSED OR IMPLIED, AS TO ANY MATTER
// INCLUDING, BUT NOT LIMITED TO, WARRANTY OF FITNESS FOR PURPOSE OR
// MERCHANTABILITY, EXCLUSIVITY, OR RESULTS OBTAINED FROM USE OF THE MATERIAL.
// CARNEGIE MELLON UNIVERSITY DOES NOT MAKE ANY WARRANTY OF ANY KIND WITH RESPECT
// TO FREEDOM FROM PATENT, TRADEMARK, OR COPYRIGHT INFRINGEMENT.
// 
// Released under a MIT (SEI)-style license, please see License.txt or contact
// permission@sei.cmu.edu for full terms.
// 
// [DISTRIBUTION STATEMENT A] This material has been approved for public release
// and unlimited distribution.  Please see Copyright notice for non-US Government
// use and distribution.
// 
// Carnegie Mellon (R) and CERT (R) are registered in the U.S. Patent and Trademark
// Office by Carnegie Mellon University.
// 
// This Software includes and/or makes use of the following Third-Party Software
// subject to its own license:
// 1. Phasar
//     (https://github.com/secure-software-engineering/phasar/blob/development/LICENSE.txt)
//     Copyright 2017 - 2023 Philipp Schubert and others.  
// 2. LLVM (https://github.com/llvm/llvm-project/blob/main/LICENSE.TXT) 
//     Copyright 2003 - 2022 LLVM Team.
// 
// DM23-0532
// </legal>

#include <map>
#include <set>
#include <unordered_set>
#include <queue>
#include <utility>
#include <optional>

#include <llvm/Pass.h> 
#include <llvm/IR/Module.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "llvm/IR/InstrTypes.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/Support/Debug.h>
#include "llvm/IR/Operator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DebugInfoMetadata.h"

#include <llvm/IR/Dominators.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/DominanceFrontier.h>
 
using namespace llvm;
using namespace std;


class CondMergePass : public llvm::FunctionPass 
{
public:  
  static char ID;

  using Edge = pair<Instruction*, int /*succ index*/>;

  DominatorTree* preDomTree;
  PostDominatorTree* postDomTree;
  map<Instruction*, size_t> id_of_jump;
  map<Edge, MDNode*> md_of_edge;
  size_t next_jump_id = 1;
  
  CondMergePass() : llvm::FunctionPass(ID) { }
  ~CondMergePass(){ }

  virtual void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<PostDominatorTreeWrapperPass>();
    AU.setPreservesCFG();
  }

  size_t get_jump_id(Instruction* inst) {
    if (!id_of_jump.count(inst)) {
      size_t jump_id = next_jump_id++;
      id_of_jump[inst] = jump_id;
      LLVMContext &ctx = inst->getContext();

      auto md = dyn_cast<ConstantAsMetadata>(llvm::ValueAsMetadata::getConstant(
        ConstantInt::get(Type::getInt64Ty(ctx), jump_id)));
      MDNode *md_node =  MDTuple::get(ctx, md);
      inst->setMetadata("JumpID", md_node);
    }
    return id_of_jump[inst];
  }

  MDNode* get_edge_md(Edge edge) {
    if (!md_of_edge.count(edge)) {
      size_t jump_id = get_jump_id(edge.first);
      LLVMContext &ctx = edge.first->getContext();
      SmallVector<Metadata *> Ops;
      Ops.push_back(llvm::ValueAsMetadata::getConstant(
        ConstantInt::get(Type::getInt64Ty(ctx), jump_id)));
      Ops.push_back(llvm::ValueAsMetadata::getConstant(
        ConstantInt::get(Type::getInt64Ty(ctx), edge.second)));
      MDNode *md_node =  MDTuple::get(ctx, Ops);
      md_of_edge[edge] = md_node;
    }
    return md_of_edge[edge];
  }

  BasicBlock* dest_bb_of_edge(Edge e) {
    return e.first->getSuccessor(e.second);
  }

  void findMergeEdges(Instruction* jump, map<BasicBlock*, vector<Edge>>& bb_to_cond_paths) {
    SmallVector<Metadata *> final_md_vec;
    int numJumpSucc = jump->getNumSuccessors();
    get_jump_id(jump);
    LLVMContext &ctx = jump->getContext();
    for (int iSucc=0; iSucc < numJumpSucc; iSucc++) {
      //BasicBlock* succ = jump->getSuccessor(iSucc);
      set<Edge> alreadySeen;
      set<BasicBlock*> seenBBs;
      queue<Edge> queue;
      vector<Edge> mergeEdges;
      Edge condEdge = {jump, iSucc};
      queue.push({jump, iSucc});
      while (queue.size() > 0) {
        Edge curEdge = queue.front();
        queue.pop();
        if (alreadySeen.count(curEdge)) {
          continue;
        }
        alreadySeen.insert(curEdge);
        BasicBlock* bb = dest_bb_of_edge(curEdge);
        Instruction* bbTerm = bb->getTerminator();
        bool is_merge_edge = (
          (bbTerm == jump) ||
          preDomTree->dominates(bbTerm, jump->getParent()) ||
          postDomTree->dominates(bbTerm, jump)
        );
        if (is_merge_edge) {
          mergeEdges.push_back(curEdge);
          //outs() << "Found a merge edge!\n";

        } else {
          if (!seenBBs.count(bb)) {
            bb_to_cond_paths[bb].push_back(condEdge);
            seenBBs.insert(bb);
          }
          int numSucc = bbTerm->getNumSuccessors();
          for (int i=0; i < numSucc; i++) {
            //BasicBlock* succ = bbTerm->getSuccessor(i);
            queue.push({bbTerm, i});
          }
        }
      }
      SmallVector<Metadata *> Ops;
      for (Edge& edge : mergeEdges) {
        Ops.push_back(get_edge_md(edge));
      }
      MDNode *md_node =  MDTuple::get(ctx, Ops);
      final_md_vec.push_back(md_node);
      //outs() << "Writing MergeEdges metadata for "; write_line_col(jump); outs() << "\n";
    }
    MDNode *md_node =  MDTuple::get(ctx, final_md_vec);
    jump->setMetadata("MergeEdges", md_node);
  }

  bool runOnFunction(llvm::Function &F) override 
  {
    //outs() << "################## \n";
    //outs() << "# Function: " << F.getName() << "\n";
    this->preDomTree  = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    this->postDomTree = &getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();
    map<BasicBlock*, vector<Edge>> bb_to_cond_paths;
    for (llvm::Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
      Instruction* bbTerm = BB->getTerminator();
      if (bbTerm->getNumSuccessors() > 1) {
        findMergeEdges(bbTerm, bb_to_cond_paths);
      }
    }

    LLVMContext &ctx = F.getContext();
    for (auto const& [bb, cond_paths] : bb_to_cond_paths) {
      Instruction* bbTerm = bb->getTerminator();
      SmallVector<Metadata *> Ops;
      for (Edge condEdge : cond_paths) {
        Ops.push_back(get_edge_md(condEdge));
      }
      bbTerm->setMetadata("CondPaths", MDTuple::get(ctx, Ops));
    }
    return true; 
  }
  
};

char CondMergePass::ID = 0;

static llvm::RegisterPass<CondMergePass> X(
              "condmerge",
              "CondMerge: Identify merge edges of conditional paths",
              false, true
);



////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////



class PrintMergeEdgesPass : public llvm::FunctionPass 
{
public:  
  static char ID;

  using Edge = pair<Instruction*, int /*succ index*/>;

  map<size_t, Instruction*> jump_id_to_inst;
  //map<Instruction*, DebugLoc*> jump_to_dl;
  
  PrintMergeEdgesPass() : llvm::FunctionPass(ID) { }
  ~PrintMergeEdgesPass(){ }

  virtual void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }

  DebugLoc get_jump_dl(Instruction* jump) {
    BasicBlock* bb = jump->getParent();
    for (auto BI = bb->rbegin(), BE = bb->rend(); BI != BE; ++BI) {
      Instruction* inst = &*BI;
      llvm::DebugLoc dl = inst->getDebugLoc();
      if (dl && dl.getLine() > 0) {
        return dl;
      }
    }
    return nullptr;
  }

  void write_line_col(BasicBlock* bb) {
    for (llvm::BasicBlock::iterator BI = bb->begin(), BE = bb->end(); BI != BE; ++BI) {
      llvm::Instruction* inst = &(*BI);
      llvm::DebugLoc dl = inst->getDebugLoc();
      if (dl && dl.getLine() > 0) {
        write_line_col(inst);
        return;
      }
    }
  }

  void write_line_col(DebugLoc dl) {
    if (dl) {
      outs() << "[Line" << dl.getLine() << ":c" << dl.getCol() << "]";
    } else {
      outs() << "[MissingLoc]";
    }
  }

  void write_line_col(Instruction* inst) {
    llvm::DebugLoc dl = inst->getDebugLoc();
    if (dl) {
      outs() << "[Line" << dl.getLine() << ":c" << dl.getCol() << "]";
    } else {
      outs() << "[MissingLoc]";
    }
  }

  void write_file_line_col(Instruction* inst) {
    llvm::DebugLoc dl = inst->getDebugLoc();
    if (dl) {
      // TODO: Escape any quotation marks in the filename.
      outs() << "[\"" << dl.get()->getFilename() << "\", ";
      outs() << "[" << dl.getLine() << ", " << dl.getCol() << "]";
      outs() << "]";
    } else {
      outs() << "[\"???\", -1, -1]";
    }
  }

  BasicBlock* dest_bb_of_edge(Edge e) {
    return e.first->getSuccessor(e.second);
  }

  static uint64_t get_md_i64_operand(MDNode* md_node, int operand_idx) {
    return mdconst::extract<ConstantInt>(md_node->getOperand(operand_idx))->getZExtValue();
  }
    

  bool runOnFunction(llvm::Function &F) override 
  {
    outs() << "################## \n";
    outs() << "# Function: " << F.getName() << "\n";
    for (llvm::Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
      Instruction* bbTerm = BB->getTerminator();
      MDNode* md_node = bbTerm->getMetadata("JumpID");
      if (md_node) {
        uint64_t jump_id = mdconst::extract<ConstantInt>(md_node->getOperand(0))->getZExtValue();
        jump_id_to_inst[jump_id] = bbTerm;
      }
    }
    for (llvm::Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
      Instruction* bbTerm = BB->getTerminator();
      int numSucc = bbTerm->getNumSuccessors();
      if (numSucc > 1) {
        MDNode* top_node = bbTerm->getMetadata("MergeEdges");
        if (top_node) {
          for (int iSucc = 0; iSucc < numSucc; iSucc++) {
            outs() << "Merge edges for "; write_line_col(get_jump_dl(bbTerm));
            outs() << " -> "; write_line_col(bbTerm->getSuccessor(iSucc));
            outs() << ":\n";
            MDNode* mid_node = dyn_cast<MDNode>(top_node->getOperand(iSucc));
            for (const MDOperand& operand : mid_node->operands()) {
              MDNode* edge_md = dyn_cast<MDNode>(operand);
              uint64_t merge_jump_id = get_md_i64_operand(edge_md, 0);
              uint64_t merge_succ_id = get_md_i64_operand(edge_md, 1);
              Instruction* merge_jump_inst = jump_id_to_inst[merge_jump_id];
              if (!merge_jump_inst) {
                outs() << "Jump is NULL!\n";
              } else {
            outs() << "  ";
                write_line_col(get_jump_dl(merge_jump_inst));
                outs() << " -> ";
                write_line_col(merge_jump_inst->getSuccessor(merge_succ_id));
                outs() << "\n";
              }
            }
          }
        } else {
          outs() << "No merge-edge info for jump at "; write_line_col(bbTerm); outs() << ".\n";
        }
      }
    }
    outs() << "-----------------\n";
    vector<Instruction*> terminators;
    for (llvm::Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
      Instruction* bbTerm = BB->getTerminator();
      terminators.push_back(bbTerm);
    }
    std::stable_sort(
        terminators.begin(), 
        terminators.end(), 
        [&](Instruction* i1, Instruction* i2) {
            DebugLoc d1 = get_jump_dl(i1);
            DebugLoc d2 = get_jump_dl(i2);
            if (!d1 || !d2) {
                return false;
            }
            return d1.getLine() < d2.getLine();
        }
    );
    for (Instruction* bbTerm : terminators) {
      MDNode* md_node = bbTerm->getMetadata("CondPaths");
      if (md_node) {
        outs() << "Basic block ending at "; write_line_col(get_jump_dl(bbTerm));
        outs() << " is on a cond path for the following cond edges:\n";
        for (const MDOperand& edge_op: md_node->operands()) {
          MDNode* edge_md = dyn_cast<MDNode>(edge_op);
          uint64_t merge_jump_id = get_md_i64_operand(edge_md, 0);
          uint64_t merge_succ_id = get_md_i64_operand(edge_md, 1);
          Instruction* merge_jump_inst = jump_id_to_inst[merge_jump_id];
          if (!merge_jump_inst) {
            outs() << "Jump is NULL!\n";
          } else {
            outs() << "  ";
            write_line_col(get_jump_dl(merge_jump_inst));
            outs() << " -> ";
            write_line_col(merge_jump_inst->getSuccessor(merge_succ_id));
            outs() << "\n";
          }
        }
      }
    }
    return false; 
  }
  
};

char PrintMergeEdgesPass::ID = 0;

static llvm::RegisterPass<PrintMergeEdgesPass> Y(
              "print-merge-edges",
              "PrintMergeEdges",
              false, true
);

