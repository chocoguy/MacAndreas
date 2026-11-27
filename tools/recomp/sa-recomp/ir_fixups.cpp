#include "ir_fixups.hpp"

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <cmath>
#include <format>
#include <optional>

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

constexpr std::pair<llvm::Instruction::BinaryOps, const char*> kExactOps[] = {
    {llvm::Instruction::FAdd, "ma_x87_exact_fadd"},
    {llvm::Instruction::FSub, "ma_x87_exact_fsub"},
    {llvm::Instruction::FMul, "ma_x87_exact_fmul"},
    {llvm::Instruction::FDiv, "ma_x87_exact_fdiv"},
};

std::optional<llvm::Instruction::BinaryOps> exact_opcode(llvm::StringRef name) {
    for (auto [op, n] : kExactOps) {
        if (name == n) {
            return op;
        }
    }
    return std::nullopt;
}

std::optional<const char*> exact_name(unsigned opcode) {
    for (auto [op, n] : kExactOps) {
        if (op == opcode) {
            return n;
        }
    }
    return std::nullopt;
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

    llvm::Value* state_pointer() const {
        if (fn_.arg_size() < 1 || !fn_.getArg(0)->getType()->isPointerTy()) {
            return nullptr;
        }
        return fn_.getArg(0);
    }

    llvm::Value* x86_fptosi(llvm::IRBuilder<>& b, llvm::Value* v, llvm::Type* type) {
        unsigned bits = type->getIntegerBitWidth();
        double limit = std::ldexp(1.0, static_cast<int>(bits) - 1);
        llvm::Value* low = b.CreateFCmpOGE(v, llvm::ConstantFP::get(f64_, -limit));
        llvm::Value* high = b.CreateFCmpOLT(v, llvm::ConstantFP::get(f64_, limit));
        llvm::Value* indefinite = llvm::ConstantInt::get(type, llvm::APInt::getSignedMinValue(bits));
        return b.CreateSelect(b.CreateAnd(low, high), b.CreateFPToSI(v, type), indefinite);
    }

    llvm::Value* apply_precision(llvm::IRBuilder<>& b, llvm::Value* v) {
        llvm::Value* state = state_pointer();
        if (!state) {
            return v;
        }
        auto* i16 = llvm::Type::getInt16Ty(ctx_);
        llvm::Value* slot = b.CreateConstInBoundsGEP1_32(llvm::Type::getInt8Ty(ctx_), state, kX87ControlWordOffset);
        llvm::Value* cw = b.CreateAlignedLoad(i16, slot, llvm::Align(2));
        llvm::Value* single = b.CreateICmpEQ(b.CreateAnd(cw, llvm::ConstantInt::get(i16, 0x300)),
                                             llvm::ConstantInt::get(i16, 0));
        llvm::Value* rounded = b.CreateFPExt(b.CreateFPTrunc(v, llvm::Type::getFloatTy(ctx_)), f64_);
        return b.CreateSelect(single, rounded, v);
    }

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
            llvm::Value* v = b.CreateBinOp(bin->getOpcode(), get(bin->getOperand(0)), get(bin->getOperand(1)));
            if (auto* vi = llvm::dyn_cast<llvm::Instruction>(v)) {
                vi->copyFastMathFlags(bin);
            }
            switch (bin->getOpcode()) {
                case llvm::Instruction::FAdd:
                case llvm::Instruction::FSub:
                case llvm::Instruction::FMul:
                case llvm::Instruction::FDiv:
                    v = apply_precision(b, v);
                    break;
                default:
                    break;
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
            replace(inst, x86_fptosi(b, get(c->getOperand(0)), c->getType()));
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
        if (auto op = exact_opcode(callee->getName())) {
            replace(call, b.CreateBinOp(*op, args[0], args[1]));
            return;
        }
        llvm::Intrinsic::ID id = callee->getIntrinsicID();
        if (id != llvm::Intrinsic::not_intrinsic) {
            if (!is_supported_unary_intrinsic(id)) {
                fail(std::format("unsupported x86_fp80 intrinsic {} in {}", callee->getName().str(), fn_.getName().str()));
                return;
            }
            auto* decl = llvm::Intrinsic::getOrInsertDeclaration(&module_, id, {f64_});
            llvm::Value* v = b.CreateCall(decl, args);
            if (id == llvm::Intrinsic::sqrt) {
                v = apply_precision(b, v);
            }
            replace(call, v);
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
        llvm::Value* v = b.CreateCall(target, args);
        if (name == "sqrtl") {
            v = apply_precision(b, v);
        }
        replace(call, v);
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

namespace {

llvm::Function* find_semantic(llvm::Module& m, llvm::StringRef fragment) {
    for (auto& fn : m) {
        if (!fn.isDeclaration() && fn.getName().contains(fragment)) {
            return &fn;
        }
    }
    return nullptr;
}

bool stores_control_word(const llvm::Function& fn) {
    for (const auto& bb : fn) {
        for (const auto& inst : bb) {
            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst); gep && gep->getNumIndices() == 1) {
                if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1));
                    c && c->getZExtValue() == kX87ControlWordOffset) {
                    return true;
                }
            }
        }
    }
    return false;
}

