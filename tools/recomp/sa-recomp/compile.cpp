#include "compile.hpp"

#include "ir_fixups.hpp"
#include "layout_check.hpp"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdio>
#include <format>
#include <memory>

namespace ma::recomp {

namespace {

std::expected<std::unique_ptr<llvm::Module>, std::string> read_module(const std::filesystem::path& path,
                                                                      llvm::LLVMContext& ctx) {
    llvm::SMDiagnostic diag;
    auto module = llvm::parseIRFile(path.string(), diag, ctx);
    if (!module) {
        std::string text;
        llvm::raw_string_ostream os(text);
        diag.print("sa-recomp", os);
        return std::unexpected(text);
    }
    return module;
}

std::expected<std::unique_ptr<llvm::TargetMachine>, std::string> make_target_machine() {
    static bool initialized = [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        return true;
    }();
    (void)initialized;
    llvm::Triple triple(kHostTriple);
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        return std::unexpected(error);
    }
    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
        triple, kHostCpu, "", options, llvm::Reloc::PIC_, std::nullopt, llvm::CodeGenOptLevel::Default));
    if (!tm) {
        return std::unexpected("cannot create arm64 target machine");
    }
    return tm;
}

std::string join_errors(const std::vector<std::string>& errors) {
    std::string out;
    for (const auto& e : errors) {
        out += "\n  " + e;
    }
    return out;
}

void internalize_runtime(llvm::Module& module) {
    for (auto& fn : module) {
        if (fn.isDeclaration()) {
            continue;
        }
        llvm::StringRef name = fn.getName();
        if (name.starts_with("__remill_") || name.starts_with("ma_")) {
            fn.setLinkage(llvm::GlobalValue::InternalLinkage);
            fn.removeFnAttr(llvm::Attribute::NoInline);
            fn.removeFnAttr(llvm::Attribute::OptimizeNone);
            fn.addFnAttr(llvm::Attribute::AlwaysInline);
        }
    }
}

size_t skip_optimizing_oversized_traces(llvm::Module& module, size_t max_instructions) {
    size_t skipped = 0;
    for (auto& fn : module) {
        if (fn.isDeclaration() || !fn.getName().starts_with("sub_") || fn.getInstructionCount() <= max_instructions) {
            continue;
        }
        fn.addFnAttr(llvm::Attribute::NoInline);
        fn.addFnAttr(llvm::Attribute::OptimizeNone);
        std::fprintf(stderr, "sa-recomp: %s has %u instructions, compiling without optimization\n",
                     fn.getName().str().c_str(), fn.getInstructionCount());
        ++skipped;
    }
    return skipped;
}

}

std::expected<void, std::string> compile_module(const CompileOptions& options) {
    llvm::LLVMContext ctx;
    auto module = read_module(options.input, ctx);
    if (!module) {
        return std::unexpected(module.error());
    }
    auto tm = make_target_machine();
    if (!tm) {
        return std::unexpected(tm.error());
    }
    llvm::Module& m = **module;
    llvm::DataLayout host_layout = (*tm)->createDataLayout();

    if (auto* state = llvm::StructType::getTypeByName(ctx, "struct.State")) {
        auto report = compare_struct_layout(state, m.getDataLayout(), host_layout);
        if (!report.ok()) {
            return std::unexpected(std::format("{}: State layout differs between i386 and arm64:{}",
                                               options.input.string(), join_errors(report.mismatches)));
        }
    }

    std::vector<std::string> errors;
    lower_x86_fp80(m, errors);
    rewrite_x86_inline_asm(m, errors);
    if (!errors.empty()) {
        return std::unexpected(std::format("{}: IR fixups failed:{}", options.input.string(), join_errors(errors)));
    }
    strip_x86_function_attributes(m);
    relax_intrinsic_memory_attributes(m);
    route_calls_through_dispatch(m);
    insert_tso_fences(m, options.tso);

    m.setTargetTriple(llvm::Triple(kHostTriple));
    m.setDataLayout(host_layout);

    if (!options.runtime_bc.empty()) {
        auto runtime = read_module(options.runtime_bc, ctx);
        if (!runtime) {
            return std::unexpected(runtime.error());
        }
        if (llvm::Linker::linkModules(m, std::move(*runtime), llvm::Linker::Flags::LinkOnlyNeeded)) {
            return std::unexpected(std::format("{}: linking guest intrinsics failed", options.input.string()));
        }
        internalize_runtime(m);
    }
    enable_guest_unwinding(m);

    std::string verify_text;
    llvm::raw_string_ostream verify_os(verify_text);
    if (llvm::verifyModule(m, &verify_os)) {
        return std::unexpected(std::format("{}: invalid IR before optimization:\n{}", options.input.string(),
                                           verify_text.substr(0, 4000)));
    }

    skip_optimizing_oversized_traces(m, options.max_optimized_instructions);

    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassInstrumentationCallbacks pic;
    llvm::StandardInstrumentations si(ctx, false);
    si.registerCallbacks(pic, &mam);
    llvm::PassBuilder pb(tm->get(), llvm::PipelineTuningOptions(), std::nullopt, &pic);
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cam, mam);
    llvm::OptimizationLevel level = options.opt_level >= 2 ? llvm::OptimizationLevel::O2
                                    : options.opt_level == 1 ? llvm::OptimizationLevel::O1
                                                             : llvm::OptimizationLevel::O0;
    llvm::ModulePassManager mpm = level == llvm::OptimizationLevel::O0
                                      ? pb.buildO0DefaultPipeline(level)
                                      : pb.buildPerModuleDefaultPipeline(level);
    mpm.run(m, mam);

    std::error_code ec;
    llvm::raw_fd_ostream out(options.output.string(), ec);
    if (ec) {
        return std::unexpected(std::format("cannot write {}: {}", options.output.string(), ec.message()));
    }
    if (options.emit_ir) {
        m.print(out, nullptr);
        return {};
    }
    llvm::legacy::PassManager codegen;
    if ((*tm)->addPassesToEmitFile(codegen, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        return std::unexpected("arm64 target cannot emit object files");
    }
    codegen.run(m);
    return {};
}

}
