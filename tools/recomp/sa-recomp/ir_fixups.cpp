#include "ir_fixups.hpp"

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <format>

namespace ma::recomp {

namespace {

bool is_f80(const llvm::Type* t) { return t->isX86_FP80Ty(); }

bool touches_f80(const llvm::Instruction& inst) {
    if (is_f80(inst.getType())) {
        return true;
    }
    for (const auto& op : inst.operands()) {
        if (is_f80(op->getType())) {
            return true;
        }
    }
    return false;
}

bool is_supported_unary_intrinsic(llvm::Intrinsic::ID id) {
    switch (id) {
        case llvm::Intrinsic::fabs:
        case llvm::Intrinsic::floor:
        case llvm::Intrinsic::ceil:
        case llvm::Intrinsic::trunc:
        case llvm::Intrinsic::rint:
        case llvm::Intrinsic::nearbyint:
        case llvm::Intrinsic::round:
        case llvm::Intrinsic::roundeven:
        case llvm::Intrinsic::sqrt:
        case llvm::Intrinsic::sin:
        case llvm::Intrinsic::cos:
        case llvm::Intrinsic::exp:
        case llvm::Intrinsic::exp2:
        case llvm::Intrinsic::log:
        case llvm::Intrinsic::log2:
        case llvm::Intrinsic::log10:
        case llvm::Intrinsic::pow:
        case llvm::Intrinsic::copysign:
        case llvm::Intrinsic::minnum:
        case llvm::Intrinsic::maxnum:
        case llvm::Intrinsic::minimum:
        case llvm::Intrinsic::maximum:
        case llvm::Intrinsic::fma:
        case llvm::Intrinsic::fmuladd:
        case llvm::Intrinsic::is_fpclass:
            return true;
        default:
            return false;
    }
}

const llvm::StringSet<> kLongDoubleLibm = {
    "sinl",   "cosl",  "tanl",  "atanl",  "atan2l", "exp2l",      "log2l", "fmodl", "remainderl",
    "sqrtl",  "powl",  "floorl", "ceill", "truncl", "roundl",     "rintl", "nearbyintl",
    "fabsl",  "expl",  "logl",  "log10l", "ldexpl", "scalbnl",
};

llvm::Function* f80_bits_from_double(llvm::Module& m) {
    if (auto* fn = m.getFunction("ma_f80_bits_from_double")) {
        return fn;
    }
    auto& ctx = m.getContext();
    auto* i80 = llvm::Type::getIntNTy(ctx, 80);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* f64 = llvm::Type::getDoubleTy(ctx);
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(i80, {f64}, false), llvm::GlobalValue::InternalLinkage,
                                      "ma_f80_bits_from_double", m);
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "", fn));
    auto c = [&](uint64_t v) { return llvm::ConstantInt::get(i64, v); };
    llvm::Value* bits = b.CreateBitCast(fn->getArg(0), i64);
    llvm::Value* sign = b.CreateLShr(bits, 63);
    llvm::Value* exp = b.CreateAnd(b.CreateLShr(bits, 52), c(0x7ff));
    llvm::Value* frac = b.CreateAnd(bits, c(0xfffffffffffffull));
    llvm::Value* exp_zero = b.CreateICmpEQ(exp, c(0));
    llvm::Value* exp_max = b.CreateICmpEQ(exp, c(0x7ff));
    llvm::Value* frac_zero = b.CreateICmpEQ(frac, c(0));
    llvm::Value* mant_normal = b.CreateOr(b.CreateShl(frac, 11), c(0x8000000000000000ull));
    llvm::Value* exp_normal = b.CreateAdd(exp, c(16383 - 1023));
    llvm::Value* mant_special =
        b.CreateSelect(frac_zero, c(0x8000000000000000ull), b.CreateOr(b.CreateShl(frac, 11), c(0xc000000000000000ull)));
    llvm::Value* lz = b.CreateIntrinsic(llvm::Intrinsic::ctlz, {i64}, {frac, b.getFalse()});
    llvm::Value* mant_denormal = b.CreateShl(frac, lz);
    llvm::Value* exp_denormal = b.CreateSub(c(63 - 1074 + 16383), lz);
    llvm::Value* mant_low = b.CreateSelect(frac_zero, c(0), mant_denormal);
    llvm::Value* exp_low = b.CreateSelect(frac_zero, c(0), exp_denormal);
    llvm::Value* mant = b.CreateSelect(exp_max, mant_special, b.CreateSelect(exp_zero, mant_low, mant_normal));
    llvm::Value* e80 = b.CreateSelect(exp_max, c(0x7fff), b.CreateSelect(exp_zero, exp_low, exp_normal));
    llvm::Value* se = b.CreateOr(b.CreateShl(sign, 15), e80);
    llvm::Value* result = b.CreateOr(b.CreateShl(b.CreateZExt(se, i80), 64), b.CreateZExt(mant, i80));
    b.CreateRet(result);
    return fn;
}

