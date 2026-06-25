#ifndef LIBRARY_MERGER_H
#define LIBRARY_MERGER_H

#include <cstddef>

class ContractNode;

class LibraryMerger
{
public:
    // 将解析阶段保存在 ContractNode::libraries 中的库成员前置合并到
    // ContractNode::members。返回被合并的库成员数量。
    static std::size_t mergeIntoContract(ContractNode& contract);
};

#endif // LIBRARY_MERGER_H
