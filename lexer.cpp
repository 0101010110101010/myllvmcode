#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"

#include "include/mylexer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <string>
#include <iostream>
#include <utility>

//#ifdef OPTIMIZATION
#define PRINT_ALIR


//The lexer returns tokens [0-255] if it is an unknow character, otherwise one of these for know things.
enum Token
{
	tok_eof = -1,

	// commands
	tok_def = -2,
	tok_extern = -3,

	//primary
	tok_identifier = -4,
	tok_number = -5,
};

static std::string IdentifierStr; // Filled in if tok identifier
static double NumVal;

static std::unique_ptr<llvm::LLVMContext> TheContext;
static std::unique_ptr<llvm::IRBuilder<>> Builder;
static std::unique_ptr<llvm::Module> TheModule;
static std::map<std::string, llvm::Value *> NamedValues;

static std::unique_ptr<llvm::orc::KaleidoscopeJIT> TheJIT;
static std::unique_ptr<llvm::FunctionPassManager> TheFPM;
static std::unique_ptr<llvm::LoopAnalysisManager> TheLAM;
static std::unique_ptr<llvm::FunctionAnalysisManager> TheFAM;
static std::unique_ptr<llvm::CGSCCAnalysisManager> TheCGAM;
static std::unique_ptr<llvm::ModuleAnalysisManager> TheMAM;
static std::unique_ptr<llvm::PassInstrumentationCallbacks> ThePIC;
static std::unique_ptr<llvm::StandardInstrumentations> TheSI;
//static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
static llvm::ExitOnError ExitOnErr;

//gettok - Return the next token from standard input.
static int gettok()
{
	static int LastChar = ' ';

	//Skip any whitespace.
	while(isspace(LastChar))
		LastChar = getchar();

	if(isalpha(LastChar)) //identifier: [a-zA-Z][a-zA-Z0-9]*
	{
		IdentifierStr = LastChar;
		while(isalnum(LastChar = getchar()))
			IdentifierStr += LastChar;

		if(IdentifierStr == "def")
			return tok_def;
		if(IdentifierStr == "extern")
			return tok_extern;
		return tok_identifier;
	}

	if(isdigit(LastChar) || LastChar == '.') // Number:[0-9.]+
	{
		std::string NumStr;
		do
		{
			NumStr += LastChar;
			LastChar = getchar();
		}while(isdigit(LastChar) || LastChar == '.');

		NumVal = strtod(NumStr.c_str(),0);
		return tok_number;
	}

	if(LastChar == '#')
	{
		//Comment unilt end of line.
		do
		{
			LastChar = getchar();
		}while(LastChar != EOF && LastChar != '\n' && LastChar != '\r');

		if(LastChar != EOF)
			return gettok();
	}

	//check for end of file. Don't eat the EOF.
	if(LastChar == EOF)
		return tok_eof;

	//Otherwise, just return the character as its ascii value.
	int ThisChar = LastChar;
	LastChar = getchar();
	return ThisChar;
}

///ExprAST - Base class for all expression nodes.
class ExprAST
{
	public:
		virtual ~ExprAST() = default;
		virtual llvm::Value *codegen() = 0;
};

//NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST
{
	double Val;
	public:
	NumberExprAST(double V) : Val(V){}
	llvm::Value *codegen() override;
};

llvm::Value * NumberExprAST::codegen()
{
	return llvm::ConstantFP::get(*TheContext, llvm::APFloat(Val));
}

///VariableExprAST - Expression class for referencing a variable, like "a"
class VariableExprAST : public ExprAST
{
	std::string Name;

	public:
	VariableExprAST(const std::string &N) : Name(N){}
	llvm::Value *codegen() override;
};

///BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST
{
	char Op;
	std::unique_ptr<ExprAST> LHS, RHS;

	public:
	BinaryExprAST(
							char Op, 
							std::unique_ptr<ExprAST> LHS, 
							std::unique_ptr<ExprAST> RHS) :
		Op(Op) , LHS(std::move(LHS)), RHS(std::move(RHS)) {}
	llvm::Value *codegen() override;
};

///CallExprAst - Expression class for function calls.
class CallExprAst : public ExprAST
{
	std::string Callee;
	std::vector<std::unique_ptr<ExprAST>> Args;

	public:
	CallExprAst(
							const std::string &Callee, 
							std::vector<std::unique_ptr<ExprAST>> Args) :
		Callee(Callee), Args(std::move(Args)){}
	llvm::Value *codegen() override;
};
///PrototypeAST - This class represents the "prototype" for a function,
///which captures its name, and its argument names (thus implicitly the number of arguments the function takes.)
class PrototypeAST
{
	std::string Name;
	std::vector<std::string> Args;

