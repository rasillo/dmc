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

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

#include <llvm/Pass.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LegacyPassManager.h>


#if LLVM_VERSION_MAJOR < 17
    #define USE_OLD_PASS_MANAGER 1
#else
    #define USE_OLD_PASS_MANAGER 0
#endif

#if USE_OLD_PASS_MANAGER
    #include <llvm/Transforms/IPO/PassManagerBuilder.h>
#else
    #include <llvm/Passes/PassBuilder.h>
    #include <llvm/Passes/PassPlugin.h>
    #include <llvm/IR/PassManager.h>
#endif

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

#include <llvm/Demangle/Demangle.h>

using namespace llvm;
using namespace std;

//////////////////////////////////////////////////////////////////////////////

void write_line_col(Instruction* inst) {
  llvm::DebugLoc dl = inst->getDebugLoc();
  if (dl) {
    outs() << "[Line" << dl.getLine() << ":c" << dl.getCol() << "]";
  } else {
    outs() << "[MissingLoc]";
  }
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

void write_file_line_col(Instruction* inst) {
  llvm::DebugLoc dl = inst->getDebugLoc();
  if (dl) {
    // TODO: Escape any quotation marks in the filename.
    outs() << "[\"" << dl.get()->getFilename() << "\",";
    outs() << "\"" << inst->getFunction()->getName() << "\",";
    outs() << "" << dl.getLine() << "," << dl.getCol() << "]";
    outs() << "";
  } else {
    outs() << "[\"???\", -1, -1]";
  }
}

bool looks_like_filename(string str) {
  ssize_t size = str.size();
  if (!(3 < size && size < 100)) {
    return false;
  }
  for (ssize_t i = 0; i < size; i++){
    if (!('-' <= str[i]) && (str[i] <= '~')) {
      return false;
    }
    if (str[i] == ' ') {
      return false;
    }
  }
  return true;
}

//////////////////////////////////////////////////////////////////////////////

#define RETVAL_CODE -1
#define AUX_TYPE_NULL 0
#define AUX_TYPE_MAIN 1
#define AUX_TYPE_FILE 2

map<llvm::GlobalVariable*, set<llvm::Function*>> fnsReferencingGvar;

const char* getAuxName(int aux) {
  switch (aux) {
    case AUX_TYPE_NULL: return "null";
    case AUX_TYPE_MAIN: return "main";
    case AUX_TYPE_FILE: return "file";
    default: return "(error)";
  }
}

// Note: There are three types of real sources/sinks:
// 1. Arguments of system API functions (callsite != null, ixArg != RETVAL_CODE)
// 2. Return values of system API functions (callsite != null, ixArg == RETVAL_CODE)
// 3. Constants such as filenames or stdin
//
// There are also two types of intermediate sources/sinks:
// 1. Parameters of user-defined functions (callsite == null, ixArg != RETVAL_CODE)
// 2. Return values of user-defined functions (callsite == null, ixArg == RETVAL_CODE)

enum SrcSinkType : size_t {
  SS_TYPE_SYS_ARG = 1,
  SS_TYPE_SYS_RET = 2,
  SS_TYPE_SUM_ARG = 3,
  SS_TYPE_SUM_RET = 4,
  SS_TYPE_AUX_CONST,
};

struct SrcOrSink_t {
  llvm::Function* func;
  int ixArg;  // 0-indexed, and RETVAL_CODE (-1) denotes the return value.
  llvm::CallBase* callsite; // see below note about what callsite==nullptr means.
  int auxType;
  struct SrcOrSink_t* wrapped;
  llvm::Value* auxConst;
  // A NULL value for callsite means that this is representing a function
  // argument or return value as source/sink in a function summary, not a true
  // source/sink.

  // "scrink" is short for "source or sink"
  bool isSummaryScrink() const {
    return (callsite == nullptr) && (auxConst == nullptr);
  }
  bool operator==(const SrcOrSink_t& other) const = default;
  auto operator<=>(const SrcOrSink_t& other) const = default;

};

using Sink_t = SrcOrSink_t;
using SensSrc_t = SrcOrSink_t;


//struct SensSrcHash {
//  size_t operator()(const SensSrc_t& s) const {
//    return std::hash<llvm::Function*>()(s.func) ^ std::hash<int>()(s.ixArg) ^ std::hash<llvm::Instruction*>()(s.callsite);
//  }
//};
//
//struct SensSrcEqual {
//  bool operator()(const SensSrc_t& lhs, const SensSrc_t& rhs) const {
//    return lhs.func == rhs.func && lhs.ixArg == rhs.ixArg && lhs.callsite == rhs.callsite && lhs.wrapped == rhs.wrapped;
//  }
//};





/*****************************************************************************
 * Simple alias analysis.  Just handles phi nodes.  Every llvm::Value except
 * phi nodes is considered a 'base' location.  Phi nodes can point to multiple
 * base locations.
 ****************************************************************************/


const set<SensSrc_t>& asSingleSet(const set<SensSrc_t>& x) {
  return x;
}

// const set<SensSrc_t> asSingleSetCopy(const set<SensSrc_t>& x) {
//   return x;
// }

using SensSrcSet_t = set<SensSrc_t>;
//using SensSrcSet_t = SrcAliasedSet;


// #define UNCHANGED 'U'
// #define CHANGED 'C'

template<typename T>
void extendWith(std::set<T>& destination, const std::set<T>& source) {
  destination.insert(source.begin(), source.end());
}

void copySet(SensSrcSet_t& base, SensSrcSet_t& addl) {
  // int n = base.size();
  extendWith(base, addl);
  // //base.insert(addl.begin(), addl.end());
  // bool isDirty = (n != base.size());
  // return isDirty;
}

//llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const SrcOrSink_t &src) {
//  dumpSrcOrSink(os, src, nullptr);
//  return os;
//}


Value* passThruGep(Value* val) {
  // if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(val)) {
  // The GEPOperator class handles correctly both getelementpointer instructions and constant expressions
  if (llvm::GEPOperator* gep = dyn_cast<llvm::GEPOperator>(val)) {
    return gep->getPointerOperand();
  } else {
    return val;
  }
}


class AliasedTaintMap {
  public:
  static map<Value*, SensSrcSet_t> globalSrcTaintSet;
  map<Value*, SensSrcSet_t> baseTaintOf;
  map<Value*, set<Value*>> aliasesOf;

  bool addTaint(Value* loc, SensSrc_t src) {
    /*
     * Associate function arg or ret value with taint source struct
     * If marking a global variable as tainted by another src, return true so we can update the function analysis worklist
     */
    llvm::GlobalVariable* gvar;
    map<Value*, SensSrcSet_t>* taintMap = &baseTaintOf;
    loc = passThruGep(loc);
    if ((gvar = llvm::dyn_cast<llvm::GlobalVariable>(loc)) && src.ixArg != RETVAL_CODE) {
      // llvm::outs() << "Global here! " << *gvar << "\n";
      taintMap = &globalSrcTaintSet;
      if (src.callsite == nullptr)
      {
        return false;
      }
    }
    auto itAli = aliasesOf.find(loc);
    if (itAli == aliasesOf.end()) {
      (*taintMap)[loc].insert(src);
    } else {
      set<Value*>& aliases = itAli->second;
      for (Value* baseLoc: aliases) {
        (*taintMap)[baseLoc].insert(src);
      }
    }
    return gvar;
  }

  bool addTaintSet(Value* loc, SensSrcSet_t srcSet) {
    bool addedToGlobalSet = false;
    loc = passThruGep(loc);
    for (auto src : srcSet) {
      if (addTaint(loc, src))
        addedToGlobalSet = true;
    }
    return addedToGlobalSet;
  }

  SensSrcSet_t getTaintAsSingleSet(Value* loc) {
    /*
     * Return the source/set of sources that have tainted this variable
     */
    llvm::GlobalVariable* gvar;
    map<Value*, SensSrcSet_t>* taintMap = &baseTaintOf;
    loc = passThruGep(loc);
    if ((gvar = llvm::dyn_cast<llvm::GlobalVariable>(loc))) {
      // llvm::outs() << "Global here! " << *gvar << "\n";
      taintMap = &globalSrcTaintSet;
    }
    auto itAli = aliasesOf.find(loc);
    if (itAli == aliasesOf.end()) {
      return (*taintMap)[loc];
    } else {
      SensSrcSet_t ret;
      set<Value*>& aliases = itAli->second;
      for (Value* baseLoc: aliases) {
        extendWith(ret, (*taintMap)[baseLoc]);
      }
      return ret;
    }
  }

  void addAlias(Value* alias, Value* baseLoc) {
    // TODO: Handle the case where baseLoc is a phi node
    alias = passThruGep(alias);
    baseLoc = passThruGep(baseLoc);
    aliasesOf[alias].insert(baseLoc);
    // outs() << "ALIAS   ";
    // alias->dump();
    // outs() << "BASELOC ";
    // baseLoc->dump();
  }

  size_t calcSize() const {
    size_t ret = 0;
    for (auto const& [sink, srcSet] : baseTaintOf) {
      ret += srcSet.size();
    }
    for (auto const& [alias, baseLocs] : aliasesOf) {
      ret += baseLocs.size();
    }
    return ret;
  }

  void dump() {
    llvm::raw_ostream& os = errs();
    os << "=================\n";
    for (auto const& [sink, srcSet] : baseTaintOf) {
      if (srcSet.size() > 0) {
        os << "baseTaintOf ";
        sink->dump();
        os << "  source count = " << srcSet.size() << "\n";
      }
    }
  }

};
map<Value*, SensSrcSet_t> AliasedTaintMap::globalSrcTaintSet; // needed here to provide definition for static class member

//////////////////////////////////////////////////////////////////////////////

using TaintMapType = AliasedTaintMap;
//using TaintMapType = map<Value*, SensSrcSet_t>;

//////////////////////////////////////////////////////////////////////////////

template<typename T>
class WorkList {
  public:
  queue<T> workList;
  set<T> workSet;

  bool empty() {
    return workList.empty();
  }

  void add(T item) {
    if (workSet.count(item) == 0) {
      workSet.insert(item);
      workList.push(item);
    }
  }

  void addSet(set<T> itemSet) {
    for (T item : itemSet) {
      add(item);
    }
  }

  T pop() {
    T ret = workList.front();
    workList.pop();
    workSet.erase(ret);
    return ret;
  }
};


#define soft_check(e, ctx) if (!(e)) {outs() << "Failed: " << #e << ", " << ctx << "\n";}

//////////////////////////////////////////////////////////////////////////////

static cl::opt<std::string> SourcesAndSinksFile("sources-and-sinks",
                             cl::desc("File identifying sources and sinks"),
                             cl::Required, cl::ValueRequired);

static cl::opt<std::string> TaintCpFile("taint-copiers",
                                        cl::desc("File identifying functions that copy taint from one entity to another"),
                                        cl::Required, cl::ValueRequired);

static cl::opt<std::string> WrappersFile("wrappers",
                             cl::desc("File identifying wrapper functions"),
                             cl::ValueRequired);

#if USE_OLD_PASS_MANAGER
class TaintPass : public llvm::ModulePass
#else
class TaintPass : public llvm::PassInfoMixin<TaintPass>
#endif
{
public:
  #if USE_OLD_PASS_MANAGER
  static char ID;

  TaintPass() : llvm::ModulePass(ID) { }
  ~TaintPass(){ }

  virtual void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
  #endif

  WorkList<Function*> funcWorkList;
  unordered_map<Function*, set<Function*>> callersOfFunc;

  map<Function*, map<Sink_t, SensSrcSet_t>> funcFlowsBySink; // function summaries
  set<Function*> taintCopiers;

  map<SrcOrSink_t, SrcOrSink_t*> scrinksInUse;

  map<Function*, vector<int>> sinksOfFunc;
  map<Function*, vector<int>> sourcesOfFunc;
  set<Function*> wrapperFuncs;

  map<Function*, vector<int>> funcArgSrcCat;
  map<Function*, vector<int>> funcArgSinkCat;
  map<Function*, int> funcRetCat;
  set<Function*> knownExtFuncs;
  set<Function*> unknownExtFuncs;

  SrcOrSink_t* storeScrink(SrcOrSink_t src) {
    SrcOrSink_t* pSrc = scrinksInUse[src];
    if (pSrc == nullptr) {
      pSrc = new SrcOrSink_t(src);
      scrinksInUse[src] = pSrc;
    }
    return pSrc;
  }

  void dumpSrcOrSink(llvm::raw_ostream &os, const SrcOrSink_t &src, string* wrapperIndent) {
    string funcName;
    if (src.auxConst) {
      string auxString = getStringFromConstantExpr(src.auxConst);
      os << "{\"aux_file\":";
      os << "\"" << auxString << "\"}";
      return;
    }
    os << "{\"Func\":";
    if (src.func) {
      funcName = src.func->getName();
      os << "\"" << funcName << "\"";
    } else {
      os << "\"null\"";
    }

    os << ", \"aux\":\"" << getAuxName(src.auxType) << "\"";

    bool is_wrapped = (src.wrapped && wrapperIndent != nullptr);

    if (!is_wrapped) {
      os << ", \"arg\":" << src.ixArg;
    }
    //if (src.auxConst) {
    //  os << ", \"filename\":" << src.ixArg;
    //}
    os << ", \"callsite\": ";
    if (src.callsite) {
      write_file_line_col(src.callsite);
      vector<int>& argCats = funcArgSinkCat[src.func];
      for (int ixArg=0; ixArg < argCats.size(); ixArg++) {
        if (argCats[ixArg] != AUX_TYPE_FILE) {
          continue;
        }
        Value* arg = src.callsite->getArgOperand(ixArg);
        //dump(arg);
        llvm::LoadInst* load = dyn_cast<LoadInst>(arg);
        if (load && dyn_cast<GlobalValue>(load->getPointerOperand())) {
          vector<string> std_files = {"stdin", "stdout", "stderr"};
          for (string sf : std_files) {
            if (load->getPointerOperand()->getName() == sf) {
              //load->getPointerOperand()->dump();
              outs() << ", \"FILE*\":\"" << sf << "\"";
            }
          }
        }
        //outs() << arg;
      }

      //src.callsite->print(os);
    } else {
      os << "null";
    }

    if (is_wrapped) {
      os << ", \"wrapped\": \n" << *wrapperIndent;
      dumpSrcOrSink(os, *src.wrapped, nullptr);
      //os << "";
    }

    os << "}";
  }


  std::pair<std::string, std::vector<std::string>>
  parse_arg_string(std::string s)
  {
    size_t pos, base;
    std::string split{" -> "};
    std::string argname, flow;
    std::vector<std::string> argv{};
    std::pair<std::string, std::vector<std::string>> pair{};

    // split arg name and flow info
    if ((pos = s.find(split)) == std::string::npos)
    {
      std::cerr << s << " formatted incorrectly" << std::endl;
      return pair;
    }
    argname = s.substr(0, pos);
    // std::cout << "\tfirst:  " << argname << std::endl;
    base = pos+split.length();
    flow = s.substr(base, s.length());
    // std::cout << "\tsecond: " << flow << std::endl;

    // is flow info formatted correctly
    if (flow.find("[ ") == std::string::npos || flow.find(" ]") == std::string::npos)
    {
      std::cerr << flow << " formatted incorrectly" << std::endl;
      return pair;
    }

    // parse arg taint flows
    argv.push_back(argname);
    if (flow.size() > 3) // only other option (via. if statements above) is flow.size == 3
    {
      flow = flow.substr(2, flow.length()-4); // cut out whitespace and '[', ']'
      // std::cout << "\tnew flow: |" << flow << "|" << std::endl;
      split = " , ";
      while ((pos = flow.find(split)) != std::string::npos)
      {
        std::string s = flow.substr(0, pos);
        // std::cout << "\t\tfind: " << s << std::endl;
        argv.push_back(s);
        flow.erase(0, pos + split.length());
      }
      // std::cout << "\t\tfind: " << flow << std::endl; // last rhs of " , "
      argv.push_back(flow);
    }
    pair = std::make_pair(argname, argv);
    // std::cout << "\tpair: " << std::get<0>(pair) << " | [";
    // for (auto& item : std::get<1>(pair))
    //   std::cout << item << ",";
    // std::cout << "]" << std::endl;

    return pair;
  }

  std::map<std::string, std::vector<std::pair<std::string, std::vector<std::string>>>> parse_taint_cp_file() {
    // below data structure: {'libcfn': [('argname', ['taint_flows_to_argname',...?]),...]}
    std::map<std::string, std::vector<std::pair<std::string, std::vector<std::string>>>> fnprototype_map;
    std::string   line;
    std::ifstream file{TaintCpFile};

    if (!file.is_open())
    {
      std::cerr << "Failed to open file " << TaintCpFile << std::endl;
      return fnprototype_map;
    }

    // read file line by line
    while (std::getline(file, line))
    {
      size_t pos, lpos, rpos;
      std::string fnname, args;
      std::vector<std::pair<std::string, std::vector<std::string>>> arg_list{};

      // std::cout << "Line: " << line << std::endl;
      // get fnname
      if ((pos = line.find(" ")) == std::string::npos ||
          (lpos = line.find("(")) == std::string::npos ||
          (rpos = line.find(")")) == std::string::npos)
      {
        std::cerr << TaintCpFile << " formatted incorrectly. Not parsing file" << std::endl;
        return fnprototype_map;
      }
      fnname = line.substr(0, pos);
      // std::cout << fnname << std::endl;
      args   = line.substr(lpos+2, rpos-lpos-3);
      // std::cout << args << "| " << args.size() << std::endl;

      fnprototype_map[fnname] = arg_list;

      std::string delim{" ]"};
      while ((pos = args.find(delim)) != std::string::npos)
      {
        std::string s = args.substr(0, pos+2);
        if (s[0] == ',')
          s.erase(0, 2);
        // std::cout << "find: " << s << std::endl;
        fnprototype_map[fnname].push_back(parse_arg_string(s));
        args.erase(0, pos + delim.length());
      } // should be no string left
    }
    file.close();
    return fnprototype_map;
  }

  void parse_taint_copiers(Module &M) {
    std::map<std::string, std::vector<std::pair<std::string, std::vector<std::string>>>> fnprototype_map;
    fnprototype_map = parse_taint_cp_file();

    // std::cout << "\n\nModule fns:" << std::endl;
    // for (const llvm::Function& fn : M.getFunctionList())
    //   std::cout << "Fn: " << fn.getName().data() << std::endl;

    for (auto const& x : fnprototype_map)
    {
      std::string libcfn = x.first;
      std::vector<std::pair<std::string, std::vector<std::string>>> val = x.second;
      Function* libc_fnptr;
      int idx;
      std::map<std::string, int> argidx_map; // argument name to idx

      argidx_map["return"] = RETVAL_CODE;

      if (!(libc_fnptr = M.getFunction(libcfn)))
      {
        bool found = false;
        // handle llvm intrinsic functions like memcpy
        // llvm.memcpy.p0i8.p0i8.i64 for memcpy (.p0i8.p0i8.i64 might not be per architecture, [.p0i8.p0i8.i32?]) so only use initial 'llvm.'+name
        for (llvm::Function& fn : M.getFunctionList())
        {
          std::string fnname = fn.getName().data();
          std::string intrinsic_fn = "llvm." + libcfn;
          if ( intrinsic_fn.length() <= fnname.length() && fnname.substr(0, intrinsic_fn.length()) == intrinsic_fn )
          {
            found = true;
            libc_fnptr = &fn;
            break;
          }
        }
        if (!found)
          continue;
      }
      knownExtFuncs.insert(libc_fnptr);
      taintCopiers.insert(libc_fnptr);

      // index arguments first because we refer 'back' at them. otherwise, this would cause an error:
      // fn ( arg1 -> [ arg2 ], arg2 -> [] )
      idx = 0;
      for (std::pair<std::string, std::vector<std::string>>& p : val)
      {
        argidx_map[std::get<0>(p)] = idx++;
      }

      idx = 0;
      for (std::pair<std::string, std::vector<std::string>>& p : val)
      {
        std::string argname;
        Sink_t arg_sink;

        argname = std::get<0>(p);
        // std::cout << "\t" << argname << ": [";
        arg_sink = {libc_fnptr, idx++, nullptr};
        // arg_sink = {libc_fnptr, idx, nullptr};
        // argidx_map[argname] = idx++;
        for (std::string flowname : std::get<1>(p))
        {
          SensSrc_t arg_src;
          arg_src = {libc_fnptr, argidx_map[flowname], nullptr};
          funcFlowsBySink[libc_fnptr][arg_src].insert(arg_sink);
          // funcFlowsBySink[libc_fnptr][arg_sink].insert(arg_src);
          // make argsrc w/ index from argidx_map
          // std::cout << flowname << ",";
        }
        // std::cout << "]\n";
      }

      // printFuncSummary(*libc_fnptr);
    }
    // std::cout << std::endl;
  }


  void populate_sources_and_sinks_2(Module &M) {
    std::string filename = SourcesAndSinksFile;
    std::ifstream file(filename);
    if (!file.is_open()) {
      std::cerr << "Failed to open file " << filename << std::endl;
      return;
    }

    vector<string> foundFuncs;
    vector<string> missingFuncs;

    std::string line;
    while (std::getline(file, line)) {
      std::istringstream iss(line);
      std::string type, funcName;

      // Read the function name
      iss >> funcName;

      Function* func = M.getFunction(funcName);
      if (!func) {
        missingFuncs.push_back(funcName);
        continue;
      }
      knownExtFuncs.insert(func);
      foundFuncs.push_back(funcName);

      funcArgSrcCat[func].resize(func->arg_size());
      // Variadic functions get 1 more sink type for their variadic args
      funcArgSinkCat[func].resize(func->arg_size() + (func->isVarArg() ? 1 : 0));

      // Read all arguments for this function
      std::string curcat;
      ssize_t ixArg = -1;
      while (iss >> curcat) {
        ixArg++;
        bool isRet = false;
        if (curcat == "-"s) {
          continue;
        } else if (curcat == "->"s) {
          iss >> curcat;
          isRet = true;
        } else if ((size_t)ixArg >= func->arg_size() && !func->isVarArg()) {
          outs() << "Error: " << funcName << ": too many arguments!\n";
          continue;
        }

        int taint_cat = AUX_TYPE_MAIN;
        bool isSrc = false;
        bool isSink = false;
        if (curcat.substr(0,4) == "File"s) {
          curcat = curcat.substr(4, curcat.size());
          taint_cat = AUX_TYPE_FILE;
        }
        if (curcat == "Src"s) {
          isSrc = true;
        } else if (curcat == "Sink"s) {
          isSink = true;
        } else if (curcat == "SrcAndSink"s) {
          isSrc = true;
          isSink = true;
        } else if (curcat == "none"s) {
          // do nothing in this case
        } else {
          outs() << "Error: unrecognized catcode '" << curcat << "', function " << funcName << "\n";
        }
        if (isRet) {
          if (isSrc) {
            funcRetCat[func] = taint_cat;
          }
          if (isSink) {
            outs() << "Error: " << funcName << ": return value cannot be a sink!\n";
          }
        } else {
          if (isSrc) {
            funcArgSrcCat[func][ixArg] = taint_cat;
            // llvm::outs() << "Reading src: " << llvm::demangle(func->getName().data()) << " (" << func->getName() << ")\n";
          }
          if (isSink) {
            funcArgSinkCat[func][ixArg] = taint_cat;
            // llvm::outs() << "Reading sink: " << llvm::demangle(func->getName().data()) << " (" << func->getName() << ")\n";
          }
        }

      }

    }
    outs() << "Found " << foundFuncs.size() << " source/sink functions in program; " << missingFuncs.size() << " are absent.\n";

    file.close();
  }

  void populate_wrappers(Module &M) {
    std::string filename = WrappersFile; //"/host_dmc/gpt/wrappers.txt";
    if (WrappersFile == "") {
      errs() << "No wrappers file specified.\n";
      return;
    }
    std::ifstream file(filename);
    if (!file.is_open()) {
      errs() << "Failed to open wrappers file '" << filename << "'\n";
      return;
    }
    std::string line;
    while (std::getline(file, line)) {
      std::istringstream iss(line);
      std::string funcName;
      iss >> funcName;
      Function* func = M.getFunction(funcName);
      if (!func) {
        outs() << "Failed to find wrapper function " << funcName << "\n";
      } else {
        wrapperFuncs.insert(func);
      }
    }
    file.close();
  }


  void plugInSummary(CallBase* callsite, TaintMapType& taintOfVal) {
    llvm::Function* callee = callsite->getCalledFunction();
    llvm::Function* caller = callsite->getFunction();
    if (!callee) {return;}
    for (auto const& [sumSink, sumSources] : funcFlowsBySink[callee]) {
      Value* valToTaint = nullptr;
      SensSrcSet_t* pTaintDest;
      if (sumSink.callsite == nullptr) {
        assert(sumSink.func == callee);
        if (sumSink.ixArg == RETVAL_CODE) {
          valToTaint = callsite;
        } else {
          valToTaint = callsite->getArgOperand(sumSink.ixArg);
        }
        pTaintDest = nullptr; //&taintOfVal[valToTaint];
      } else {
        if (wrapperFuncs.count(callee)) {
          int wrapperArgIx = 0; // TODO: FIXME!!!
          SrcOrSink_t* pSumSink = storeScrink(sumSink);
          SensSrc_t sink = {callee, wrapperArgIx, callsite, sumSink.auxType, (SrcOrSink_t*)(pSumSink)};
          pTaintDest = &funcFlowsBySink[caller][sink];
        } else {
          pTaintDest = &funcFlowsBySink[caller][sumSink];
        }
      }
      for (const SensSrc_t& sumSrc : asSingleSet(sumSources)) {
        //errs() << "sumSrc = ";
        //dumpSrcOrSink(errs(), sumSrc, nullptr);
        //errs() << "\n";
        if (!sumSrc.isSummaryScrink()) {
          if (sumSink.callsite != nullptr) {
            // Do nothing; no need to propagate fully concrete flows upwards.
          } else {
            SensSrc_t insSrc;
            if (wrapperFuncs.count(callee)) {
              int wrapperArgIx = 0; // TODO: FIXME!!!
              SrcOrSink_t* pSumSrc = storeScrink(sumSrc);
              SensSrc_t src = {callee, wrapperArgIx, callsite, sumSrc.auxType, (SrcOrSink_t*)(pSumSrc)};
              insSrc = src;
            } else {
              insSrc = sumSrc;
            }
            if (pTaintDest) {
              pTaintDest->insert(insSrc);
            } else {
              taintOfVal.addTaint(valToTaint, insSrc);
            }
          }
        } else {
          assert(sumSrc.func == callee);
          assert(sumSrc.ixArg != RETVAL_CODE) ;
          Value* actArg = callsite->getArgOperand(sumSrc.ixArg);
          if (pTaintDest) {
            extendWith(*pTaintDest, taintOfVal.getTaintAsSingleSet(actArg));
          } else {
            taintOfVal.addTaintSet(valToTaint, taintOfVal.getTaintAsSingleSet(actArg));
          }
        }
      }
    }
  }

  set<Function*> findCallers(Function *callee) {
    set<Function*> hit;
    for (auto *U : callee->users()) {
      if (auto *callInst = dyn_cast<CallInst>(U)) {
        Function *caller = callInst->getFunction();
        hit.insert(caller); // sets do not contain duplicates
      }
    }
    return hit;
  }

#if USE_OLD_PASS_MANAGER
  bool runOnModule(Module &M) override
#else
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM)
#endif
  {

    //populate_sources_and_sinks_1(M);
    populate_sources_and_sinks_2(M);
    populate_wrappers(M);
    parse_taint_copiers(M);
    funcWorkList.add(nullptr);
    map<Function*, set<Function*>> calleesOfFunc;
    for (Function &F : M) {
      calleesOfFunc[&F];
    }
    for (Function &F : M) {
      if (F.isDeclaration()) {
        continue;
      }
      callersOfFunc[&F] = findCallers(&F);
      for (Function* caller : callersOfFunc[&F]) {
        if (caller != &F) {
          calleesOfFunc[caller].insert(&F);
        }
      }
    }
    
    //outs() << "Size = " << calleesOfFunc.size() << "\n";
    {
      int count=0;
      for (bool isStuck=false; isStuck==false && count < 20; ) {
        count++;
        isStuck = true;
        int numScheduled = 0;
        for (auto const& [func, callees] : calleesOfFunc) {
          if (funcWorkList.workSet.count(func)) {
            continue;
          }
          if (callees.size() == 0) {
            funcWorkList.add(func);
            numScheduled++;
            isStuck = false;
            for (Function* caller : callersOfFunc[func]) {
              calleesOfFunc[caller].erase(func);
            }
          }
        }
        //outs() << "Count = " << count << ", numScheduled = " << numScheduled << ", WorkList size = " << funcWorkList.workList.size() << "\n";
      }
      // count = 0;
      // for (auto const& [func, callees] : calleesOfFunc) {
      //   if (funcWorkList.workSet.count(func)) {continue;}
      //   if (callees.size() == 0) {continue;}
      //   outs() << func->getName() << ": " << (*callees.begin())->getName() << "\n";
      //   //if (count++ > 20) {break;};
      // }
      for (Function &F : M) {
        if (F.isDeclaration()) {continue;}
        funcWorkList.add(&F);
      }
    }

    int round = 1;
    while (!funcWorkList.empty()) {
      Function* func = funcWorkList.pop();
      if (func == nullptr) {
        if (!funcWorkList.empty()) {
          funcWorkList.add(nullptr);
        }
        errs() << "Round " << (round++) << " (" << funcWorkList.workList.size() << " functions in worklist) \n";
        errs().flush();
        continue;
      }
      analyzeFunc(*func);
    }
    outs() << "\n############################################################\n";
    outs() << "# Function summaries\n";
    outs() << "############################################################\n";
    for (Function &F : M) {
      if (F.isDeclaration()) {continue;}
      printFuncSummary(F);
    }
    outs() << "\n############################################################\n";
    outs() << "# FULL FLOWS\n";
    outs() << "############################################################\n";
    for (Function &F : M) {
      if (F.isDeclaration()) {continue;}
      printFuncTaints(F);
    }

    outs() << "\n############################################################\n";
    outs() << "Unrecognized external functions: [ ";
    for (Function* func : unknownExtFuncs) {
      outs() << func->getName() << " ";
    }
    outs() << "]\n";
    #if USE_OLD_PASS_MANAGER
    return true;
    #else
    return PreservedAnalyses::all();
    #endif
  }


  std::string getGlobalStringLiteral(llvm::Value* value) {
    llvm::GlobalVariable* GV = llvm::dyn_cast<llvm::GlobalVariable>(value);
    if (GV) {
      GV->dump();
    }
    if (GV && GV->isConstant() && GV->hasInitializer()) {
      llvm::ConstantDataArray* CDA = llvm::dyn_cast<llvm::ConstantDataArray>(GV->getInitializer());
      if (CDA && CDA->isString()) {
        return CDA->getAsString().str();
      }
    }
    return ""; // Return empty string if conditions are not met
  }

  std::string getStringFromConstantExpr(Value* val) {
    llvm::ConstantExpr* constExpr = llvm::dyn_cast<llvm::ConstantExpr>(val);
    if (!constExpr) {return "";}
    if (constExpr->getOpcode() == Instruction::GetElementPtr) {
      // Check if it's actually a GEP to a global variable
      if (GlobalVariable* GV = dyn_cast<GlobalVariable>(constExpr->getOperand(0))) {
        if (GV->hasInitializer()) {
          Constant* init = GV->getInitializer();
          // Assuming initializer is a constant array (common for strings)
          if (ConstantDataArray* cda = dyn_cast<ConstantDataArray>(init)) {
            if (cda->isCString()) {
              return cda->getAsCString().str();
            }
          }
        }
      }
    }
    return ""; // Return empty string if val isn't a string constant
  }


  inline void analyzeInst(llvm::Instruction* inst, Function* func, TaintMapType& taintOfVal, set<llvm::GlobalVariable*>& gvarSet) {
    bool is_cmp = (inst->getOpcode() == llvm::Instruction::ICmp ||
                   inst->getOpcode() == llvm::Instruction::FCmp);
    if (is_cmp) {
      return;
    }
    if (llvm::isa<llvm::CallBase>(inst)) {
      llvm::CallBase* callsite = dyn_cast<CallBase>(inst);
      llvm::Function* callee = callsite->getCalledFunction();
      if (callee) {
        // Check for string literals that should be taint sources (e.g., filenames).
        for (int ixArg=0; ixArg < callsite->arg_size(); ixArg++) {
          Value* arg = callsite->getArgOperand(ixArg);
          llvm::ConstantExpr * ce = llvm::dyn_cast<llvm::ConstantExpr>(arg);
          if (ce) {
            string str_const = getStringFromConstantExpr(ce);
            if (looks_like_filename(str_const)) {
              SensSrc_t src = {.auxType=AUX_TYPE_MAIN, .auxConst=arg};
              taintOfVal.addTaint(arg, src);
              //errs() << "HELLO\n";
            }
          }
        }
        // std::string calleeName = llvm::demangle(callee->getName().data());

        if (callee->isDeclaration()) {
	  if (taintCopiers.count(callee) != 0)
	  {
	    plugInSummary(callsite, taintOfVal);
	  }
          // If a func has only a decl, then it's an external function.
          for (int arg=0; arg < callsite->arg_size(); arg++) {
            int sink_arg = arg >= funcArgSinkCat[callee].size() ? funcArgSinkCat[callee].size() - 1 : arg;
            // All variadic fns must have >=1 fixed arg
            if (sink_arg == -1) {break;}
            int auxType = funcArgSinkCat[callee][sink_arg];
            if (auxType == AUX_TYPE_NULL) {continue;}
            Sink_t sink = {callee, arg, callsite, auxType};
            // if ((sink == <passThruGep(callsite->getArgOperand(arg))>))
            funcFlowsBySink[func][sink] = taintOfVal.getTaintAsSingleSet(callsite->getArgOperand(arg));
            llvm::GlobalVariable* gv = llvm::dyn_cast<llvm::GlobalVariable>(passThruGep(callsite->getArgOperand(arg)));
            if (gv && !gv->isConstant()) {
              fnsReferencingGvar[gv].insert(callsite->getFunction()); // callsite->getFunction() = caller
              gvarSet.insert(gv);
              // llvm::outs() << "Added global taint at " << *callsite << " for " << gv->getName() << "(" << !gv->isConstant() << ")\n";
            }
          }
          if (callee->getName().startswith(StringRef("llvm."))) {
            // TODO
          } else {
            if (knownExtFuncs.count(callee) == 0) {
              unknownExtFuncs.insert(callee);
            }
            //sinkSites.insert(callsite);
          }
          // bool flag = false;
          for (int arg=-1; arg < (ssize_t) funcArgSrcCat[callee].size(); arg++) {
            int auxType;
            if (arg == RETVAL_CODE) {
              auxType = funcRetCat[callee];
            } else {
              auxType = funcArgSrcCat[callee][arg];
            }
            if (auxType == AUX_TYPE_NULL) {
              continue;
            }
            // flag = true;
            SensSrc_t src = {callee, arg, callsite, auxType};
            if (arg == RETVAL_CODE) {
              taintOfVal.addTaint(callsite, src);
            } else {
              if (taintOfVal.addTaint(callsite->getArgOperand(arg), src)) {
                llvm::GlobalVariable* gv = llvm::dyn_cast<llvm::GlobalVariable>(passThruGep(callsite->getArgOperand(arg)));
                if (!gv->isConstant()) {
                  fnsReferencingGvar[gv].insert(callsite->getFunction()); // callsite->getFunction() = caller
                  gvarSet.insert(gv);
                  // llvm::outs() << "Added global taint at " << *callsite << " for " << gv->getName() << "(" << !gv->isConstant() << ")\n";
                }
              }
            }
          }
          // if (flag) {
          //   errs() << "\n=====================\n" <<
          //     "FUNCTION CALL " << callee->getName() << "\n";
          //   taintOfVal.dump();
          // }
        } else {
          plugInSummary(callsite, taintOfVal);
        }
      }
    }
    else if (inst->getOpcode() == llvm::Instruction::Store) {
      llvm::StoreInst* store = dyn_cast<StoreInst>(inst);
      taintOfVal.addTaintSet(store->getPointerOperand(),
          taintOfVal.getTaintAsSingleSet(store->getValueOperand()));
    }
    else if (PHINode* phi = dyn_cast<PHINode>(inst)) {
      for (Value* incoming : phi->incoming_values()) {
        taintOfVal.addAlias(phi, incoming);
      }
      goto normal_inst;
    }
    else {
      normal_inst:
      for (auto op = inst->op_begin(); op != inst->op_end(); ++op) {
        Value *operand = *op;
        SensSrcSet_t operandTaint = taintOfVal.getTaintAsSingleSet(operand);
        taintOfVal.addTaintSet(inst, operandTaint);
      }
    }
  }

  void analyzeFunc(llvm::Function &F) {
    map<Sink_t, SensSrcSet_t> oldSummary = funcFlowsBySink[&F]; // deep copy
    TaintMapType taintOfVal;
    set<llvm::GlobalVariable*> gvarSet;
    // Each argument is tainted with itself.
    {
      int ixArg = -1;
      for (auto &Arg : F.args()) {
        ixArg++;
        taintOfVal.addTaint(&Arg, (SensSrc_t){&F, ixArg, nullptr});
      }
    }

    //bool isDirty = true;
    //set<CallInst*> sinkSites;
    while (true) {
      size_t sizeAtStart = taintOfVal.calcSize();
      //isDirty = false;
      for (auto &B : F) {
        for (auto &I : B) {
          analyzeInst(&I, &F, taintOfVal, gvarSet);
        }
      }
      if (sizeAtStart == taintOfVal.calcSize()) {
        break;
      }
    }

    // Look at all the "return" instructions in the fuction
    SensSrcSet_t retTaint;
    for (auto &B : F) {
      for (auto &I : B) {
        llvm::ReturnInst* ret = dyn_cast<ReturnInst>(&I);
        if (!ret) {continue;}
        Value* retVal = ret->getReturnValue();
        if (retVal == nullptr) {continue;}
        extendWith(retTaint, taintOfVal.getTaintAsSingleSet(retVal));
      }
    }

    // Taint of return value
    Sink_t retSink = {&F, RETVAL_CODE, nullptr};
    funcFlowsBySink[&F][retSink] = retTaint;

    // Taint of "OUT"/"INOUT" arguments
    for (int ixArg=0; ixArg < F.arg_size(); ixArg++) {
      Sink_t argSink = {&F, ixArg, nullptr};
      funcFlowsBySink[&F][argSink] = taintOfVal.getTaintAsSingleSet(F.getArg(ixArg));
    }

    if (oldSummary != funcFlowsBySink[&F]) {
      set<llvm::Function*> noRepeats;
      for (Function* caller : callersOfFunc[&F]) {
        funcWorkList.add(caller);
        // outs() << "Adding caller *" << caller->getName() << "* of " << F.getName() << " for analysis\n";
      }
      for (llvm::GlobalVariable* gv : gvarSet)
      {
        for (llvm::Function* fn : fnsReferencingGvar[gv]) {
          // no repeat analysis on this function we just analyzed or on any of the recently added callers of the recently analyzed function
          if (callersOfFunc[&F].count(fn) == 0 && fn != &F)
          {
            funcWorkList.add(fn);
            // llvm::outs() << "Adding \"" << gv->getName() << "\" user " << fn->getName() << " for analysis\n";
          }
        }
      }
    }

    return;

  }

  void printFuncSummary(Function& F) {
    outs() << "################## \n";
    outs() << "# Function: " << F.getName() << "\n";
    // Print return-value taint.
    Sink_t retSink = {&F, RETVAL_CODE, nullptr};
    SensSrcSet_t& retTaint = funcFlowsBySink[&F][retSink];
    llvm::outs() << "\"Return\": [";
    for (const SensSrc_t& src : asSingleSet(retTaint)) {
      dumpSrcOrSink(outs(), src, nullptr);
      llvm::outs() << ", ";
    }
    llvm::outs() << "]\n";

    // Print OUT-argument taints.
    {
      int ixArg = -1;
      for (auto &Arg : F.args()) {
        ixArg++;
        llvm::outs() << "Arg " << ixArg << ": " << Arg.getName() << ": ";
        Sink_t argSink = {&F, ixArg, nullptr};
        for (const SensSrc_t& src : asSingleSet(funcFlowsBySink[&F][argSink])) {
          dumpSrcOrSink(outs(), src, nullptr);
          llvm::outs() << ", ";
        }
        llvm::outs() << "\n";
      }
    }

    // Print sink taints.
    llvm::outs() << "\"Sinks\": [\n";
    for (auto const& [sink, taints] : funcFlowsBySink[&F]) {
      if (sink.callsite == nullptr || sink.ixArg == RETVAL_CODE) {
        continue;
      }
      SensSrcSet_t fullTaints;
      SensSrcSet_t halfTaints;
      for (SensSrc_t taint : asSingleSet(taints)) {
        if (!taint.isSummaryScrink()) {
          fullTaints.insert(taint);
        } else {
          halfTaints.insert(taint);
        }
      }
      if (halfTaints.empty()) {
        continue;
      }
      llvm::outs() << "  [";
      write_file_line_col(sink.callsite);
      llvm::outs() << ", \"" << sink.callsite->getCalledFunction()->getName() << " arg " << sink.ixArg << "\", [\n";
      for (const SensSrc_t& src : asSingleSet(halfTaints)) {
        outs() << "    ";
        dumpSrcOrSink(outs(), src, nullptr);
        outs() << ",\n";
      }
      llvm::outs() << "  ]],\n";
    }
    llvm::outs() << "]\n";
  }

  void printFuncTaints(Function& F) {
    bool printedHeader = false;
    StringRef funcName = F.getName(); // for debugging
    (void)funcName;
    // Print full taints.

    for (auto const& [sink, taints] : funcFlowsBySink[&F]) {
      // llvm::outs() << "Evaling " << funcName.data() << " for flow\n";
      // std::cout << "Evaling " << funcName.data() << " for flow" << std::endl;
      if (sink.callsite == nullptr || sink.ixArg == RETVAL_CODE) {
        continue;
      }
      auto callee_name = sink.callsite->getCalledFunction()->getName();
      (void)callee_name;
      if (sink.callsite->getArgOperand(sink.ixArg)) {
        //continue;
      }
      SensSrcSet_t fullTaints;
      for (SensSrc_t taint : asSingleSet(taints)) {
        if (taint.auxConst && sink.auxType != AUX_TYPE_FILE) {
          continue;
        }
        if (!taint.isSummaryScrink()) {
          fullTaints.insert(taint);
        }
      }
      if (fullTaints.empty()) {
        continue;
      }
      if (!printedHeader) {
        printedHeader = true;
        outs() << "################## \n";
        outs() << "# Function: " << F.getName() << "\n";
        llvm::outs() << "<flows>\n[\n";
      }
      llvm::outs() << "  {\"sink\": ";
      string sink_wrap_indent = "      "s;
      dumpSrcOrSink(outs(), sink, &sink_wrap_indent);
      llvm::outs() << ",\n   \"sources\": [\n";
      //write_file_line_col(sink.callsite);
      //llvm::outs() << ", \"" << sink.callsite->getCalledFunction()->getName() << " arg " << sink.ixArg << "\", [\n";
      for (const SensSrc_t& src : asSingleSet(fullTaints)) {
        llvm::outs() << "    ";
        string indent = "      ";
        dumpSrcOrSink(outs(), src, &indent);
        outs() << ",\n";
      }
      llvm::outs() << "  ]},\n";
    }
    if (!printedHeader) {
      outs() << "Function " << F.getName() << ": no full flows.\n";
    } else {
      llvm::outs() << "]\n</flows>\n";
    }
  }


};


#if USE_OLD_PASS_MANAGER
#else

llvm::PassPluginLibraryInfo getTaintPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "TaintPass", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
              [](StringRef Name, ModulePassManager &MPM,
                 ArrayRef<PassBuilder::PipelineElement>) {
                if (Name == "taint") {
                  MPM.addPass(TaintPass());
                  return true;
                }
                return false;
              });
          }};
}

#endif


#if USE_OLD_PASS_MANAGER

char TaintPass::ID = 0;

static llvm::RegisterPass<TaintPass> X(
              "taint",
              "Taint: Track flow of sensitive data",
              false, true
);

#else

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getTaintPassPluginInfo();
}

#endif


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
