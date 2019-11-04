#ifndef _HI_FunctionInterfaceInfo
#define _HI_FunctionInterfaceInfo
// related headers should be included.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>
#include "HI_print.h"
#include "HI_SysExec.h"
#include "clang/AST/Type.h"
#include "clang/Driver/Options.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Parse/ParseAST.h"
#include <stdio.h>
#include <string>
#include <ios>
#include <stdlib.h>
#include <map>
#include <set>
#include <vector>
#include <sstream>
#include "llvm/Support/raw_ostream.h"
#include <sys/time.h>
#include "HI_StringProcess.h"

using namespace clang;


// According the official template of Clang, this is a frontend factory with function createASTConsumer(), which
// will generator a AST consumer. We can first create a rewriter and pass the reference of the
// rewriter to the factory. Finally,  we can pass the rewriter reference to the inner visitor.
// rewriter  -> factory -> frontend-action -> ASTconsumer -> Visitor


//                         declare a rewriter
//                               |  pass the reference to
//                  create       V
// frontend Factory ----->   FrontEnd Action
//         |                     |  create / pass the rewriter
//         |   Src Code          V
//         ------------->   AST consumer
//                               |
//                               |  generate AST
//                               V
//                            Visitor (visit the nodes in AST and do the rewritting)


class HI_FunctionInterfaceInfo_Visitor : public RecursiveASTVisitor<HI_FunctionInterfaceInfo_Visitor> 
{ 
public: 
    HI_FunctionInterfaceInfo_Visitor(CompilerInstance &_CI, 
                                    Rewriter &R,std::string _parselog_name, 
                                    std::map<std::string, int> &FuncParamLine2OutermostSize, std::string topFunctioName) : 
                                    CI(_CI), TheRewriter(R) , parselog_name(_parselog_name), 
                                    FuncParamLine2OutermostSize(FuncParamLine2OutermostSize),
                                    topFunctioName(topFunctioName)
    {
        parseLog = new llvm::raw_fd_ostream(_parselog_name.c_str(), ErrInfo, llvm::sys::fs::F_None);
    } 

    ~HI_FunctionInterfaceInfo_Visitor() 
    {
        parseLog->flush();
        delete parseLog;
    } 

    // Toy: access functions in AST
    bool VisitFunctionDecl(FunctionDecl *f) 
    { 
        // Only function definitions (with bodies), not declarations. 

        // f->dump();
        if (f->hasBody()) 
        { 
            Stmt *FuncBody = f->getBody(); // Type name as string 
            QualType QT = f->getReturnType(); 
            std::string TypeStr = QT.getAsString(); // Function name 
            DeclarationName DeclName = f->getNameInfo().getName(); 
            std::string FuncName = DeclName.getAsString(); 
            
            // Add comment before 
            SourceLocation ST = f->getSourceRange().getBegin(); 
            FullSourceLoc FSL(ST, CI.getSourceManager());
            if (FuncName != topFunctioName)
                TheRewriter.InsertText(f->getBeginLoc(), "inline __attribute__((always_inline)) ", false, true); 
                
            if(f->getNumParams() > 0)
            {
                for (int i=0;i<f->getNumParams();i++)
                {
                    std::string argStr = f->parameters()[i]->getQualifiedNameAsString();
                    ParmVarDecl *arg = f->parameters()[i];
                    
                    *parseLog << "// arg#" << i << ": " << argStr << " type: " << arg->getOriginalType().getAsString()  ;
                    if (isa<ConstantArrayType>(arg->getOriginalType()))
                    {
                        auto tmp_arrayTy = cast<ConstantArrayType>(arg->getOriginalType().getTypePtr());
                        
                        const llvm::APInt arraySize = tmp_arrayTy->getSize();
                        *parseLog  << "outermost dimensionSize = " << arraySize.getSExtValue();
                        FuncParamLine2OutermostSize[FuncName+"-"+arg->getNameAsString()+"-"+std::to_string(FSL.getLineNumber())] = arraySize.getSExtValue();
                        *parseLog << "recorded as : " << FuncName+"-"+arg->getNameAsString()+"-"+std::to_string(FSL.getLineNumber()) << " <--> " <<  arraySize.getSExtValue() << "\n";
                    }
                    else if (isa<clang::PointerType>(arg->getOriginalType()))
                    {
                        *parseLog  << "outermost dimensionSize = " << 1;
                        FuncParamLine2OutermostSize[FuncName+"-"+arg->getNameAsString()+"-"+std::to_string(FSL.getLineNumber())] = 1;
                        *parseLog << "recorded as : " << FuncName+"-"+arg->getNameAsString()+"-"+std::to_string(FSL.getLineNumber()) << " <--> " <<  1 << "\n";
                    }
                    *parseLog  << "\n";
                }

            }     

            label_counter = 0;
            for (auto it = f->getBody()->child_begin(), ie = f->getBody()->child_end(); it!=ie; it++)
            {
                traceLoop(*it, f);
            }
        } 
        return true; 
    } 

    void traceLoop(Stmt *curStmt, FunctionDecl *f)
    {
        if (curStmt == nullptr)
            return;
        if (isa<ForStmt>(curStmt))
        {
            
            label_counter++;
            ForStmt *ForStatement = cast<ForStmt>(curStmt); 
            DeclarationName DeclName = f->getNameInfo().getName(); 
            std::string FuncName = DeclName.getAsString(); 
            // llvm::errs() << "find loop in function " << FuncName << "\n";
            //curStmt->dump();
            TheRewriter.InsertText(ForStatement->getBeginLoc(), "Loop_"+FuncName+"_"+std::to_string(label_counter)+": ", false, true); 
        }
        for (auto it = curStmt->child_begin(), ie = curStmt->child_end(); it!=ie; it++)
        {
            traceLoop(*it, f);
        }
    }