void clear_body(llvm::Function& fn) {
    fn.dropAllReferences();
    while (!fn.empty()) {
        fn.begin()->eraseFromParent();
    }
    for (auto& arg : fn.args()) {
        arg.removeAttr(llvm::Attribute::Returned);
        arg.removeAttr(llvm::Attribute::WriteOnly);
        arg.removeAttr(llvm::Attribute::ReadNone);
        arg.removeAttr(llvm::Attribute::Initializes);
    }
    fn.removeFnAttr(llvm::Attribute::Memory);
}

}

void fix_x87_control_word_semantics(llvm::Module& m, std::vector<std::string>& errors) {
    auto& ctx = m.getContext();
    auto* i8 = llvm::Type::getInt8Ty(ctx);
    auto* i16 = llvm::Type::getInt16Ty(ctx);
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Function* read16 = m.getFunction("__remill_read_memory_16");
    llvm::Function* write16 = m.getFunction("__remill_write_memory_16");
    llvm::Function* set_rounding = m.getFunction("__remill_fpu_set_rounding");
    llvm::Function* get_rounding = m.getFunction("__remill_fpu_get_rounding");
    llvm::Function* fldcw = find_semantic(m, "5FLDCWEP6Memory");
    llvm::Function* fnstcw = find_semantic(m, "6FNSTCWEP6Memory");
    if (!read16 || !write16 || !set_rounding || !get_rounding || !fldcw || !fnstcw) {
        errors.push_back("x87 control word semantics or intrinsics not found");
        return;
    }
    if (!stores_control_word(*fldcw) || !stores_control_word(*fnstcw) || fldcw->arg_size() != 3 ||
        fnstcw->arg_size() != 3) {
        errors.push_back("x87 control word semantics do not match the expected State layout");
        return;
    }

    clear_body(*fldcw);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "", fldcw));
        llvm::Value* memory = fldcw->getArg(0);
        llvm::Value* slot = b.CreateConstInBoundsGEP1_32(i8, fldcw->getArg(1), kX87ControlWordOffset);
        llvm::Value* cw = b.CreateCall(read16, {memory, fldcw->getArg(2)});
        b.CreateAlignedStore(cw, slot, llvm::Align(2));
        llvm::Value* rc = b.CreateZExt(b.CreateAnd(b.CreateLShr(cw, 10), llvm::ConstantInt::get(i16, 3)), i32);
        b.CreateCall(set_rounding, {rc});
        b.CreateRet(memory);
    }

    clear_body(*fnstcw);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "", fnstcw));
        llvm::Value* memory = fnstcw->getArg(0);
        llvm::Value* slot = b.CreateConstInBoundsGEP1_32(i8, fnstcw->getArg(1), kX87ControlWordOffset);
        llvm::Value* cw = b.CreateAlignedLoad(i16, slot, llvm::Align(2));
        llvm::Value* rc = b.CreateShl(b.CreateAnd(b.CreateTrunc(b.CreateCall(get_rounding, {}), i16),
                                                  llvm::ConstantInt::get(i16, 3)),
                                      10);
        llvm::Value* merged = b.CreateOr(b.CreateAnd(cw, llvm::ConstantInt::get(i16, 0xf3ff)), rc);
        b.CreateAlignedStore(merged, slot, llvm::Align(2));
        b.CreateRet(b.CreateCall(write16, {memory, fnstcw->getArg(2), merged}));
    }
}

