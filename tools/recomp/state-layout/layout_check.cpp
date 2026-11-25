#include "layout_check.hpp"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>

namespace ma::recomp {

namespace {

struct Walker {
    const llvm::DataLayout& guest;
    const llvm::DataLayout& host;
    LayoutReport& report;

    void mismatch(const std::string& path, const std::string& what, uint64_t g, uint64_t h) {
        report.mismatches.push_back(path + ": " + what + " guest=" + std::to_string(g) + " host=" + std::to_string(h));
    }

    void walk(llvm::Type* type, const std::string& path, uint64_t guest_offset, uint64_t host_offset) {
        uint64_t gsize = guest.getTypeAllocSize(type);
        uint64_t hsize = host.getTypeAllocSize(type);
        if (guest_offset != host_offset) {
            mismatch(path, "offset", guest_offset, host_offset);
            return;
        }
        if (gsize != hsize) {
            mismatch(path, "size", gsize, hsize);
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
            mismatch(path, "pointer field", guest.getPointerSize(), host.getPointerSize());
            return;
        }
        ++report.leaves;
    }
};

}

LayoutReport compare_struct_layout(llvm::StructType* type, const llvm::DataLayout& guest, const llvm::DataLayout& host) {
    LayoutReport report;
    report.size = guest.getTypeAllocSize(type);
    Walker walker{guest, host, report};
    walker.walk(type, type->getName().str(), 0, 0);
    return report;
}

}
