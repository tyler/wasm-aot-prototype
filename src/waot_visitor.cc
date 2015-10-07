#include "waot_visitor.h"
#include "wasm.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <cassert>

// Should I just give up and do 'using namespace llvm' like everything in LLVM?
using llvm::BasicBlock;
using llvm::BranchInst;
using llvm::Constant;
using llvm::ConstantInt;
using llvm::ConstantFP;
using llvm::Function;
using llvm::FunctionType;
using llvm::IRBuilder;
using llvm::isa;
using llvm::Module;
using llvm::ReturnInst;
using llvm::SmallVector;
using llvm::TerminatorInst;
using llvm::Type;
using llvm::Value;

Type* WAOTVisitor::getLLVMType(wasm::Type T) {
  switch (T) {
    case wasm::Type::kVoid:
      return Type::getVoidTy(ctx_);
    case wasm::Type::kI32:
      return Type::getInt32Ty(ctx_);
    case wasm::Type::kI64:
      return Type::getInt64Ty(ctx_);
    case wasm::Type::kF32:
      return Type::getFloatTy(ctx_);
    case wasm::Type::kF64:
      return Type::getDoubleTy(ctx_);
    default:
      llvm_unreachable("Unexpexted type in getLLVMType");
  }
}

static const char* TypeName(wasm::Type t) {
  switch (t) {
    case wasm::Type::kVoid:
      return "void";
    case wasm::Type::kI32:
      return "i32";
    case wasm::Type::kI64:
      return "i64";
    case wasm::Type::kF32:
      return "f32";
    case wasm::Type::kF64:
      return "f64";
    default:
      return "(unknown type)";
  }
}

static std::string Mangle(const std::string& module,
                          const std::string& function) {
  return std::string("." + module + "." + function);
}

// RAII class to set current_bb_ and restore it on return.
class BBStacker {
 public:
  BBStacker(BasicBlock** current_bb_ptr, BasicBlock* new_value) {
    current_bb_ = current_bb_ptr;
    last_value_ = *current_bb_ptr;
    *current_bb_ptr = new_value;
  }
  ~BBStacker() { *current_bb_ = last_value_; }

 private:
  BasicBlock** current_bb_;
  BasicBlock* last_value_;
};

Module* WAOTVisitor::VisitModule(const wasm::Module& mod) {
  assert(module_);
  for (auto& imp : mod.imports)
    VisitImport(*imp);
  for (auto& func : mod.functions)
    VisitFunction(*func);
  for (auto& exp : mod.exports)
    VisitExport(*exp);
  return module_;
}

Function* WAOTVisitor::GetFunction(const wasm::Callable& func,
                                   Function::LinkageTypes linkage) {
  Type* ret_type = Type::getVoidTy(ctx_);
  if (func.result_type != wasm::Type::kVoid) {
    ret_type = getLLVMType(func.result_type);
  }
  SmallVector<Type*, 4> arg_types;
  for (auto& arg : func.args) {
    arg_types.push_back(getLLVMType(arg->type));
  }

  auto* f = Function::Create(FunctionType::get(ret_type, arg_types, false),
                             linkage, func.local_name.c_str(), module_);
  assert(f && "Could not create Function");

  auto arg_iterator = f->arg_begin();
  for (auto& arg : func.args) {
    if (!arg->local_name.empty())
      arg_iterator->setName(arg->local_name);
    ++arg_iterator;
  }
  functions_.emplace(&func, f);
  return f;
}

void WAOTVisitor::VisitFunction(const wasm::Function& func) {
  auto* f = GetFunction(func, Function::InternalLinkage);
  current_func_ = f;

  BasicBlock::Create(ctx_, "entry", f);
  auto* bb = &f->getEntryBlock();
  assert(current_bb_ == nullptr);
  BBStacker bbs(&current_bb_, bb);

  IRBuilder<> irb(bb);

  unsigned i = 0;
  for (auto& local : func.locals) {
    current_locals_.push_back(
        irb.CreateAlloca(getLLVMType(local->type), nullptr));
    if (!local->local_name.empty()) {
      current_locals_.back()->setName(local->local_name.c_str());
    } else if (i < func.args.size()) {
      current_locals_.back()->setName("arg");
    } else {
      current_locals_.back()->setName("local");
    }
    ++i;
  }
  i = 0;
  for (auto& arg : f->args()) {
    irb.CreateStore(&arg, current_locals_[i++]);
  }

  Value* last_value = nullptr;
  for (auto& expr : func.body) {
    last_value = VisitExpression(expr.get());
  }
  // Handle implicit return of the last expression
  if (!current_bb_->getTerminator()) {
    IRBuilder<> irb_end(current_bb_);
    if (func.result_type == wasm::Type::kVoid) {
      irb_end.CreateRetVoid();
    } else {
      assert(func.body.size());
      last_value->dump();
      irb_end.CreateRet(last_value);
    }
  }
  current_func_ = nullptr;
  current_locals_.clear();
}