void fix_x87_status_word_semantics(llvm::Module& m, std::vector<std::string>& errors) {
    auto& ctx = m.getContext();
    auto* i8 = llvm::Type::getInt8Ty(ctx);
    auto* i16 = llvm::Type::getInt16Ty(ctx);
    llvm::Function* write16 = m.getFunction("__remill_write_memory_16");
    llvm::Function* to_memory = find_semantic(m, "6FNSTSWI3MnWItEEEP6Memory");
    llvm::Function* to_register = find_semantic(m, "6FNSTSWI3RnWItEEEP6Memory");
    if (!write16 || !to_memory || !to_register || to_memory->arg_size() != 3 || to_register->arg_size() != 3 ||
        !to_register->getArg(2)->getType()->isPointerTy()) {
        errors.push_back("x87 FNSTSW semantics not found or not in the expected form");
        return;
    }
    auto build = [&](llvm::Function& fn) -> llvm::Value* {
        clear_body(fn);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "", &fn));
        llvm::Value* state = fn.getArg(1);
        llvm::Value* slot = b.CreateConstInBoundsGEP1_32(i8, state, kX87StatusWordOffset);
        llvm::Value* word = b.CreateAnd(b.CreateAlignedLoad(i16, slot, llvm::Align(2)), llvm::ConstantInt::get(i16, 0x3800));
        constexpr std::pair<unsigned, unsigned> kCodes[] = {{1, 8}, {3, 9}, {5, 10}, {7, 14}};
        for (auto [offset, bit] : kCodes) {
            llvm::Value* flag = b.CreateLoad(i8, b.CreateConstInBoundsGEP1_32(i8, state, kX87StatusFlagsOffset + offset));
            llvm::Value* one = b.CreateZExt(b.CreateAnd(flag, llvm::ConstantInt::get(i8, 1)), i16);
            word = b.CreateOr(word, b.CreateShl(one, bit));
        }
        b.CreateAlignedStore(word, slot, llvm::Align(2));
        return word;
    };
    llvm::Value* word = build(*to_memory);
    {
        llvm::IRBuilder<> b(&to_memory->getEntryBlock());
        b.CreateRet(b.CreateCall(write16, {to_memory->getArg(0), to_memory->getArg(2), word}));
    }
    word = build(*to_register);
    {
        llvm::IRBuilder<> b(&to_register->getEntryBlock());
        b.CreateAlignedStore(word, to_register->getArg(2), llvm::Align(2));
        b.CreateRet(to_register->getArg(0));
    }
}

void mark_x87_exact_semantics(llvm::Module& m, std::vector<std::string>& errors) {
    for (llvm::StringRef name : {"7DoFYL2XEP6Memory", "9DoFYL2XP1EP6Memory", "7DoF2XM1EP6Memory",
                                 "8DoFPATANEP6Memory", "7DoFPREMEP6Memory", "8DoFPREM1EP6Memory",
                                 "8DoFSCALEEP6Memory"}) {
        llvm::Function* fn = find_semantic(m, name);
        if (!fn) {
            errors.push_back(std::format("x87 semantic {} not found", name.str()));
            continue;
        }
        std::vector<llvm::BinaryOperator*> ops;
        for (auto& bb : *fn) {
            for (auto& inst : bb) {
                if (auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(&inst); bin && bin->getType()->isX86_FP80Ty()) {
                    ops.push_back(bin);
                }
            }
        }
        for (auto* bin : ops) {
            auto name = exact_name(bin->getOpcode());
            if (!name) {
                continue;
            }
            auto* type = bin->getType();
            auto callee = m.getOrInsertFunction(*name, llvm::FunctionType::get(type, {type, type}, false));
            auto* decl = llvm::cast<llvm::Function>(callee.getCallee());
            decl->setDoesNotAccessMemory();
            decl->setDoesNotThrow();
            decl->setWillReturn();
            decl->setSpeculatable();
            llvm::IRBuilder<> b(bin);
            llvm::CallInst* call = b.CreateCall(callee, {bin->getOperand(0), bin->getOperand(1)});
            bin->replaceAllUsesWith(call);
            bin->eraseFromParent();
        }
    }
}