    // print the detailed information of the type
    void printTypeInfo(const clang::Type *T);

    // check whether it is a template structure like XXXX<X>
    bool isAPInt(VarDecl *VD);

    // get tht template name
    std::string getAPIntName(VarDecl *VD);

    PrintingPolicy PP()
    {
        return PrintingPolicy(CI.getLangOpts());
    }

private: 
    Rewriter &TheRewriter; 
    std::map<std::string, int> &FuncParamLine2OutermostSize;
    std::string topFunctioName;
    CompilerInstance &CI;
    std::error_code ErrInfo;
    raw_ostream *parseLog;
    std::string parselog_name;

    int label_counter = 0;

}; 





// Implementation of the ASTConsumer interface for reading an AST produced 
// by the Clang parser. 
class HI_FunctionInterfaceInfo_ASTConsumer : public ASTConsumer 
{ 
  public: 
    HI_FunctionInterfaceInfo_ASTConsumer(CompilerInstance &_CI,
                                         Rewriter &R,std::string _parselog_name, 
                                         std::map<std::string, int> &FuncParamLine2OutermostSize, std::string topFunctioName) : 
                Visitor(_CI,R,_parselog_name,FuncParamLine2OutermostSize, topFunctioName),CI(_CI),parselog_name(_parselog_name), topFunctioName(topFunctioName)
    {

    } // Override the method that gets called for each parsed top-level // declaration. 
    bool HandleTopLevelDecl(DeclGroupRef DR) override 
    { 
        for (DeclGroupRef::iterator b = DR.begin(), e = DR.end(); b != e; ++b) 
        { 
            // Traverse the declaration using our AST visitor. 
            Visitor.TraverseDecl(*b); //(*b)->dump(); 
        } 
        return true; 
    } 

private: 
    HI_FunctionInterfaceInfo_Visitor Visitor; 
    CompilerInstance &CI;
    std::string parselog_name;
    std::string topFunctioName;
    
/// Timer

    struct timeval tv_begin;
    struct timeval tv_end;
}; 






// For each source file provided to the tool, a new FrontendAction is created. 
class HI_FunctionInterfaceInfo_FrontendAction : public ASTFrontendAction 
{ 
public: 
    HI_FunctionInterfaceInfo_FrontendAction(const char* _parselog_name,Rewriter &R,const char* _outputCode_name,std::map<std::string, int> &FuncParamLine2OutermostSize, std::string topFunctioName): 
                parselog_name(_parselog_name),TheRewriter(R),outputCode_name(_outputCode_name),
                FuncParamLine2OutermostSize(FuncParamLine2OutermostSize), topFunctioName(topFunctioName) {} 
    void EndSourceFileAction() override 
    { 
        SourceManager &SM = TheRewriter.getSourceMgr(); 
        llvm::errs() << "** EndSourceFileAction for: " << SM.getFileEntryForID(SM.getMainFileID())->getName() << "\n";  // Now emit the rewritten buffer. 
        outputCode = new llvm::raw_fd_ostream(outputCode_name.c_str(), ErrInfo, llvm::sys::fs::F_None);
        TheRewriter.getEditBuffer(SM.getMainFileID()).write(*outputCode); 
        outputCode->flush();
        delete outputCode;
    } 
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override 
    { 
        llvm::errs() << "** Creating AST consumer for: " << file << "\n"; 
        TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts()); 
        return llvm::make_unique<HI_FunctionInterfaceInfo_ASTConsumer>(CI,TheRewriter,parselog_name,FuncParamLine2OutermostSize,topFunctioName); 
    } 
    
private: 
    Rewriter &TheRewriter; 
    std::map<std::string, int> &FuncParamLine2OutermostSize;
    std::string topFunctioName;
    std::string parselog_name;
    std::string outputCode_name;
    raw_ostream *outputCode;
    std::error_code ErrInfo;
};

// We need a factory to produce such a frontend action
template <typename T>
std::unique_ptr<tooling::FrontendActionFactory> HI_FunctionInterfaceInfo_rewrite_newFrontendActionFactory(const char * _parseLog_name,
                                                            Rewriter &R,
                                                            const char * _outputCode_name,
                                                            std::map<std::string, int> &FuncParamLine2OutermostSize,
                                                            std::string topFunctioName) 
{
  class SimpleFrontendActionFactory : public tooling::FrontendActionFactory {
  public:
    SimpleFrontendActionFactory(const char * _parseLog_name,Rewriter &R,const char * _outputCode_name, 
                                std::map<std::string, int> &FuncParamLine2OutermostSize, std::string topFunctioName): 
    parseLog_name(_parseLog_name), TheRewriter(R),outputCode_name(_outputCode_name), FuncParamLine2OutermostSize(FuncParamLine2OutermostSize), topFunctioName(topFunctioName)
    {
    }
    FrontendAction *create() override 
    { 
        return new T(parseLog_name.c_str(), TheRewriter, outputCode_name.c_str(), FuncParamLine2OutermostSize, topFunctioName); 
    }
    std::string parseLog_name;
    std::string outputCode_name;
    Rewriter &TheRewriter;
    std::map<std::string, int> &FuncParamLine2OutermostSize;
    std::string topFunctioName;
  };

  return std::unique_ptr<tooling::FrontendActionFactory>(
      new SimpleFrontendActionFactory(_parseLog_name,R,_outputCode_name,FuncParamLine2OutermostSize, topFunctioName));
}




#endif