#ifndef FOR_LOOP_POLICY_H
#define FOR_LOOP_POLICY_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace apc::compiler
{

inline constexpr uint64_t kMaxStaticRangeIterations = 100000;
inline constexpr uint64_t kMaxExpandedLoopBodies = 1000000;
inline constexpr size_t kMaxGeneratedLoopInstructions = 2000000;

// A Range assignment overwrites the complete scalar value before the first
// body execution. "auto" is therefore safe once array/compound metadata has
// been excluded by the caller. Keep this set aligned with backend numeric
// assignment compatibility.
inline bool isCompatibleLoopTargetType(std::string_view type)
{
    return type == "int" || type == "number" || type == "num" ||
           type == "uint64" || type == "auto";
}

} // namespace apc::compiler

#endif // FOR_LOOP_POLICY_H
