#include "waot.h"
#include "wasm.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <cassert>

// Should I just give up and do 'using namespace llvm' like everything in LLVM?
using llvm::BasicBlock;
using llvm::Function;
using llvm::FunctionType;
using llvm::IRBuilder;
using llvm::Module;
using llvm::SmallVector;
using llvm::Type;
using llvm::Value;

static Type* getLLVMType(WasmType T, llvm::LLVMContext& C) {
  switch (T) {
    case WASM_TYPE_VOID:
      return Type::getVoidTy(C);
    case WASM_TYPE_I32:
      return Type::getInt32Ty(C);
    case WASM_TYPE_I64:
      return Type::getInt64Ty(C);
    case WASM_TYPE_F32:
      return Type::getFloatTy(C);
    case WASM_TYPE_F64:
      return Type::getDoubleTy(C);
    default:
      llvm_unreachable("Unexpexted type in getLLVMType");
  }
}

std::unique_ptr<Module> WAOTVisitor::VisitModule(const wasm::Module& mod) {
  module_ = llvm::make_unique<Module>(mod.name, ctx_);
  assert(module_ && "Could not create Module");

  for (auto& func : mod.functions) {
    VisitFunction(func);
  }
  return std::move(module_);
}

void WAOTVisitor::VisitFunction(const wasm::Function& func) {
  Type* ret_type = Type::getVoidTy(ctx_);
  if (func.result_type != WASM_TYPE_VOID) {
    ret_type = getLLVMType(func.result_type, ctx_);
  }
  SmallVector<Type*, 4> arg_types;
  for (auto& arg : func.args) {
    arg_types.push_back(getLLVMType(arg.type, ctx_));
  }

  auto* f = Function::Create(FunctionType::get(ret_type, arg_types, false),
                             Function::InternalLinkage, func.local_name.c_str(),
                             module_.get());
  assert(f && "Could not create Function");

  auto arg_iterator = f->arg_begin();
  for (auto& arg : func.args) {
    if (!arg.local_name.empty())
      arg_iterator->setName(arg.local_name);
    ++arg_iterator;
  }

  BasicBlock::Create(ctx_, "entry", f);
  auto& bb = f->getEntryBlock();

  IRBuilder<> irb(&bb);
  for (auto& local : func.locals) {
    irb.CreateAlloca(getLLVMType(local.type, ctx_), nullptr,
                     local.local_name.c_str());
  }
  functions_.emplace(&func, f);
  current_func_ = f;
  current_bb_ = &bb;

  for (auto& expr : func.body) {
    VisitExpression(*expr);
  }
}

void WAOTVisitor::VisitImport(const wasm::Import& imp) {}
void WAOTVisitor::VisitSegment(const wasm::Segment& seg) {}

Value* WAOTVisitor::VisitNop() {
  return nullptr;
}
Value* WAOTVisitor::VisitBlock(const wasm::Expression::ExprVector& exprs) {
  return nullptr;
}

Value* WAOTVisitor::VisitCall(WasmOpType opcode,
                              const wasm::Callable& callee,
                              int callee_index,
                              const wasm::Expression::ExprVector& args) {
  assert(current_bb_);
  BasicBlock* bb(current_bb_);
  SmallVector<Value*, 8> arg_values;
  for (auto& arg : args) {
    arg_values.push_back(VisitExpression(*arg));
  }
  IRBuilder<> irb(bb);
  return irb.CreateCall(functions_[&callee], arg_values);
}

Value* WAOTVisitor::VisitReturn(const wasm::Expression::ExprVector& value) {
  IRBuilder<> irb(current_bb_);
  if (!value.size())
    return irb.CreateRetVoid();
  return irb.CreateRet(VisitExpression(*value.front()));
}

Value* WAOTVisitor::VisitConst(const wasm::Literal& l) {
  switch (l.type) {
    case WASM_TYPE_VOID:
      return llvm::UndefValue::get(Type::getVoidTy(ctx_));
    case WASM_TYPE_I32:
    case WASM_TYPE_I64:
      return llvm::ConstantInt::get(
          getLLVMType(l.type, ctx_),
          l.type == WASM_TYPE_I32 ? l.value.i32 : l.value.i64);
    case WASM_TYPE_F32:
    case WASM_TYPE_F64:
      return llvm::ConstantFP::get(
          getLLVMType(l.type, ctx_),
          l.type == WASM_TYPE_F32 ? l.value.f32 : l.value.f64);
    default:
      assert(false);
  }
}