void WAOTVisitor::VisitImport(const wasm::Import& imp) {
  auto* f = GetFunction(imp, Function::ExternalLinkage);
  f->setName(Mangle(imp.module_name, imp.func_name));
}

void WAOTVisitor::VisitExport(const wasm::Export& exp) {
  llvm::GlobalAlias::create(
      functions_[exp.function]->getType(), Function::ExternalLinkage,
      Mangle(exp.module->name, exp.name), functions_[exp.function], module_);
}

void WAOTVisitor::VisitSegment(const wasm::Segment& seg) {}

Value* WAOTVisitor::VisitNop(wasm::Expression* expr) {
  return nullptr;
}
Value* WAOTVisitor::VisitBlock(wasm::Expression* expr,
                               wasm::UniquePtrVector<wasm::Expression>* exprs) {
  Value* ret = nullptr;  // A void expr instead?
  for (auto& expr : *exprs) {
    ret = VisitExpression(expr.get());
  }
  return ret;
}

static Value* CreateEqualityCompare(IRBuilder<>* irb,
                                    Value* lhs,
                                    Value* rhs,
                                    bool is_eq) {
  Value* cmp_result;
  if (lhs->getType()->isIntOrIntVectorTy()) {
    llvm::CmpInst::Predicate p =
        is_eq ? llvm::CmpInst::ICMP_EQ : llvm::CmpInst::ICMP_NE;
    cmp_result = irb->CreateICmp(p, lhs, rhs);
  } else if (lhs->getType()->isFloatTy() || lhs->getType()->isDoubleTy()) {
    llvm::CmpInst::Predicate p =
        is_eq ? llvm::CmpInst::FCMP_OEQ : llvm::CmpInst::FCMP_ONE;
    cmp_result = irb->CreateFCmp(p, lhs, rhs);
  } else {
    assert(false);
  }
  return cmp_result;
}

Value* WAOTVisitor::VisitIf(wasm::Expression* expr,
                            wasm::Expression* condition,
                            wasm::Expression* then,
                            wasm::Expression* els) {
  IRBuilder<> irb(current_bb_);
  // TODO: convert to i32
  Value* cmp_result =
      CreateEqualityCompare(&irb, VisitExpression(condition),
                            ConstantInt::get(Type::getInt32Ty(ctx_), 0), false);
  cmp_result->setName("if_cmp");

  auto* then_bb = BasicBlock::Create(ctx_, "if.then", current_func_);
  auto* else_bb = BasicBlock::Create(ctx_, "if.else", current_func_);
  auto* end_bb = BasicBlock::Create(ctx_, "if.end", current_func_);
  irb.CreateCondBr(cmp_result, then_bb, else_bb);

  current_bb_ = then_bb;
  Value* then_expr = VisitExpression(then);
  if (then_expr && isa<TerminatorInst>(then_expr)) {
    assert(isa<ReturnInst>(then_expr));
  } else {
    BranchInst::Create(end_bb, current_bb_);
  }
  then_bb = current_bb_;

  Value* else_expr = nullptr;
  current_bb_ = else_bb;
  if (els) {
    else_expr = VisitExpression(els);
    if (else_expr && isa<TerminatorInst>(else_expr)) {
      assert(isa<ReturnInst>(else_expr));
    } else {
      BranchInst::Create(end_bb, current_bb_);
    }
  } else {
    BranchInst::Create(end_bb, current_bb_);
  }
  else_bb = current_bb_;

  Value* ret = nullptr;
  if (expr->expected_type != wasm::Type::kVoid) {
    Type* expr_type = getLLVMType(expr->expected_type);
    IRBuilder<> end_irb(end_bb);
    if (!isa<TerminatorInst>(then_expr) ||
        (else_expr && !isa<TerminatorInst>(else_expr))) {
      auto* phi = end_irb.CreatePHI(expr_type, 2, "if.result");
      if (then_expr && !isa<TerminatorInst>(then_expr)) {
        phi->addIncoming(then_expr, then_bb);
      } else {
        phi->addIncoming(llvm::UndefValue::get(expr_type), then_bb);
      }
      if (else_expr && !isa<TerminatorInst>(else_expr)) {
        phi->addIncoming(else_expr, else_bb);
      } else {
        phi->addIncoming(llvm::UndefValue::get(expr_type), else_bb);
      }
      ret = phi;
    } else {
      end_irb.CreateUnreachable();
    }
  }
  current_bb_ = end_bb;

  return ret;
}

Value* WAOTVisitor::VisitCall(wasm::Expression* expr,
                              bool is_import,
                              wasm::Callable* callee,
                              int callee_index,
                              wasm::UniquePtrVector<wasm::Expression>* args) {
  assert(current_bb_);
  SmallVector<Value*, 8> arg_values;
  for (auto& arg : *args) {
    arg_values.push_back(VisitExpression(arg.get()));
  }
  IRBuilder<> irb(current_bb_);
  return irb.CreateCall(functions_[callee], arg_values);
}