	public:
	PrototypeAST(const std::string &Name, std::vector<std::string> Args):
		Name(Name), Args(std::move(Args)){}

	const std::string &getName() const {
		return Name;
	}
	llvm::Function *codegen();
};

/// FunctionAst - This class represents a functions a function definition ifself.
class FunctionAST
{
	std::unique_ptr<PrototypeAST> Proto;
	std::unique_ptr<ExprAST> Body;

	public:
	FunctionAST(std::unique_ptr<PrototypeAST> Proto,
							std::unique_ptr<ExprAST> Body):
		Proto(std::move(Proto)), Body(std::move(Body))
	{ }
	llvm::Function *codegen();
};

static std::unique_ptr<ExprAST> ParsePrimary();
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS);
static std::unique_ptr<ExprAST> ParseParenExpr();

/// CurTok/getNextToken - Provide a simple token buffer. CurTok is the current
/// token the parser is looking at. getNextToken reads another token from the lexer and updates CurTok with its results.
static int CurTok;
static int getNextToken()
{
	return CurTok = gettok();
}

///LogError * - These are little helper functions for error handing.
std::unique_ptr<ExprAST> LogError(const char * Str)
{
	fprintf(stderr, "Error:%s\n", Str);
	return nullptr;
}
std::unique_ptr<PrototypeAST> LogErrorP(const char * Str)
{
	LogError(Str);
	return nullptr;
}

llvm::Value *LogErrorV(const char * Str)
{
	LogError(Str);
	return nullptr;
}

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr()
{
	auto Result = std::make_unique<NumberExprAST>(NumVal);
	getNextToken();
	return std::move(Result);
}

/// expression
/// ::= primary binoprhs
static std::unique_ptr<ExprAST> ParseExpression()
{
	auto LHS = ParsePrimary();
	if(!LHS)
		return nullptr;
	return ParseBinOpRHS(0, std::move(LHS));
}

/// identifierexpr
/// ::= identifier
/// ::= identifier '(' expression ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr()
{
	std::string IdName = IdentifierStr;
	getNextToken(); // eat identifier
									
	if(CurTok != '(')
		return std::make_unique<VariableExprAST>(IdName);

	//Call
	getNextToken(); //eat (
	std::vector<std::unique_ptr<ExprAST>> Args;
	if(CurTok != ')')
	{
		while(true)
		{
			if(auto Arg = ParseExpression())
				Args.push_back(std::move(Arg));
			else
				return nullptr;

			if(CurTok == ')')
				break;
			if(CurTok !=',')
				return LogError("Expect ')' or ',' in argument list");

			getNextToken();
		}
	}

	//Eat the ')'
	getNextToken();

	return std::make_unique<CallExprAst>(IdName, std::move(Args));
}

/// primary
/// ::= identifierexpr
/// ::= numberexpr
/// ::= parenexpr
static std::unique_ptr<ExprAST> ParsePrimary()
{
	switch(CurTok)
	{
		default:
			return LogError("unknow token when expecting an expression");
		case tok_identifier:
			return ParseIdentifierExpr();
		case tok_number:
			return ParseNumberExpr();
		case '(':
			return ParseParenExpr();
	}
}

/// pareexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr()
{
	getNextToken(); // eat (
	auto V = ParseExpression();

	if(!V)
		return nullptr;

	if(CurTok != ')')
		return LogError("expected ')'");
	getNextToken();

	return V;
}



/// BinopPrecedence - This holds the precedence for each binary operator that is defined.
static std::map<char, int> BinopPrecedence;
/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static int GetTokPrecedence()
{
	if(!isascii(CurTok))
		return -1;

	//Make sure it's a declared binop.
	int TokPrec = BinopPrecedence[CurTok];
	if(TokPrec <= 0)
		return -1;
	return TokPrec;
}

///binoprhs
// ::= ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
																							std::unique_ptr<ExprAST> LHS)
{
	//if this is a binop, find its precedence
	while(true)
	{
		int TokPrec = GetTokPrecedence();

		if(TokPrec < ExprPrec)
			return LHS;
		
		// Okay, we know this is a binop.
		int BinOp = CurTok;
		getNextToken(); //eat binop

		//Parse the primary expression after the binary operator.
		auto RHS = ParsePrimary();
		if(!RHS)
			return nullptr;

		//if Binop binds less tightly with RHS than the opetator after RHS, let
		//the pending operator take RHS as its LHS.
		int NextPrec = GetTokPrecedence();
		if(TokPrec < NextPrec)
		{
			RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
			if(!RHS)
				return nullptr;
		}

		//Merge LHS/RHS.
		LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
	}//loop around to the top of the while loop.
}