llvm::Function* double_from_f80_bits(llvm::Module& m) {
    if (auto* fn = m.getFunction("ma_double_from_f80_bits")) {
        return fn;
    }
    auto& ctx = m.getContext();
    auto* i80 = llvm::Type::getIntNTy(ctx, 80);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* f64 = llvm::Type::getDoubleTy(ctx);
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(f64, {i80}, false), llvm::GlobalValue::InternalLinkage,
                                      "ma_double_from_f80_bits", m);
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "", fn));
    auto c = [&](uint64_t v) { return llvm::ConstantInt::get(i64, v); };
    llvm::Value* bits = fn->getArg(0);
    llvm::Value* mant = b.CreateTrunc(bits, i64);
    llvm::Value* se = b.CreateAnd(b.CreateTrunc(b.CreateLShr(bits, 64), i64), c(0xffff));
    llvm::Value* negative = b.CreateICmpNE(b.CreateAnd(se, c(0x8000)), c(0));
    llvm::Value* exp = b.CreateAnd(se, c(0x7fff));
    llvm::Value* scale = b.CreateTrunc(b.CreateSub(exp, c(16383 + 63)), i32);
    llvm::Value* normal = b.CreateIntrinsic(llvm::Intrinsic::ldexp, {f64, i32}, {b.CreateUIToFP(mant, f64), scale});
    llvm::Value* is_zero = b.CreateAnd(b.CreateICmpEQ(exp, c(0)), b.CreateICmpEQ(mant, c(0)));
    llvm::Value* is_special = b.CreateICmpEQ(exp, c(0x7fff));
    llvm::Value* is_inf = b.CreateICmpEQ(b.CreateShl(mant, 1), c(0));
    llvm::Value* special = b.CreateSelect(is_inf, llvm::ConstantFP::getInfinity(f64), llvm::ConstantFP::getQNaN(f64));
    llvm::Value* magnitude =
        b.CreateSelect(is_special, special, b.CreateSelect(is_zero, llvm::ConstantFP::get(f64, 0.0), normal));
    b.CreateRet(b.CreateSelect(negative, b.CreateFNeg(magnitude), magnitude));
    return fn;
}

class F80Lowering {
public:
    F80Lowering(llvm::Function& fn, std::vector<std::string>& errors)
        : fn_(fn), module_(*fn.getParent()), ctx_(fn.getContext()), errors_(errors) {
        f80_ = llvm::Type::getX86_FP80Ty(ctx_);
        f64_ = llvm::Type::getDoubleTy(ctx_);
    }

    void run() {
        std::vector<llvm::Instruction*> work;
        llvm::ReversePostOrderTraversal<llvm::Function*> rpo(&fn_);
        for (llvm::BasicBlock* bb : rpo) {
            for (auto& inst : *bb) {
                if (touches_f80(inst)) {
                    work.push_back(&inst);
                }
            }
        }
        if (work.empty()) {
            return;
        }
        for (auto* inst : work) {
            if (auto* phi = llvm::dyn_cast<llvm::PHINode>(inst); phi && is_f80(phi->getType())) {
                auto* np = llvm::PHINode::Create(f64_, phi->getNumIncomingValues(), phi->getName(), phi->getIterator());
                map_[phi] = np;
                phis_.push_back(phi);
            }
        }
        for (auto* inst : work) {
            if (llvm::isa<llvm::PHINode>(inst)) {
                continue;
            }
            lower(inst);
        }
        for (auto* phi : phis_) {
            auto* np = llvm::cast<llvm::PHINode>(map_[phi]);
            for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                np->addIncoming(get(phi->getIncomingValue(i)), phi->getIncomingBlock(i));
            }
            dead_.push_back(phi);
        }
        for (auto it = dead_.rbegin(); it != dead_.rend(); ++it) {
            auto* inst = *it;
            if (!inst->use_empty()) {
                inst->replaceAllUsesWith(llvm::PoisonValue::get(inst->getType()));
            }
        }
        for (auto* inst : dead_) {
            inst->eraseFromParent();
        }
    }

