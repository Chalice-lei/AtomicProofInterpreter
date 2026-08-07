#ifndef STRUCTURED_IF_TAIL_OPTIMIZER_H
#define STRUCTURED_IF_TAIL_OPTIMIZER_H

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace tbc
{

// Placeholder labels listed here must expand to pushed data rather than raw
// executable opcodes. Only an instruction consisting entirely of one known
// placeholder is movable. Unknown/mixed placeholders and all other non-hex
// instructions are retained, but act as barriers to common-tail extraction.
struct StructuredIfTailOptions
{
    std::unordered_set<std::string> knownDataPlaceholderLabels;
};

// Branch paths are expressed in terms of the final instruction stream.  An
// IF/NOTIF instruction's new PC is its control-region identity, which lets a
// debugger compare an origin directly with a runtime branch trace.
using ControlRegionId = size_t;

enum class BranchArm { THEN, ELSE };

struct BranchPredicate
{
    ControlRegionId ifPC{0};
    BranchArm arm{BranchArm::THEN};

    bool operator==(const BranchPredicate&) const = default;
};

// Source-side instruction identity plus the branch decisions under which it
// was reached.  oldPC always addresses the optimizer input; path predicates
// address IF/NOTIF instructions in the optimizer output.
struct InstructionOriginRef
{
    size_t oldPC{0};
    std::vector<BranchPredicate> path;

    bool operator==(const InstructionOriginRef&) const = default;
};

// Complete many-to-one rewrite mapping.  newToOld[newPC] contains all source
// origins represented by that output instruction.
struct InstructionRewritePlan
{
    std::vector<size_t> oldToNew;
    std::vector<std::vector<InstructionOriginRef>> newToOld;
};

// Validates both directions of a normalized plan and every branch predicate.
// Deleted old instructions may use SIZE_MAX in oldToNew.  This is public so
// pass/debug integrations can reject a stale or partially composed plan before
// committing a rewrite.
bool validateInstructionRewritePlan(
    const InstructionRewritePlan& plan,
    const std::vector<std::string>& oldInstructions,
    const std::vector<std::string>& newInstructions
);

struct StructuredIfTailRewriteResult
{
    std::vector<std::string> instructions;

    // Path-aware mapping used by new consumers.  On a successful optimize()
    // call this is normalized and rewritePlanValid is true.  Malformed input
    // still returns a valid identity plan while structurallyValid is false.
    InstructionRewritePlan rewritePlan;
    bool rewritePlanValid{true};

    // Every old instruction maps to its new PC. The two copies of a hoisted
    // common-tail instruction intentionally map to the same new PC.  This is
    // a compatibility view of rewritePlan.oldToNew.
    std::vector<size_t> oldToNew;

    // Reverse provenance for diagnostics and future many-to-one debug mapping.
    // Entries are sorted old PCs and contain at least one origin.  This is a
    // compatibility view that drops branch paths from rewritePlan.newToOld.
    std::vector<std::vector<size_t>> newToOldOrigins;

    bool changed{false};
    bool structurallyValid{true};
    size_t mergedIfCount{0};
    size_t skippedUnsafeTailCount{0};
    size_t removedInstructionCount{0};
};

// Finds exact, complete common suffix nodes in explicit IF/ELSE branches:
//
//   IF A C ELSE B C ENDIF  ->  IF A ELSE B ENDIF C
//
// The optimizer parses the flat instruction vector into a temporary balanced
// control-flow tree. It never cuts through a nested IF. An OP_RETURN,
// OP_CODESEPARATOR, unknown placeholder, or other non-hex instruction anywhere
// in either branch blocks extraction from that IF. On malformed control flow
// the input is returned unchanged with structurallyValid=false.
class StructuredIfTailOptimizer final
{
public:
    static StructuredIfTailRewriteResult optimize(
        const std::vector<std::string>& instructions,
        const StructuredIfTailOptions& options = {}
    );
};

} // namespace tbc

#endif // STRUCTURED_IF_TAIL_OPTIMIZER_H
