#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/Local.h"
#include "SplitBasicBlock.h"
#include <vector>
#include <cstdlib>
#include <ctime>
using namespace llvm;
using std::vector;
 
namespace{
    class Flattening : public FunctionPass{
        public:
            static char ID;
            Flattening() : FunctionPass(ID){}

            // 对函数 F 进行平坦化
            void flatten(Function &F);
    
            bool runOnFunction(Function &F);

            // 修复 PHINode 和逃逸变量
            void fixStack(Function &F);
    };
}

bool Flattening::runOnFunction(Function &F){
    FunctionPass *pass = createSplitBasicBlockPass();
    pass->runOnFunction(F);
    flatten(F);
    return true;
}
 
void Flattening::flatten(Function &F){
    // 基本块数量不超过1则无需平坦化
    if(F.size() <= 1){
        return;
    }
    // Lower switch
    // 调用 Lower switch 会导致崩溃，解决方法未知
    //FunctionPass *pass = createLowerSwitchPass();
    //pass->runOnFunction(F);
    IntegerType *i32 = Type::getInt32Ty(F.getContext());
    // 将除入口块（第一个基本块）以外的基本块保存到一个 vector 容器中，便于后续处理
    // 首先保存所有基本块
    vector<BasicBlock*> origBB;
    for(BasicBlock &BB: F){
        origBB.push_back(&BB);
    }

    // 从vector中去除第一个基本块
    origBB.erase(origBB.begin());
    BasicBlock &firstBB = F.front();
    // 如果第一个基本块的末尾是条件跳转
    if(BranchInst *br = dyn_cast<BranchInst>(firstBB.getTerminator())){
        if(br->isConditional()){
            BasicBlock *newBB = firstBB.splitBasicBlock(br, "newBB");
            origBB.insert(origBB.begin(), newBB);
        }
    }

    // 创建分发块和返回块
    BasicBlock *dispatchBB = BasicBlock::Create(F.getContext(), "dispatchBB", &F, &firstBB);
    BasicBlock *returnBB = BasicBlock::Create(F.getContext(), "returnBB", &F, &firstBB);
    BranchInst::Create(dispatchBB, returnBB);
    firstBB.moveBefore(dispatchBB);
    // 去除第一个基本块末尾的跳转
    firstBB.getTerminator()->eraseFromParent();
    // 使第一个基本块跳转到dispatchBB
    BranchInst *brDispatchBB = BranchInst::Create(dispatchBB, &firstBB);

    // 向分发块中插入switch指令和switch on的变量
    srand(time(0));
    int randNumCase = rand();
    AllocaInst *swVarPtr = new AllocaInst(i32, 0, "swVar.ptr", brDispatchBB);
    new StoreInst(ConstantInt::get(i32, randNumCase), swVarPtr, brDispatchBB);
    LoadInst *swVar = new LoadInst(i32, swVarPtr, "swVar", false, dispatchBB);
    // 初始化switch指令的default case
    // default case实际上不会被执行
    BasicBlock *swDefault = BasicBlock::Create(F.getContext(), "swDefault", &F, returnBB);
    BranchInst::Create(returnBB, swDefault);
    SwitchInst *swInst = SwitchInst::Create(swVar, swDefault, 0, dispatchBB);
    // 将原基本块插入到返回块之前，并分配case值
    for(BasicBlock *BB : origBB){
        ConstantInt *numCase = cast<ConstantInt>(ConstantInt::get(i32, randNumCase));
        BB->moveBefore(returnBB);
        swInst->addCase(numCase, BB);
        randNumCase = rand();
    }

    // 在每个基本块最后修改 switchVarPtr 指向的值
    for(BasicBlock *BB : origBB){
        // retn BB
        if(BB->getTerminator()->getNumSuccessors() == 0){
            continue;
        }
        // 非条件跳转
        else if(BB->getTerminator()->getNumSuccessors() == 1){
            BasicBlock *sucBB = BB->getTerminator()->getSuccessor(0);
            BB->getTerminator()->eraseFromParent();
            ConstantInt *numCase = swInst->findCaseDest(sucBB);
            new StoreInst(numCase, swVarPtr, BB);
            BranchInst::Create(returnBB, BB);
            continue;
        }
        // 条件跳转
        else if(BB->getTerminator()->getNumSuccessors() == 2){
            ConstantInt *numCaseTrue = swInst->findCaseDest(BB->getTerminator()->getSuccessor(0));
            ConstantInt *numCaseFalse = swInst->findCaseDest(BB->getTerminator()->getSuccessor(1));
            BranchInst *br = cast<BranchInst>(BB->getTerminator());
            SelectInst *sel = SelectInst::Create(br->getCondition(), numCaseTrue, numCaseFalse, "", BB->getTerminator());
            BB->getTerminator()->eraseFromParent();
            new StoreInst(sel, swVarPtr, BB);
            BranchInst::Create(returnBB, BB);
        }
    }
    fixStack(F);
}

void Flattening::fixStack(Function &F) {
    vector<PHINode*> origPHI;
    vector<Instruction*> origReg;
    BasicBlock &entryBB = F.getEntryBlock();
    for(BasicBlock &BB : F){
        for(Instruction &I : BB){
            if(PHINode *PN = dyn_cast<PHINode>(&I)){
                origPHI.push_back(PN);
            }else if(!(isa<AllocaInst>(&I) && I.getParent() == &entryBB) 
                && I.isUsedOutsideOfBlock(&BB)){
                origReg.push_back(&I);
            }
        }
    }
    for(PHINode *PN : origPHI){
        DemotePHIToStack(PN, entryBB.getTerminator());
    }
    for(Instruction *I : origReg){
        DemoteRegToStack(*I, entryBB.getTerminator());
    }
}
 
char Flattening::ID = 0;
static RegisterPass<Flattening> X("fla", "Flatten the basic blocks in each function.");