#include "library_merger.h"

#include <memory>
#include <utility>
#include <vector>

#include "../ast/ast.h"

std::size_t LibraryMerger::mergeIntoContract(ContractNode& contract)
{
    if (contract.libraries.empty()) {
        return 0;
    }

    std::size_t merged = 0;
    std::vector<std::unique_ptr<ASTNode>> mergedMembers;

    // 库成员排在合约成员之前：public 函数调用库函数时，库函数需要先登记。
    for (auto& library : contract.libraries) {
        if (!library) {
            continue;
        }

        for (auto& member : library->members) {
            if (!member) {
                continue;
            }

            if (auto* fn = dynamic_cast<FunctionNode*>(member.get())) {
                fn->fromLibrary = true;
            }
            member->setParent(&contract);
            mergedMembers.push_back(std::move(member));
            ++merged;
        }
    }

    for (auto& member : contract.members) {
        if (member) {
            member->setParent(&contract);
        }
        mergedMembers.push_back(std::move(member));
    }

    contract.members = std::move(mergedMembers);
    contract.libraries.clear();

    return merged;
}
