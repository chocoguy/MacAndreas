#include "lift.hpp"

#include "analysis.hpp"
#include "guest_image.hpp"
#include "ir_fixups.hpp"

#include "macandreas/manifest.hpp"

#include <remill/Arch/Arch.h>
#include <remill/Arch/Instruction.h>
#include <remill/Arch/Name.h>
#include <remill/BC/IntrinsicTable.h>
#include <remill/BC/Optimizer.h>
#include <remill/BC/TraceLifter.h>
#include <remill/BC/Util.h>
#include <remill/OS/OS.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ma::recomp {

namespace {

uint32_t guest_addr(uint64_t addr) { return static_cast<uint32_t>(addr); }

std::string trace_name(uint64_t addr) { return std::format("sub_{:08x}", guest_addr(addr)); }

class Manager final : public remill::TraceManager {
public:
    Manager(const remill::Arch* arch, llvm::Module* semantics, const GuestImage& image, const Analysis& analysis)
        : arch_(arch), semantics_(semantics), image_(image) {
        for (const auto& f : analysis.functions) {
            if (image.is_exec(f.entry)) {
                heads_.insert(f.entry);
            }
        }
    }

    std::string TraceName(uint64_t addr) override { return trace_name(addr); }

    void SetLiftedTraceDefinition(uint64_t addr, llvm::Function* fn) override {
        batch_[guest_addr(addr)] = fn;
        lifted_.insert(guest_addr(addr));
    }

    llvm::Function* GetLiftedTraceDefinition(uint64_t addr) override {
        if (auto it = batch_.find(guest_addr(addr)); it != batch_.end()) {
            return it->second;
        }
        if (lifted_.contains(guest_addr(addr))) {
            return declare(addr);
        }
        return nullptr;
    }

    llvm::Function* GetLiftedTraceDeclaration(uint64_t addr) override {
        auto a = guest_addr(addr);
        if (heads_.contains(a) || lifted_.contains(a)) {
            return declare(addr);
        }
        return nullptr;
    }

    bool TryReadExecutableByte(uint64_t addr, uint8_t* byte) override {
        uint8_t value = 0;
        if (!image_.read_exec(guest_addr(addr), value)) {
            return false;
        }
        if (byte) {
            *byte = value;
        }
        return true;
    }

    bool lifted(uint32_t addr) const { return lifted_.contains(addr); }
    const std::unordered_set<uint32_t>& all_lifted() const { return lifted_; }
    std::unordered_map<uint64_t, llvm::Function*>& batch() { return batch_; }

private:
    llvm::Function* declare(uint64_t addr) {
        auto name = trace_name(addr);
        if (auto* fn = semantics_->getFunction(name)) {
            return fn;
        }
        return arch_->DeclareLiftedFunction(name, semantics_);
    }