size_t preserve_x87_condition_codes(llvm::Module& m, std::vector<std::string>& errors) {
    const llvm::DataLayout& layout = m.getDataLayout();
    std::vector<llvm::StoreInst*> dead;
    for (auto& fn : m) {
        if (fn.isDeclaration()) {
            continue;
        }
        for (auto& bb : fn) {
            for (auto& inst : bb) {
                auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst);
                auto* call = store ? llvm::dyn_cast<llvm::CallInst>(store->getValueOperand()) : nullptr;
                auto* callee = call ? call->getCalledFunction() : nullptr;
                if (!callee || callee->getName() != "__remill_undefined_8") {
                    continue;
                }
                llvm::APInt offset(layout.getIndexTypeSizeInBits(store->getPointerOperandType()), 0);
                auto* base = llvm::dyn_cast<llvm::Argument>(
                    store->getPointerOperand()->stripAndAccumulateConstantOffsets(layout, offset, true));
                if (!base || base->getDereferenceableBytes() <= kX87StatusWordOffset) {
                    continue;
                }
                uint64_t at = offset.getZExtValue();
                if (at >= kX87StatusFlagsOffset && at < kX87StatusFlagsOffset + 8 && (at - kX87StatusFlagsOffset) % 2 == 1) {
                    dead.push_back(store);
                }
            }
        }
    }
    for (auto* store : dead) {
        store->eraseFromParent();
    }
    if (dead.empty()) {
        errors.push_back("no undefined x87 condition-code writes found in the semantics");
    }
    return dead.size();
}

void fix_x87_fpatan_semantics(llvm::Module& m, std::vector<std::string>& errors) {
    llvm::Function* fpatan = find_semantic(m, "8DoFPATANEP6Memory");
    if (!fpatan) {
        errors.push_back("x87 FPATAN semantics not found");
        return;
    }
    llvm::CallInst* atan = nullptr;
    for (auto& bb : *fpatan) {
        for (auto& inst : bb) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (auto* callee = call->getCalledFunction(); callee && callee->getName() == "atanl") {
                    atan = call;
                }
            }
        }
    }
    auto* quotient = atan ? llvm::dyn_cast<llvm::BinaryOperator>(atan->getArgOperand(0)) : nullptr;
    if (!quotient || quotient->getOpcode() != llvm::Instruction::FDiv) {
        errors.push_back("x87 FPATAN semantics do not compute atanl(st1 / st0)");
        return;
    }
    auto* type = atan->getType();
    auto atan2 = m.getOrInsertFunction("atan2l", llvm::FunctionType::get(type, {type, type}, false));
    llvm::IRBuilder<> b(atan);
    llvm::CallInst* replacement = b.CreateCall(atan2, {quotient->getOperand(0), quotient->getOperand(1)});
    replacement->copyMetadata(*atan);
    atan->replaceAllUsesWith(replacement);
    atan->eraseFromParent();
}

