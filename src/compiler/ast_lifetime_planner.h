#ifndef AST_LIFETIME_PLANNER_H
#define AST_LIFETIME_PLANNER_H

#include "../ast/ast.h"
#include "value_lifetime_analysis.h"

#include <map>
#include <string>
#include <vector>

namespace apc::compiler
{

// AST lifetime planning deliberately runs after ConstantFolder.  At that
// point dead branches have been removed and statically-known indices have
// normally become literals, so the planner can distinguish a precise access
// from a conservative dynamic-index escape without duplicating folding logic.
enum class AstLifetimePlanningStage {
    AfterConstantFolding
};

enum class AstLifetimePlannerIssueCode {
    MissingFunctionBody,
    InvalidDefinition,
    UnknownAssignmentTarget,
    InternalControlFlowError
};

struct AstLifetimePlannerIssue
{
    AstLifetimePlannerIssueCode code{
        AstLifetimePlannerIssueCode::InternalControlFlowError
    };
    const ASTNode* node{nullptr};
    std::string message;
};

struct AstValueRecord
{
    std::string name;
    BindingId binding;
    ValueId value;
    AtomId atom;
    ValueContext context;
    const ASTNode* definition{nullptr};
    bool parameter{false};
    bool alias{false};
};

struct InlineCallContext
{
    const CallNode* call{nullptr};
    const FunctionNode* callee{nullptr};
    CallSiteId callSite;
    InlineFrameId inlineFrame;

    // This planning layer does not replay an inline callee's mutable AST at
    // every call site.  Its arguments are therefore escaped conservatively;
    // the IDs still give the lowering layer stable context keys when it does
    // instantiate the callee.
    bool conservativelyEscaped{true};
};

struct LoopPlanningContext
{
    const ForNode* loop{nullptr};
    LoopIterationId iteration;

    // A true value denotes one summary iteration connected by a CFG backedge,
    // not a promise that the AST was statically expanded N times.
    bool summarized{true};
};

struct ReclaimableBinding
{
    std::string name;
    BindingId binding;
    ValueId value;
};

struct AstKillSuggestion
{
    KillSuggestion lifetime;
    std::vector<ReclaimableBinding> bindings;
};

using AstKillSchedule =
    std::map<const StmtNode*, std::vector<AstKillSuggestion>>;

struct AstLifetimePlan
{
    AstLifetimePlanningStage stage{
        AstLifetimePlanningStage::AfterConstantFolding
    };
    LifetimeControlFlowGraph cfg;
    StableValueTable values;
    LifetimeAnalysisResult analysis;
    std::vector<AstValueRecord> valueRecords;
    std::map<const CallNode*, InlineCallContext> inlineCalls;
    std::map<const ForNode*, LoopPlanningContext> loops;
    std::vector<AstLifetimePlannerIssue> issues;

    // Only cleanup-requiring, non-escaped suggestions from a valid analysis
    // are published here.  Consume operations remain visible in
    // analysis.kills but are intentionally absent from this schedule.
    AstKillSchedule cleanupSchedule;

    bool valid() const noexcept
    {
        return issues.empty() && analysis.valid();
    }

    const std::vector<AstKillSuggestion>&
    suggestionsAfter(const StmtNode* statement) const;

    std::vector<const AstValueRecord*>
    recordsForName(const std::string& name) const;
};

// Builds a conservative, statement-anchored lifetime CFG for one function.
// The optional contract is used only to identify private/library calls and
// assign stable CallSiteId/InlineFrameId pairs; calls are not recursively
// expanded at this layer.
class AstLifetimePlanner final
{
public:
    explicit AstLifetimePlanner(const ContractNode* contract = nullptr)
        : m_contract(contract)
    {}

    AstLifetimePlan
    planAfterConstantFolding(const FunctionNode& function) const;

private:
    const ContractNode* m_contract{nullptr};
};

using ContractLifetimePlans =
    std::map<const FunctionNode*, AstLifetimePlan>;

ContractLifetimePlans
planContractLifetimesAfterConstantFolding(const ContractNode& contract);

} // namespace apc::compiler

#endif // AST_LIFETIME_PLANNER_H