    const remill::Arch* arch_;
    llvm::Module* semantics_;
    const GuestImage& image_;
    std::unordered_set<uint32_t> heads_;
    std::unordered_set<uint32_t> lifted_;
    std::unordered_map<uint64_t, llvm::Function*> batch_;
};

void prepare_semantics(llvm::Module& semantics) {
    if (auto* hyper = semantics.getFunction("__remill_sync_hyper_call"); hyper && !hyper->isDeclaration()) {
        auto name = hyper->getName().str();
        auto* type = hyper->getFunctionType();
        auto replacement = semantics.getOrInsertFunction(name + "_ma", type);
        hyper->replaceAllUsesWith(replacement.getCallee());
        hyper->eraseFromParent();
        llvm::cast<llvm::Function>(replacement.getCallee())->setName(name);
    }
    relax_intrinsic_memory_attributes(semantics);
}

std::expected<void, std::string> write_module(llvm::Module& module, const std::filesystem::path& path) {
    std::error_code ec;
    llvm::raw_fd_ostream os(path.string(), ec);
    if (ec) {
        return std::unexpected(std::format("cannot write {}: {}", path.string(), ec.message()));
    }
    llvm::WriteBitcodeToFile(module, os);
    return {};
}

std::expected<void, std::string> write_functable(const std::filesystem::path& path, const std::set<uint32_t>& defined,
                                                 const std::set<uint32_t>& stubs) {
    llvm::LLVMContext ctx;
    llvm::Module m("functable", ctx);
    auto* ptr = llvm::PointerType::getUnqual(ctx);
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* trace_type = llvm::FunctionType::get(ptr, {ptr, i32, ptr}, false);
    auto missing = m.getOrInsertFunction("__remill_missing_block", trace_type);

    for (uint32_t addr : stubs) {
        auto* fn = llvm::Function::Create(trace_type, llvm::GlobalValue::ExternalLinkage, trace_name(addr), m);
        auto* bb = llvm::BasicBlock::Create(ctx, "", fn);
        llvm::IRBuilder<> b(bb);
        auto* call = b.CreateCall(missing, {fn->getArg(0), llvm::ConstantInt::get(i32, addr), fn->getArg(2)});
        call->setTailCall(true);
        b.CreateRet(call);
    }

    auto* entry_type = llvm::StructType::get(ctx, {i32, ptr});
    std::vector<llvm::Constant*> entries;
    std::set<uint32_t> all = defined;
    all.insert(stubs.begin(), stubs.end());
    for (uint32_t addr : all) {
        auto callee = m.getOrInsertFunction(trace_name(addr), trace_type);
        entries.push_back(llvm::ConstantStruct::get(
            entry_type, {llvm::ConstantInt::get(i32, addr), llvm::cast<llvm::Constant>(callee.getCallee())}));
    }
    auto* array_type = llvm::ArrayType::get(entry_type, entries.size());
    new llvm::GlobalVariable(m, array_type, true, llvm::GlobalValue::ExternalLinkage,
                             llvm::ConstantArray::get(array_type, entries), "ma_functable");
    new llvm::GlobalVariable(m, i32, true, llvm::GlobalValue::ExternalLinkage,
                             llvm::ConstantInt::get(i32, entries.size()), "ma_functable_count");
    return write_module(m, path);
}

std::expected<size_t, std::string> apply_manifest(const std::filesystem::path& dir, GuestImage& image,
                                                  const Analysis& analysis, const remill::Arch& arch) {
    auto loaded = ma::manifest::load_directory(dir);
    std::string report;
    if (!loaded) {
        for (const auto& d : loaded.error()) {
            report += d.text() + "\n";
        }
        return std::unexpected(report);
    }
    std::map<uint32_t, uint32_t> extents;
    for (const auto& f : analysis.functions) {
        extents[f.entry] = f.end;
    }
    auto containing = [&](uint32_t addr) -> std::optional<std::pair<uint32_t, uint32_t>> {
        auto it = extents.upper_bound(addr);
        if (it == extents.begin()) {
            return std::nullopt;
        }
        --it;
        if (it->second == 0 || addr >= it->second) {
            return std::nullopt;
        }
        return *it;
    };
    ma::manifest::CheckContext context;
    context.is_function_entry = [&](uint32_t addr) { return extents.contains(addr); };
    context.function_end = [&](uint32_t addr) -> std::optional<uint32_t> {
        auto it = extents.find(addr);
        if (it == extents.end() || it->second == 0) {
            return std::nullopt;
        }
        return it->second;
    };
    context.is_call_instruction = [&](uint32_t addr) { return image.is_call_at(addr); };
    context.is_instruction_start = [&](uint32_t addr) {
        auto fn = containing(addr);
        if (!fn) {
            return true;
        }
        uint32_t pc = fn->first;
        while (pc < addr) {
            std::string bytes;
            for (uint32_t i = 0; i < 15; ++i) {
                uint8_t b = 0;
                if (!image.read_exec(pc + i, b)) {
                    break;
                }
                bytes.push_back(static_cast<char>(b));
            }
            remill::Instruction inst;
            if (!arch.DecodeInstruction(pc, bytes, inst, arch.CreateInitialContext()) || inst.bytes.empty()) {
                return true;
            }
            pc += static_cast<uint32_t>(inst.bytes.size());
        }
        return pc == addr;
    };
    for (const auto& d : ma::manifest::check(*loaded, context)) {
        report += d.text() + "\n";
    }
    size_t applied = 0;
    for (const auto& e : loaded->entries) {
        if (e.kind == ma::manifest::Kind::call_redirect || e.kind == ma::manifest::Kind::mid_hook) {
            report += std::format("{}:{}: {} lowering is not implemented yet\n", e.file, e.line,
                                  ma::manifest::to_string(e.kind));
            continue;
        }
        if (e.kind != ma::manifest::Kind::byte_patch && e.kind != ma::manifest::Kind::nop) {
            continue;
        }
        std::vector<uint8_t> bytes = e.kind == ma::manifest::Kind::nop ? std::vector<uint8_t>(e.len, 0x90) : e.bytes;
        if (!image.patch(e.addr, bytes)) {
            report += std::format("{}:{}: 0x{:08x} is not backed by file data\n", e.file, e.line, e.addr);
            continue;
        }
        ++applied;
    }
    if (!report.empty()) {
        return std::unexpected(report);
    }
    return applied;
}

uint32_t parse_trace_name(llvm::StringRef name) {
    uint32_t addr = 0;
    name.drop_front(4).getAsInteger(16, addr);
    return addr;
}

}