Value* WAOTVisitor::VisitReturn(
    wasm::Expression* expr,
    wasm::UniquePtrVector<wasm::Expression>* value) {
  Value* retval = nullptr;
  if (value->size())
    retval = VisitExpression(value->front().get());
  return llvm::ReturnInst::Create(ctx_, retval, current_bb_);
}

Value* WAOTVisitor::VisitGetLocal(wasm::Expression* expr, wasm::Variable* var) {
  IRBuilder<> irb(current_bb_);
  auto* load_addr = current_locals_[var->index];
  return irb.CreateLoad(getLLVMType(var->type), load_addr, "get_local");
}

Value* WAOTVisitor::VisitSetLocal(wasm::Expression* expr,
                                  wasm::Variable* var,
                                  wasm::Expression* value) {
  Value* store_addr = current_locals_[var->index];
  IRBuilder<> irb(current_bb_);
  auto* store_value = VisitExpression(value);
  return irb.CreateStore(store_value, store_addr);
}

Value* WAOTVisitor::VisitConst(wasm::Expression* expr, wasm::Literal* l) {
  switch (l->type) {
    case wasm::Type::kVoid:
      return llvm::UndefValue::get(Type::getVoidTy(ctx_));
    case wasm::Type::kI32:
    case wasm::Type::kI64:
      return ConstantInt::get(getLLVMType(l->type), l->type == wasm::Type::kI32
                                                        ? l->value.i32
                                                        : l->value.i64);
    case wasm::Type::kF32:
    case wasm::Type::kF64:
      return ConstantFP::get(getLLVMType(l->type), l->type == wasm::Type::kF32
                                                       ? l->value.f32
                                                       : l->value.f64);
    default:
      assert(false);
  }
}

Value* WAOTVisitor::VisitInvoke(wasm::TestScriptExpr* expr,
                                wasm::Export* callee,
                                wasm::UniquePtrVector<wasm::Expression>* args) {
  auto* ret_type = getLLVMType(callee->function->result_type);
  auto* f = Function::Create(
      FunctionType::get(ret_type, SmallVector<Type*, 1>(), false),
      Function::ExternalLinkage, "Invoke", module_);
  assert(f);
  BasicBlock::Create(ctx_, "entry", f);
  auto* bb = &f->getEntryBlock();
  BBStacker bbs(&current_bb_, bb);

  current_func_ = f;
  Value* call = VisitCall(nullptr, false, callee->function,
                          callee->function->index_in_module, args);

  IRBuilder<> irb(bb);
  if (ret_type->isVoidTy()) {
    irb.CreateRetVoid();
  } else {
    irb.CreateRet(call);
  }
  return f;
}

Constant* WAOTVisitor::getAssertFailFunc(wasm::Type ty) {
  SmallVector<Type*, 1> params;
  params.push_back(Type::getInt32Ty(ctx_));
  params.push_back(getLLVMType(ty));
  params.push_back(getLLVMType(ty));
  return module_->getOrInsertFunction(
      std::string("__assert_fail_") + TypeName(ty),
      FunctionType::get(Type::getVoidTy(ctx_), params, false));
}

Value* WAOTVisitor::VisitAssertEq(wasm::TestScriptExpr* expr,
                                  wasm::TestScriptExpr* invoke,
                                  wasm::Expression* expected) {
  auto* f = Function::Create(
      FunctionType::get(Type::getVoidTy(ctx_), SmallVector<Type*, 1>(), false),
      Function::ExternalLinkage, "AssertEq", module_);
  BasicBlock::Create(ctx_, "entry", f);
  auto* bb = &f->getEntryBlock();
  current_func_ = f;
  BBStacker bbs(&current_bb_, bb);
  Value* invoke_func = VisitInvoke(invoke, invoke->callee, &invoke->exprs);

  IRBuilder<> irb(bb);
  Value* result = irb.CreateCall(invoke_func, SmallVector<Value*, 1>());
  Value* expected_result = VisitExpression(expected);

  assert(result->getType() == expected_result->getType());
  Value* cmp_result =
      CreateEqualityCompare(&irb, result, expected_result, true);

  BasicBlock* success_bb = BasicBlock::Create(ctx_, "AssertSuccess", f);
  llvm::ReturnInst::Create(ctx_, nullptr, success_bb);

  BasicBlock* fail_bb = BasicBlock::Create(ctx_, "AssertFail", f);
  IRBuilder<> fail_irb(fail_bb);
  // Call a runtime function, passing it the current assert_eq, the type, and
  // the expected and actual values.
  SmallVector<Value*, 1> args;
  args.push_back(
      ConstantInt::get(Type::getInt32Ty(ctx_), ++current_assert_eq_));
  args.push_back(expected_result);
  args.push_back(result);
  fail_irb.CreateCall(getAssertFailFunc(expected->expr_type), args);

  fail_irb.CreateRetVoid();
  irb.CreateCondBr(cmp_result, success_bb, fail_bb);

  return f;
}