void lower_i80_memory(llvm::Function& fn, llvm::Module& module) {
    std::vector<llvm::Instruction*> work;
    for (auto& bb : fn) {
        for (auto& inst : bb) {
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst); st && st->getValueOperand()->getType()->isIntegerTy(80)) {
                work.push_back(st);
            } else if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst); ld && ld->getType()->isIntegerTy(80)) {
                work.push_back(ld);
            }
        }
    }
    auto& ctx = module.getContext();
    auto* i8 = llvm::Type::getInt8Ty(ctx);
    auto* i16 = llvm::Type::getInt16Ty(ctx);
    auto* f64 = llvm::Type::getDoubleTy(ctx);
    for (auto* inst : work) {
        llvm::IRBuilder<> b(inst);
        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(inst)) {
            llvm::Value* bits = st->getValueOperand();
            llvm::Value* ptr = st->getPointerOperand();
            b.CreateAlignedStore(b.CreateCall(double_from_f80_bits(module), {bits}), ptr, llvm::Align(1));
            llvm::Value* high = b.CreateTrunc(b.CreateLShr(bits, 64), i16);
            b.CreateAlignedStore(high, b.CreateConstInBoundsGEP1_32(i8, ptr, 8), llvm::Align(1));
        } else {
            auto* ld = llvm::cast<llvm::LoadInst>(inst);
            llvm::Value* d = b.CreateAlignedLoad(f64, ld->getPointerOperand(), llvm::Align(1));
            inst->replaceAllUsesWith(b.CreateCall(f80_bits_from_double(module), {d}));
        }
        inst->eraseFromParent();
    }
}