std::expected<LiftResult, std::string> lift_program(const LiftOptions& options) {
    auto image = GuestImage::load(options.exe);
    if (!image) {
        return std::unexpected(image.error());
    }
    auto analysis = load_analysis(options.analysis_dir);
    if (!analysis) {
        return std::unexpected(analysis.error());
    }
    std::filesystem::create_directories(options.out_dir);

    llvm::LLVMContext context;
    auto arch = remill::Arch::Get(context, remill::kOSWindows, remill::kArchX86);
    if (!arch) {
        return std::unexpected("remill has no x86 architecture");
    }
    std::unique_ptr<llvm::Module> semantics(remill::LoadArchSemantics(arch.get()));
    if (!semantics) {
        return std::unexpected("cannot load remill x86 semantics");
    }
    prepare_semantics(*semantics);

    LiftResult result;
    if (!options.manifest_dir.empty()) {
        auto applied = apply_manifest(options.manifest_dir, *image, *analysis, *arch);
        if (!applied) {
            return std::unexpected("manifest:\n" + applied.error());
        }
        result.manifest_patches = *applied;
    }

    remill::IntrinsicTable intrinsics(semantics.get());
    Manager manager(arch.get(), semantics.get(), *image, *analysis);
    remill::TraceLifter lifter(arch.get(), manager);

    std::vector<uint32_t> roots;
    if (!options.only.empty()) {
        roots = options.only;
    } else {
        roots.push_back(image->entry());
        for (const auto& f : analysis->functions) {
            if (image->is_exec(f.entry)) {
                roots.push_back(f.entry);
            }
        }
        for (uint32_t extra : analysis->extra_roots) {
            if (image->is_exec(extra)) {
                roots.push_back(extra);
            }
        }
        for (const auto& [site, targets] : analysis->jump_targets) {
            for (uint32_t target : targets) {
                if (image->is_exec(target)) {
                    roots.push_back(target);
                }
            }
        }
        size_t recovered_sites = 0;
        size_t recovered_targets = 0;
        for (uint32_t site : analysis->unresolved_jumps) {
            auto targets = image->scan_jump_table(site);
            if (targets.empty()) {
                result.unresolved_jumps.push_back(site);
                continue;
            }
            ++recovered_sites;
            recovered_targets += targets.size();
            roots.insert(roots.end(), targets.begin(), targets.end());
        }
        std::fprintf(stderr, "sa-recomp: recovered %zu of %zu unresolved jump tables (%zu targets)\n", recovered_sites,
                     analysis->unresolved_jumps.size(), recovered_targets);
        if (options.data_pointer_roots) {
            uint32_t base = image->image().image_base;
            for (const auto& s : image->image().sections) {
                if (s.name == ".text" || s.name == "_rwcseg") {
                    uint32_t start = base + s.virtual_address;
                    if (start >= 0x01000000) {
                        continue;
                    }
                    auto found = image->data_pointers_into(start, start + s.mapped_size());
                    roots.insert(roots.end(), found.begin(), found.end());
                    auto immediates = image->code_immediates_into(start, start + s.mapped_size());
                    roots.insert(roots.end(), immediates.begin(), immediates.end());
                }
            }
        }
    }
    std::ranges::sort(roots);
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    if (options.limit != 0 && roots.size() > options.limit) {
        roots.resize(options.limit);
    }

    result.roots = roots.size();
    std::set<uint32_t> referenced;
    std::set<uint32_t> defined;
    size_t shard_index = 0;
    auto started = std::chrono::steady_clock::now();

    auto flush = [&]() -> std::expected<void, std::string> {
        auto& batch = manager.batch();
        if (batch.empty()) {
            return {};
        }
        relax_intrinsic_memory_attributes(*semantics);
        remill::OptimizeBareModule(semantics.get());

        auto shard_name = std::format("shard_{:04}", shard_index++);
        llvm::Module shard(shard_name, context);
        arch->PrepareModuleDataLayout(&shard);
        std::vector<std::pair<uint64_t, llvm::Function*>> ordered(batch.begin(), batch.end());
        std::ranges::sort(ordered, {}, &std::pair<uint64_t, llvm::Function*>::first);
        for (auto& [addr, fn] : ordered) {
            remill::MoveFunctionIntoModule(fn, &shard);
            defined.insert(static_cast<uint32_t>(addr));
        }
        batch.clear();
        for (auto& fn : shard) {
            if (fn.isDeclaration() && fn.getName().starts_with("sub_")) {
                referenced.insert(parse_trace_name(fn.getName()));
            }
        }
        auto path = options.out_dir / (shard_name + ".bc");
        if (auto w = write_module(shard, path); !w) {
            return std::unexpected(w.error());
        }
        result.shards.push_back(path);
        auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::fprintf(stderr, "\rsa-recomp: lifted %zu traces into %zu shards (%.0fs)", defined.size(), shard_index,
                     secs);
        return {};
    };

    for (uint32_t root : roots) {
        if (manager.lifted(root)) {
            continue;
        }
        lifter.Lift(root);
        if (manager.batch().size() >= options.traces_per_shard) {
            if (auto f = flush(); !f) {
                return std::unexpected(f.error());
            }
        }
    }
    if (auto f = flush(); !f) {
        return std::unexpected(f.error());
    }
    std::fprintf(stderr, "\n");

    std::set<uint32_t> stubs;
    for (uint32_t addr : referenced) {
        if (!defined.contains(addr)) {
            stubs.insert(addr);
        }
    }
    result.traces = defined.size();
    result.stubs = stubs.size();
    result.functable = options.out_dir / "functable.bc";
    if (auto w = write_functable(result.functable, defined, stubs); !w) {
        return std::unexpected(w.error());
    }
    return result;
}

}