private:
    llvm::Value* get(llvm::Value* v) {
        if (!is_f80(v->getType())) {
            return v;
        }
        if (auto it = map_.find(v); it != map_.end()) {
            return it->second;
        }
        if (auto* c = llvm::dyn_cast<llvm::ConstantFP>(v)) {
            llvm::APFloat value = c->getValueAPF();
            bool lost = false;
            value.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven, &lost);
            return llvm::ConstantFP::get(ctx_, value);
        }
        if (llvm::isa<llvm::PoisonValue>(v)) {
            return llvm::PoisonValue::get(f64_);
        }
        if (llvm::isa<llvm::UndefValue>(v)) {
            return llvm::UndefValue::get(f64_);
        }
        fail(std::format("unmapped x86_fp80 value in {}", fn_.getName().str()));
        return llvm::PoisonValue::get(f64_);
    }

    void fail(std::string message) {
        if (errors_.size() < 50) {
            errors_.push_back(std::move(message));
        }
    }

    void replace(llvm::Instruction* inst, llvm::Value* with) {
        if (is_f80(inst->getType())) {
            map_[inst] = with;
        } else {
            inst->replaceAllUsesWith(with);
        }
        dead_.push_back(inst);
    }

    llvm::Type* lowered(llvm::Type* t) const { return is_f80(t) ? f64_ : t; }

    void lower(llvm::Instruction* inst) {
        llvm::IRBuilder<> b(inst);
        if (auto* ext = llvm::dyn_cast<llvm::FPExtInst>(inst)) {
            llvm::Value* src = ext->getOperand(0);
            replace(inst, src->getType()->isDoubleTy() ? src : b.CreateFPExt(src, f64_));
            return;
        }
        if (auto* tr = llvm::dyn_cast<llvm::FPTruncInst>(inst)) {
            llvm::Value* v = get(tr->getOperand(0));
            replace(inst, tr->getType()->isDoubleTy() ? v : b.CreateFPTrunc(v, tr->getType()));
            return;
        }
        if (auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
            auto* v = b.CreateBinOp(bin->getOpcode(), get(bin->getOperand(0)), get(bin->getOperand(1)));
            if (auto* vi = llvm::dyn_cast<llvm::Instruction>(v)) {
                vi->copyFastMathFlags(bin);
            }
            replace(inst, v);
            return;
        }
        if (auto* un = llvm::dyn_cast<llvm::UnaryOperator>(inst); un && un->getOpcode() == llvm::Instruction::FNeg) {
            replace(inst, b.CreateFNeg(get(un->getOperand(0))));
            return;
        }
        if (auto* cmp = llvm::dyn_cast<llvm::FCmpInst>(inst)) {
            replace(inst, b.CreateFCmp(cmp->getPredicate(), get(cmp->getOperand(0)), get(cmp->getOperand(1))));
            return;
        }
        if (auto* c = llvm::dyn_cast<llvm::SIToFPInst>(inst)) {
            replace(inst, b.CreateSIToFP(c->getOperand(0), f64_));
            return;
        }
        if (auto* c = llvm::dyn_cast<llvm::UIToFPInst>(inst)) {
            replace(inst, b.CreateUIToFP(c->getOperand(0), f64_));
            return;
        }
        if (auto* c = llvm::dyn_cast<llvm::FPToSIInst>(inst)) {
            replace(inst, b.CreateFPToSI(get(c->getOperand(0)), c->getType()));
            return;
        }
        if (auto* c = llvm::dyn_cast<llvm::FPToUIInst>(inst)) {
            replace(inst, b.CreateFPToUI(get(c->getOperand(0)), c->getType()));
            return;
        }
        if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst)) {
            auto* v = b.CreateAlignedLoad(f64_, ld->getPointerOperand(), llvm::Align(1), ld->isVolatile());
            replace(inst, v);
            return;
        }
        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(inst)) {
            b.CreateAlignedStore(get(st->getValueOperand()), st->getPointerOperand(), llvm::Align(1), st->isVolatile());
            dead_.push_back(inst);
            return;
        }
        if (auto* cast = llvm::dyn_cast<llvm::BitCastInst>(inst)) {
            auto* src = cast->getOperand(0);
            if (is_f80(src->getType()) && cast->getType()->isIntegerTy(80)) {
                replace(inst, b.CreateCall(f80_bits_from_double(module_), {get(src)}));
                return;
            }
            if (src->getType()->isIntegerTy(80) && is_f80(cast->getType())) {
                replace(inst, b.CreateCall(double_from_f80_bits(module_), {src}));
                return;
            }
        }
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(inst)) {
            replace(inst, b.CreateSelect(sel->getCondition(), get(sel->getTrueValue()), get(sel->getFalseValue())));
            return;
        }
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(inst)) {
            lower_call(call, b);
            return;
        }
        std::string text;
        llvm::raw_string_ostream os(text);
        inst->print(os);
        fail(std::format("unsupported x86_fp80 instruction in {}: {}", fn_.getName().str(), text));
    }

    void lower_call(llvm::CallInst* call, llvm::IRBuilder<>& b) {
        llvm::Function* callee = call->getCalledFunction();
        if (!callee) {
            fail(std::format("indirect call with x86_fp80 in {}", fn_.getName().str()));
            return;
        }
        std::vector<llvm::Value*> args;
        for (auto& arg : call->args()) {
            args.push_back(get(arg.get()));
        }
        llvm::Intrinsic::ID id = callee->getIntrinsicID();
        if (id != llvm::Intrinsic::not_intrinsic) {
            if (!is_supported_unary_intrinsic(id)) {
                fail(std::format("unsupported x86_fp80 intrinsic {} in {}", callee->getName().str(), fn_.getName().str()));
                return;
            }
            auto* decl = llvm::Intrinsic::getOrInsertDeclaration(&module_, id, {f64_});
            replace(call, b.CreateCall(decl, args));
            return;
        }
        llvm::StringRef name = callee->getName();
        if (!kLongDoubleLibm.contains(name)) {
            fail(std::format("unsupported call {} with x86_fp80 in {}", name.str(), fn_.getName().str()));
            return;
        }
        std::vector<llvm::Type*> params;
        for (auto* p : callee->getFunctionType()->params()) {
            params.push_back(lowered(p));
        }
        auto* type = llvm::FunctionType::get(lowered(callee->getReturnType()), params, false);
        auto target = module_.getOrInsertFunction(name.drop_back(1), type);
        replace(call, b.CreateCall(target, args));
    }

    llvm::Function& fn_;
    llvm::Module& module_;
    llvm::LLVMContext& ctx_;
    std::vector<std::string>& errors_;
    llvm::Type* f80_ = nullptr;
    llvm::Type* f64_ = nullptr;
    llvm::DenseMap<llvm::Value*, llvm::Value*> map_;
    std::vector<llvm::PHINode*> phis_;
    std::vector<llvm::Instruction*> dead_;
};

