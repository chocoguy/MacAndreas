#include "check.hpp"
#include "ir_fixups.hpp"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

namespace {

llvm::FunctionType* trace_type(llvm::LLVMContext& ctx) {
    auto* ptr = llvm::PointerType::getUnqual(ctx);
    return llvm::FunctionType::get(ptr, {ptr, llvm::Type::getInt32Ty(ctx), ptr}, false);
}

llvm::Function* define_leaf(llvm::Module& m, const std::string& name) {
    auto* fn = llvm::Function::Create(trace_type(m.getContext()), llvm::GlobalValue::ExternalLinkage, name, m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(m.getContext(), "", fn));
    b.CreateRet(fn->getArg(2));
    return fn;
}

llvm::Function* define_caller(llvm::Module& m, const std::string& name, llvm::Function* callee) {
    auto* fn = llvm::Function::Create(trace_type(m.getContext()), llvm::GlobalValue::ExternalLinkage, name, m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(m.getContext(), "", fn));
    auto* call = b.CreateCall(callee, {fn->getArg(0), b.getInt32(0x1000), fn->getArg(2)});
    call->setTailCall(true);
    b.CreateRet(call);
    return fn;
}

std::vector<llvm::CallInst*> calls_in(llvm::Function& fn) {
    std::vector<llvm::CallInst*> out;
    for (auto& block : fn) {
        for (auto& inst : block) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                out.push_back(call);
            }
        }
    }
    return out;
}

bool calls(llvm::Function& fn, llvm::StringRef name) {
    for (auto* call : calls_in(fn)) {
        if (call->getCalledFunction() && call->getCalledFunction()->getName() == name) {
            return true;
        }
    }
    return false;
}

bool verified(llvm::Module& m) {
    std::string text;
    llvm::raw_string_ostream os(text);
    bool broken = llvm::verifyModule(m, &os);
    if (broken) {
        std::fprintf(stderr, "%s\n", text.c_str());
    }
    return !broken;
}

void test_wrap_routes_same_module_callers() {
    llvm::LLVMContext ctx;
    llvm::Module m("shard", ctx);
    auto* body = define_leaf(m, "sub_00001000");
    auto* caller = define_caller(m, "sub_00002000", body);
    ma::recomp::wrap_mid_hook(m, *body, 0x1000);
    CHECK(verified(m));
    auto* wrapper = m.getFunction("sub_00001000");
    CHECK(wrapper && !wrapper->isDeclaration());
    CHECK(wrapper != body);
    CHECK(body->getName() == "sub_00001000_body");
    CHECK(calls(*caller, "sub_00001000"));
    CHECK(!calls(*caller, "sub_00001000_body"));
    CHECK(calls(*wrapper, "sub_00001000_body"));
    CHECK(calls(*wrapper, "__remill_jump"));
    bool site = false;
    for (auto* call : calls_in(*wrapper)) {
        if (call->getCalledFunction() && call->getCalledFunction()->getName() == "__ma_mid_hook") {
            auto* arg = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
            site = arg && arg->getZExtValue() == 0x1000 && call->getArgOperand(0) == wrapper->getArg(0);
        }
    }
    CHECK(site);
    CHECK(&wrapper->getEntryBlock() == &*wrapper->begin());
    CHECK(calls_in(*wrapper).front()->getCalledFunction()->getName() == "__ma_mid_hook");
}

void test_route_repairs_old_shards() {
    llvm::LLVMContext ctx;
    llvm::Module m("linked", ctx);
    auto* body = define_leaf(m, "sub_00001000");
    ma::recomp::wrap_mid_hook(m, *body, 0x1000);
    auto* stale = define_caller(m, "sub_00003000", body);
    CHECK(calls(*stale, "sub_00001000_body"));
    CHECK(ma::recomp::route_mid_hook_bodies(m) == 1);
    CHECK(calls(*stale, "sub_00001000"));
    CHECK(!calls(*stale, "sub_00001000_body"));
    CHECK(calls(*m.getFunction("sub_00001000"), "sub_00001000_body"));
    CHECK(ma::recomp::route_mid_hook_bodies(m) == 0);
    CHECK(verified(m));
}