void lower_x86_fp80(llvm::Module& module, std::vector<std::string>& errors) {
    for (auto& fn : module) {
        if (fn.isDeclaration()) {
            continue;
        }
        if (!fn.getName().starts_with("ma_f80_bits_from_double") && !fn.getName().starts_with("ma_double_from_f80_bits")) {
            lower_i80_memory(fn, module);
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

size_t route_self_loops(llvm::Function& body, llvm::Function& wrapper, uint32_t addr) {
    llvm::BasicBlock& entry = body.getEntryBlock();
    auto* br = llvm::dyn_cast<llvm::BranchInst>(entry.getTerminator());
    if (!br || br->isConditional()) {
        return 0;
    }
    llvm::BasicBlock* head = br->getSuccessor(0);
    llvm::PHINode* memory = nullptr;
    for (auto& phi : head->phis()) {
        if (phi.getBasicBlockIndex(&entry) >= 0 && phi.getIncomingValueForBlock(&entry) == body.getArg(2)) {
            memory = &phi;
            break;
        }
    }
    std::vector<llvm::BasicBlock*> loops;
    for (auto* pred : llvm::predecessors(head)) {
        if (pred != &entry && std::ranges::find(loops, pred) == loops.end()) {
            loops.push_back(pred);
        }
    }
    auto* i32 = llvm::Type::getInt32Ty(body.getContext());
    for (auto* pred : loops) {
        llvm::Value* mem = memory ? memory->getIncomingValueForBlock(pred) : body.getArg(2);
        auto* target = llvm::BasicBlock::Create(body.getContext(), "mid_hook_loop", &body);
        llvm::IRBuilder<> b(target);
        auto* call = b.CreateCall(&wrapper, {body.getArg(0), llvm::ConstantInt::get(i32, addr), mem});
        call->setTailCall(true);
        b.CreateRet(call);
        pred->getTerminator()->replaceSuccessorWith(head, target);
        for (auto& phi : head->phis()) {
            while (phi.getBasicBlockIndex(pred) >= 0) {
                phi.removeIncomingValue(pred, false);
            }
        }
    }
    return loops.size();
}

void wrap_mid_hook(llvm::Module& m, llvm::Function& body, uint32_t addr) {
    auto& ctx = m.getContext();
    auto* type = body.getFunctionType();
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* ptr = llvm::PointerType::getUnqual(ctx);
    std::string name = body.getName().str();
    body.setName(name + "_body");
    auto* wrapper = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, name, m);
    wrapper->copyAttributesFrom(&body);
    body.replaceAllUsesWith(wrapper);
    auto hook = m.getOrInsertFunction("__ma_mid_hook", llvm::FunctionType::get(i32, {ptr, i32}, false));
    auto jump = m.getOrInsertFunction("__remill_jump", type);
    auto* entry = llvm::BasicBlock::Create(ctx, "", wrapper);
    auto* cont = llvm::BasicBlock::Create(ctx, "continue", wrapper);
    auto* redirect = llvm::BasicBlock::Create(ctx, "resume", wrapper);
    llvm::IRBuilder<> b(entry);
    auto* site = llvm::ConstantInt::get(i32, addr);
    auto* resume = b.CreateCall(hook, {wrapper->getArg(0), site});
    b.CreateCondBr(b.CreateICmpEQ(resume, site), cont, redirect);
    b.SetInsertPoint(cont);
    auto* call = b.CreateCall(&body, {wrapper->getArg(0), wrapper->getArg(1), wrapper->getArg(2)});
    call->setTailCall(true);
    b.CreateRet(call);
    b.SetInsertPoint(redirect);
    auto* jumped = b.CreateCall(jump, {wrapper->getArg(0), resume, wrapper->getArg(2)});
    jumped->setTailCall(true);
    b.CreateRet(jumped);
    route_self_loops(body, *wrapper, addr);
}

size_t route_mid_hook_bodies(llvm::Module& module) {
    size_t routed = 0;
    for (auto& body : module) {
        llvm::StringRef name = body.getName();
        if (!name.starts_with("sub_") || !name.ends_with("_body")) {
            continue;
        }
        llvm::Function* wrapper = module.getFunction(name.drop_back(5));
        if (!wrapper || wrapper->isDeclaration()) {
            continue;
        }
        std::vector<llvm::Use*> uses;
        for (auto& use : body.uses()) {
            auto* inst = llvm::dyn_cast<llvm::Instruction>(use.getUser());
            if (!inst || inst->getFunction() != wrapper) {
                uses.push_back(&use);
            }
        }
        for (auto* use : uses) {
            use->set(wrapper);
            ++routed;
        }
    }
    return routed;
}

size_t keep_traces_separate(llvm::Module& module) {
    size_t marked = 0;
    for (auto& fn : module) {
        if (fn.getName().starts_with("sub_") && !fn.hasFnAttribute(llvm::Attribute::NoInline)) {
            fn.addFnAttr(llvm::Attribute::NoInline);
            ++marked;
        }
    }
    return marked;
}

size_t allow_trace_inlining(llvm::Module& module) {
    size_t cleared = 0;
    for (auto& fn : module) {
        if (fn.getName().starts_with("sub_") && fn.hasFnAttribute(llvm::Attribute::NoInline)) {
            fn.removeFnAttr(llvm::Attribute::NoInline);
            ++cleared;
        }
    }
    return cleared;
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

size_t insert_tso_fences(llvm::Module& module, const std::vector<uint32_t>& functions) {
    size_t fenced = 0;
    for (uint32_t addr : functions) {
        llvm::Function* fn = module.getFunction(std::format("sub_{:08x}", addr));
        if (!fn || fn->isDeclaration()) {
            continue;
        }
        std::vector<std::pair<llvm::CallInst*, bool>> accesses;
        for (auto& bb : *fn) {
            for (auto& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                llvm::Function* callee = call ? call->getCalledFunction() : nullptr;
                if (!callee) {
                    continue;
                }
                llvm::StringRef name = callee->getName();
                if (name.starts_with("__remill_read_memory_")) {
                    accesses.emplace_back(call, false);
                } else if (name.starts_with("__remill_write_memory_")) {
                    accesses.emplace_back(call, true);
                }
            }
        }
        for (auto [call, write] : accesses) {
            if (write) {
                new llvm::FenceInst(module.getContext(), llvm::AtomicOrdering::Release, llvm::SyncScope::System,
                                    call->getIterator());
            } else {
                new llvm::FenceInst(module.getContext(), llvm::AtomicOrdering::Acquire, llvm::SyncScope::System,
                                    std::next(call->getIterator()));
            }
            ++fenced;
        }
    }
    return fenced;
}

}