bool signature_uses_f80(const llvm::Function& fn) {
    if (is_f80(fn.getReturnType())) {
        return true;
    }
    for (const auto* p : fn.getFunctionType()->params()) {
        if (is_f80(p)) {
            return true;
        }
    }
    return false;
}

}

void lower_x86_fp80(llvm::Module& module, std::vector<std::string>& errors) {
    for (auto& fn : module) {
        if (fn.isDeclaration()) {
            continue;
        }
        if (signature_uses_f80(fn)) {
            errors.push_back(std::format("function {} has x86_fp80 in its signature", fn.getName().str()));
            continue;
        }
        F80Lowering(fn, errors).run();
    }
    std::vector<llvm::Function*> unused;
    for (auto& fn : module) {
        if (fn.isDeclaration() && signature_uses_f80(fn)) {
            if (fn.use_empty()) {
                unused.push_back(&fn);
            } else {
                errors.push_back(std::format("declaration {} with x86_fp80 is still used", fn.getName().str()));
            }
        }
    }
    for (auto* fn : unused) {
        fn->eraseFromParent();
    }
}

void rewrite_x86_inline_asm(llvm::Module& module, std::vector<std::string>& errors) {
    std::vector<llvm::CallInst*> calls;
    for (auto& fn : module) {
        for (auto& bb : fn) {
            for (auto& inst : bb) {
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst); call && call->isInlineAsm()) {
                    calls.push_back(call);
                }
            }
        }
    }
    for (auto* call : calls) {
        auto* asm_value = llvm::cast<llvm::InlineAsm>(call->getCalledOperand());
        if (!asm_value->getAsmString().empty() || !call->getType()->isVoidTy() || call->arg_size() != 0) {
            errors.push_back(std::format("x86 inline asm in {}: {}", call->getFunction()->getName().str(),
                                         asm_value->getAsmString().str()));
            continue;
        }
        auto* type = llvm::FunctionType::get(llvm::Type::getVoidTy(module.getContext()), false);
        auto* barrier = llvm::InlineAsm::get(type, "", "~{memory}", true);
        llvm::CallInst::Create(type, barrier, "", call->getIterator());
        call->eraseFromParent();
    }
}

