#include "layout_check.hpp"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

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
    auto report = ma::recomp::compare_struct_layout(state, module->getDataLayout(), *host);
    if (!report.ok()) {
        for (size_t i = 0; i < report.mismatches.size() && i < 25; ++i) {
            llvm::errs() << "  " << report.mismatches[i] << "\n";
        }
        llvm::errs() << name << ": " << report.mismatches.size() << " layout mismatch(es) between guest and host datalayouts\n";
        return 1;
    }
    llvm::outs() << name << ": " << report.size << " bytes, " << report.leaves
                 << " leaf fields, identical layout on guest and host\n";
    return 0;
}
