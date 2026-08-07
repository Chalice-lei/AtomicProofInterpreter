#ifndef STACK_PERMUTATION_PLANNER_H
#define STACK_PERMUTATION_PLANNER_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "bytecode_opcodes.h"

namespace tbc
{

// StackPlanStep is deliberately independent of Scope and BytecodeGenerator.
// Callers can emit encodedInstructions() and mirror the same operation in
// their symbolic stack without coupling the search code to the AST visitor.
enum class StackPlanOp : uint8_t {
    Swap,
    Rot,
    TwoSwap,
    TwoRot,
    Roll,
    Dup,
    Over,
    Pick,
    Drop,
    Nip,
    Tuck,
    ToAltStack,
    FromAltStack
};

struct StackPlanStep
{
    StackPlanOp op{StackPlanOp::Swap};
    // Used only by Roll and Pick.  It is an offset from the main-stack top.
    size_t depth{0};

    size_t serializedByteSize() const;
    size_t emittedInstructionCount() const;
    size_t touchedDepth() const;
    std::vector<std::string> encodedInstructions() const;
};

struct StackPermutationPlan
{
    std::vector<StackPlanStep> steps;
    size_t serializedBytes{0};
    size_t emittedInstructions{0};
    size_t maximumTouchedDepth{0};

    bool empty() const
    {
        return steps.empty();
    }

    std::vector<std::string> encodedInstructions() const;
};

class StackPermutationPlanner
{
public:
    // Move-only tables remain small through eight slots:
    // sum(1!..8!) == 46,233 states (plus the empty state).
    static constexpr size_t MAX_MOVE_WINDOW = 8;
    static constexpr size_t MAX_COPY_DEPTH = 6;

    // The current top-first window is the identity permutation [0, 1, ...].
    // targetTopFirst must contain every slot id exactly once.  The returned
    // plan uses only SWAP/ROT/2SWAP/2ROT/ROLL and is optimal by the tuple
    // (serialized bytes, instruction count, maximum touched depth).
    static std::optional<StackPermutationPlan>
    planMoveOnly(const std::vector<size_t>& targetTopFirst);

    // Convenience overload for callers that already have stable unique slot
    // ids.  currentTopFirst and targetTopFirst must contain the same ids.
    static std::optional<StackPermutationPlan> planMoveOnly(
        const std::vector<uint64_t>& currentTopFirst,
        const std::vector<uint64_t>& targetTopFirst
    );

    // Produce the shortest supported exact replacement of b with a while
    // preserving the source and every other main-stack position.  Search is
    // limited to source/target depths <= MAX_COPY_DEPTH.  It may use one copy,
    // one deletion, move-only operations, and balanced TOALT/FROMALT pairs.
    // Pre-existing altstack entries are never observed and the temporary
    // planner altstack is empty at both ends.
    //
    // If strictByteLimit is supplied, only plans whose serialized size is
    // strictly smaller are returned.  This is useful for safely replacing a
    // legacy sequence without increasing bytecode size.
    static std::optional<StackPermutationPlan> planCopyAssignment(
        size_t sourceDepth,
        size_t targetDepth,
        size_t strictByteLimit = std::numeric_limits<size_t>::max()
    );

    // Exact byte cost of the scalar copy sequence currently emitted by
    // ASTToBytecodeVisitor.  This lets a caller request only strict wins:
    // planCopyAssignment(a, b, legacyCopyAssignmentByteCost(a, b)).
    static size_t
    legacyCopyAssignmentByteCost(size_t sourceDepth, size_t targetDepth);
};

} // namespace tbc

#endif // STACK_PERMUTATION_PLANNER_H