void strip_x86_function_attributes(llvm::Module& module) {
    for (auto& fn : module) {
        fn.removeFnAttr("target-cpu");
        fn.removeFnAttr("target-features");
        fn.removeFnAttr("tune-cpu");
    }
}

size_t route_calls_through_dispatch(llvm::Module& module) {
    llvm::Function* dispatcher = nullptr;
    size_t routed = 0;
    for (auto& fn : module) {
        for (auto& block : fn) {
            for (auto& inst : block) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call) {
                    continue;
                }
                auto* callee = call->getCalledFunction();
                if (!callee || !callee->getName().starts_with("sub_")) {
                    continue;
                }
                auto* next = call->getNextNode();
                auto* ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(next);
                if (ret && ret->getReturnValue() == call) {
                    continue;
                }
                if (!dispatcher) {
                    dispatcher = llvm::cast<llvm::Function>(
                        module.getOrInsertFunction("__remill_function_call", callee->getFunctionType()).getCallee());
                }
                call->setCalledFunction(dispatcher);
                call->setTailCallKind(llvm::CallInst::TCK_None);
                ++routed;
            }
        }
    }
    return routed;
}

void enable_guest_unwinding(llvm::Module& module) {
    auto may_unwind = [](const llvm::Function& fn) {
        llvm::StringRef name = fn.getName();
        if (name.starts_with("llvm.")) {
            return false;
        }
        return name.starts_with("sub_") || !fn.hasLocalLinkage();
    };
    for (auto& fn : module) {
        if (!may_unwind(fn)) {
            continue;
        }
        fn.removeFnAttr(llvm::Attribute::NoUnwind);
        if (!fn.isDeclaration()) {
            fn.setUWTableKind(llvm::UWTableKind::Async);
        }
        for (auto* user : fn.users()) {
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(user)) {
                call->removeFnAttr(llvm::Attribute::NoUnwind);
            }
        }
    }
    for (auto& fn : module) {
        if (!fn.isDeclaration()) {
            fn.setUWTableKind(llvm::UWTableKind::Async);
        }
    }
}

void relax_intrinsic_memory_attributes(llvm::Module& module) {
    for (auto& fn : module) {
        llvm::StringRef name = fn.getName();
        if (!name.starts_with("__remill_") || name.starts_with("__remill_flag_computation") ||
            name.starts_with("__remill_compare_") || name.starts_with("__remill_undefined")) {
            continue;
        }
        fn.removeFnAttr(llvm::Attribute::Memory);
        fn.removeFnAttr(llvm::Attribute::ReadNone);
        fn.removeFnAttr(llvm::Attribute::ReadOnly);
        fn.removeFnAttr(llvm::Attribute::WillReturn);
        for (auto* user : fn.users()) {
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(user)) {
                call->removeFnAttr(llvm::Attribute::Memory);
                call->removeFnAttr(llvm::Attribute::ReadNone);
                call->removeFnAttr(llvm::Attribute::ReadOnly);
            }
        }
    }
}

}