void test_route_ignores_unwrapped_bodies() {
    llvm::LLVMContext ctx;
    llvm::Module m("plain", ctx);
    auto* body = define_leaf(m, "sub_00004000_body");
    define_caller(m, "sub_00005000", body);
    CHECK(ma::recomp::route_mid_hook_bodies(m) == 0);
}

}

void test_loops_back_to_the_site_run_the_hook() {
    llvm::LLVMContext ctx;
    llvm::Module m("shard", ctx);
    auto* fn = llvm::Function::Create(trace_type(ctx), llvm::GlobalValue::ExternalLinkage, "sub_00001000", m);
    auto* entry = llvm::BasicBlock::Create(ctx, "", fn);
    auto* head = llvm::BasicBlock::Create(ctx, "inst_1000", fn);
    auto* loop = llvm::BasicBlock::Create(ctx, "inst_1010", fn);
    auto* done = llvm::BasicBlock::Create(ctx, "inst_1020", fn);
    llvm::IRBuilder<> b(entry);
    b.CreateBr(head);
    b.SetInsertPoint(head);
    auto* memory = b.CreatePHI(fn->getArg(2)->getType(), 2);
    memory->addIncoming(fn->getArg(2), entry);
    auto* counter = b.CreatePHI(b.getInt32Ty(), 2);
    counter->addIncoming(b.getInt32(0), entry);
    b.CreateCondBr(b.CreateICmpEQ(counter, b.getInt32(3)), done, loop);
    b.SetInsertPoint(loop);
    auto* next = b.CreateAdd(counter, b.getInt32(1));
    memory->addIncoming(memory, loop);
    counter->addIncoming(next, loop);
    b.CreateBr(head);
    b.SetInsertPoint(done);
    b.CreateRet(memory);
    ma::recomp::wrap_mid_hook(m, *fn, 0x1000);
    CHECK(verified(m));
    auto* wrapper = m.getFunction("sub_00001000");
    CHECK(wrapper && wrapper != fn);
    CHECK(llvm::pred_size(head) == 1);
    auto* back = loop->getTerminator()->getSuccessor(0);
    CHECK(back != head);
    bool calls_wrapper = false;
    for (auto& inst : *back) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            calls_wrapper = call->getCalledFunction() == wrapper && call->getArgOperand(2) == memory;
        }
    }
    CHECK(calls_wrapper);
    CHECK(memory->getNumIncomingValues() == 1);
}

void test_traces_stay_separate_until_routed() {
    llvm::LLVMContext ctx;
    llvm::Module m("shard", ctx);
    auto* target = define_leaf(m, "sub_00002000");
    auto* jumper = define_caller(m, "sub_00001000", target);
    auto* caller = llvm::Function::Create(trace_type(ctx), llvm::GlobalValue::ExternalLinkage, "sub_00003000", m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "", caller));
    auto* call = b.CreateCall(jumper, {caller->getArg(0), b.getInt32(0x1000), caller->getArg(2)});
    b.CreateStore(b.getInt32(1), caller->getArg(0));
    b.CreateRet(call);
    CHECK(ma::recomp::keep_traces_separate(m) == 3);
    CHECK(jumper->hasFnAttribute(llvm::Attribute::NoInline));
    CHECK(ma::recomp::route_calls_through_dispatch(m) == 1);
    CHECK(calls(*caller, "__remill_function_call"));
    CHECK(!calls(*caller, "sub_00001000"));
    CHECK(calls(*jumper, "sub_00002000"));
    CHECK(ma::recomp::allow_trace_inlining(m) == 3);
    CHECK(!jumper->hasFnAttribute(llvm::Attribute::NoInline));
    CHECK(verified(m));
}

int main() {
    test_wrap_routes_same_module_callers();
    test_route_repairs_old_shards();
    test_route_ignores_unwrapped_bodies();
    test_traces_stay_separate_until_routed();
    test_loops_back_to_the_site_run_the_hook();
    return ma::test::finish("recomp.mid_hook_wrapper");
}
