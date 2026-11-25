#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class DataLayout;
class StructType;
}

namespace ma::recomp {

struct LayoutReport {
    uint64_t size = 0;
    uint64_t leaves = 0;
    std::vector<std::string> mismatches;
    bool ok() const { return mismatches.empty(); }
};

LayoutReport compare_struct_layout(llvm::StructType* type, const llvm::DataLayout& guest, const llvm::DataLayout& host);

}