/// Prototype
//::= id '(' id* ')'
static std::unique_ptr<PrototypeAST> ParsePrototype()
{
	if(CurTok != tok_identifier)
		return LogErrorP("Expected function name in prototype");

	std::string FnName = IdentifierStr;
	getNextToken();

	if(CurTok != '(')
		return LogErrorP("Expected '(' in prototype");

	//Read the list of argument names.
	std::vector<std::string> ArgNames;
	while(getNextToken() == tok_identifier)
		ArgNames.push_back(IdentifierStr);
	if(CurTok != ')')
		return LogErrorP("Expected ')' in prototype");

	//sucesss.
	getNextToken(); //eat )
	
	return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

//define ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition()
{
	getNextToken(); //eat def
	auto Proto = ParsePrototype();
	if(!Proto)
		return nullptr;

	if(auto E = ParseExpression())
		return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));

	return nullptr;
}

static std::unique_ptr<PrototypeAST> ParseExtern()
{
	getNextToken(); //eat extern
	return ParsePrototype();
}

//toplevelexpr ::= expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr()
{
	if(auto E = ParseExpression())
	{
		//Make an anonymous proto
		auto Proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
		return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
	}
	return nullptr;
}

static void HandleDefinition() {
  if (auto AST = ParseDefinition()) {
    fprintf(stderr, "Parsed a function definition.\n");
		auto *IR = AST->codegen();
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleExtern() {
  if (auto AST = ParseExtern()) {
    fprintf(stderr, "Parsed an extern\n");
		auto *IR = AST->codegen();
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void InitializeModule() {
  // Open a new context and module.
  TheContext = std::make_unique<llvm::LLVMContext>();
  TheModule = std::make_unique<llvm::Module>("my cool jit", *TheContext);
	TheModule->setDataLayout(TheJIT->getDataLayout());

  // Create a new builder for the module.
  Builder = std::make_unique<llvm::IRBuilder<>>(*TheContext);

  // Create new pass and analysis managers.
  TheFPM = std::make_unique<llvm::FunctionPassManager>();
  TheLAM = std::make_unique<llvm::LoopAnalysisManager>();
  TheFAM = std::make_unique<llvm::FunctionAnalysisManager>();
  TheCGAM = std::make_unique<llvm::CGSCCAnalysisManager>();
  TheMAM = std::make_unique<llvm::ModuleAnalysisManager>();
  ThePIC = std::make_unique<llvm::PassInstrumentationCallbacks>();
  TheSI = std::make_unique<llvm::StandardInstrumentations>(
                                                    /*DebugLogging*/ true);
  TheSI->registerCallbacks(*ThePIC, TheFAM.get());

  // Add transform passes.
  // Do simple "peephole" optimizations and bit-twiddling optzns.
  TheFPM->addPass(llvm::InstCombinePass());
  // Reassociate expressions.
  TheFPM->addPass(llvm::ReassociatePass());
  // Eliminate Common SubExpressions.
  TheFPM->addPass(llvm::GVNPass());
  // Simplify the control flow graph (deleting unreachable blocks, etc).
  TheFPM->addPass(llvm::SimplifyCFGPass());

  // Register analysis passes used in these transform passes.
  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(*TheMAM);
  PB.registerFunctionAnalyses(*TheFAM);
  PB.crossRegisterProxies(*TheLAM, *TheFAM, *TheCGAM, *TheMAM);
}

static void HandleTopLevelExpression() {
	auto AST = ParseTopLevelExpr();
  // Evaluate a top-level expression into an anonymous function.
  if (AST) {
    fprintf(stderr, "Parsed a top-level expr\n");
		auto *IR = AST->codegen();
			#ifdef PRINT_ALIR
      IR->print(llvm::errs());
      fprintf(stderr,"\n");
      // Remove the anonymous expression.
      IR->eraseFromParent();

			#else
      IR->print(llvm::errs());
      fprintf(stderr,"\n");

      // Create a ResourceTracker to track JIT'd memory allocated to our
      // anonymous expression -- that way we can free it after executing.
      auto RT = TheJIT->getMainJITDylib().createResourceTracker();

      auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), std::move(TheContext));
      ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
      InitializeModule();

      // Search the JIT for the __anon_expr symbol.
      auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));
      assert(ExprSymbol && "Function not found");

      // Get the symbol's address and cast it to the right type (takes no
      // arguments, returns a double) so we can call it as a native function.
			#if 0
      double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
			#else
			llvm::JITTargetAddress funcAddr = ExprSymbol.getAddress(); // 从 JIT 获取的函数地址
			double (*FP)() = reinterpret_cast<double (*)()>(static_cast<unsigned long>(funcAddr));
			#endif

      fprintf(stderr, "Evaluated to %f\n", FP());

      // Delete the anonymous expression module from the JIT.
      ExitOnErr(RT->remove());
			#endif
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

/// top ::= definition | external | expression | ';'
static void Mainloop()
{
	while(true)
	{
		fprintf(stderr, "ready> ");
		switch(CurTok)
		{
			case tok_eof:
				TheModule->print(llvm::errs(), nullptr);
				return;
			case ';': //ignore top-level semicolons.
				getNextToken();
				break;
			case tok_def: //ignore top-level semicolons.
				HandleDefinition();
				break;
			case tok_extern:
				HandleExtern();
				break;
			default:
				HandleTopLevelExpression();
				break;
		}
	}
}

llvm::Value *VariableExprAST::codegen()
{
	llvm::Value * V = NamedValues[Name];
	if(!V)
		LogErrorV("Unknow variable name");
	return V;
}

llvm::Value *BinaryExprAST::codegen()
{
	llvm::Value *L = LHS->codegen();
	llvm::Value *R = RHS->codegen();
	if(!L || !R)
		return nullptr;

	switch(Op)
	{
		case '+':
			return Builder->CreateFAdd(L, R, "addtmp");
		case '-':
			return Builder->CreateFSub(L, R, "subtmp");
		case '*':
			return Builder->CreateFMul(L, R, "multmp");
		case '<':
			L = Builder->CreateFCmpULT(L, R, "cmptmp");
			//Convert bool 0/1 to double 0.0 or 1.0
			return Builder->CreateUIToFP(L, llvm::Type::getDoubleTy(*TheContext), "booltmp");
		default:
			return LogErrorV("invalid binary operator");
	}
}

llvm::Value * CallExprAst::codegen()
{
	//Look up the name in the golbal module table.
	llvm::Function *CalleeF = TheModule->getFunction(Callee);
	if(!CalleeF)
		return LogErrorV("Unknow function referenced");

	//If argument mismatch error.
	if(CalleeF->arg_size() != Args.size())
		return LogErrorV("Incorrect #arguments passed");

	std::vector<llvm::Value *> ArgsV;
	for(unsigned i = 0, e = Args.size(); i !=e; ++i)
	{
		ArgsV.push_back(Args[i]->codegen());
		if(!ArgsV.back())
			return nullptr;
	}

	return Builder->CreateCall(CalleeF, ArgsV, "Calltmp");
}

llvm::Function *PrototypeAST::codegen()
{
	//Make the function type: double(double,double) etc.
	std::vector<llvm::Type*> Doubles(Args.size(), llvm::Type::getDoubleTy(*TheContext));

	llvm::FunctionType *FT = llvm::FunctionType::get(llvm::Type::getDoubleTy(*TheContext), Doubles, false);

	llvm::Function *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, TheModule.get());

	//Set names for all arguments.
	unsigned Idx = 0;
	for(auto &Arg : F->args())
		Arg.setName(Args[Idx++]);

	return F;
}

llvm::Function *FunctionAST::codegen()
{
	//First, check for an existing function from a previous 'extern' declaration.
	llvm::Function * TheFunction = TheModule->getFunction(Proto->getName());
	
	if(!TheFunction)
		TheFunction = Proto->codegen();

	if(!TheFunction)
		return nullptr;

	if(!TheFunction->empty())
		return (llvm::Function *)LogErrorV("Function cannot be redefined.");

	//Create a new basic block to start insertion into.
	llvm::BasicBlock *BB = llvm::BasicBlock::Create(*TheContext, "entry", TheFunction);
	Builder->SetInsertPoint(BB);

	//Record the function arguments in the NamedValues map.
	NamedValues.clear();
	for(auto &Arg : TheFunction->args())
		NamedValues[std::string(Arg.getName())] = &Arg;

	if(llvm::Value *RetVal = Body->codegen())
	{
		//Finish off the function.
		Builder->CreateRet(RetVal);

		//Validate the generated code, checking for consistency.
		verifyFunction(*TheFunction);

		// Optimize the function.
		#ifdef OPTIMIZATION
		TheFPM->run(*TheFunction, *TheFAM);
		#endif

		return TheFunction;
	}

	TheFunction->eraseFromParent();
	return nullptr;
}


int main()
{
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

	// Install standard binary operators.
	// 1 is lowest precedence.
	BinopPrecedence['<'] = 10;
	BinopPrecedence['>'] = 10;
	BinopPrecedence['+'] = 20;
	BinopPrecedence['-'] = 20;
	BinopPrecedence['/'] = 40;
	BinopPrecedence['*'] = 40; //highest
	
	TheJIT = ExitOnErr(llvm::orc::KaleidoscopeJIT::Create());

	InitializeModule();

	fprintf(stderr, "ready> ");
	getNextToken();

	Mainloop();

	return 0;
}
