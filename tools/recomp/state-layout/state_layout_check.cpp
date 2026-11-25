#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdint>
#include <string>

namespace {

struct Walker {
    const llvm::DataLayout& guest;
    const llvm::DataLayout& host;
    uint64_t leaves = 0;
    uint64_t mismatches = 0;

    void report(const std::string& path, const std::string& what, uint64_t g, uint64_t h) {
        ++mismatches;
        if (mismatches <= 25) {
            llvm::errs() << "  " << path << ": " << what << " guest=" << g << " host=" << h << "\n";
        }
    }

    void walk(llvm::Type* type, const std::string& path, uint64_t guest_offset, uint64_t host_offset) {
        uint64_t gsize = guest.getTypeAllocSize(type);
        uint64_t hsize = host.getTypeAllocSize(type);
        if (guest_offset != host_offset) {
            report(path, "offset", guest_offset, host_offset);
            return;
        }
        if (gsize != hsize) {
            report(path, "size", gsize, hsize);
            return;
        }
        if (auto* st = llvm::dyn_cast<llvm::StructType>(type)) {
            const auto* gl = guest.getStructLayout(st);
            const auto* hl = host.getStructLayout(st);
            for (unsigned i = 0; i < st->getNumElements(); ++i) {
                walk(st->getElementType(i), path + "." + std::to_string(i), guest_offset + gl->getElementOffset(i),
                     host_offset + hl->getElementOffset(i));
            }
            return;
        }
        if (auto* at = llvm::dyn_cast<llvm::ArrayType>(type)) {
            if (at->getNumElements() > 0) {
                walk(at->getElementType(), path + "[0]", guest_offset, host_offset);
            }
            return;
        }
        if (type->isPointerTy()) {
            report(path, "pointer field (size differs between targets)", guest.getPointerSize(), host.getPointerSize());
            return;
        }
        ++leaves;
    }
};

}

int main(int argc, char** argv) {
    if (argc < 3) {
        llvm::errs() << "usage: state-layout-check <lifted.ll> <host datalayout> [struct name]\n";
        return 64;
    }
    llvm::LLVMContext context;
    llvm::SMDiagnostic diag;
    auto module = llvm::parseIRFile(argv[1], diag, context);
    if (!module) {
        diag.print("state-layout-check", llvm::errs());
        return 1;
    }
    auto host = llvm::DataLayout::parse(argv[2]);
    if (!host) {
        llvm::errs() << "invalid host datalayout: " << llvm::toString(host.takeError()) << "\n";
        return 1;
    }
    std::string name = argc > 3 ? argv[3] : "struct.State";
    auto* state = llvm::StructType::getTypeByName(context, name);
    if (!state) {
        llvm::errs() << name << " not found in " << argv[1] << "\n";
        return 1;
    }
    const llvm::DataLayout& guest = module->getDataLayout();
    Walker walker{guest, *host};
    walker.walk(state, name, 0, 0);
    if (walker.mismatches != 0) {
        llvm::errs() << name << ": " << walker.mismatches << " layout mismatch(es) between guest and host datalayouts\n";
        return 1;
    }
    llvm::outs() << name << ": " << guest.getTypeAllocSize(state) << " bytes, " << walker.leaves
                 << " leaf fields, identical layout on guest and host\n";
    return 0;
}
