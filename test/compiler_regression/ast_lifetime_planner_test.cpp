#include "compiler/ast_lifetime_planner.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace apc::compiler;

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "ast_lifetime_planner_test: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

void requireValid(const AstLifetimePlan& plan, const std::string& message)
{
    if (plan.valid()) {
        return;
    }
    std::cerr << message << '\n';
    for (const auto& issue : plan.issues) {
        std::cerr << "  planner: " << issue.message << '\n';
    }
    for (const auto& issue : plan.analysis.issues) {
        std::cerr << "  lifetime: " << issue.message << " (block "
                  << issue.block.value() << ", statement "
                  << issue.statementIndex << ")\n";
    }
    fail(message);
}

std::unique_ptr<LiteralNode> number(const std::string& value)
{
    return std::make_unique<LiteralNode>(LiteralNode::Type::Number, value);
}

std::unique_ptr<IdentifierNode> identifier(const std::string& name)
{
    return std::make_unique<IdentifierNode>(name);
}

bool scheduleContains(
    const AstLifetimePlan& plan,
    const StmtNode* statement,
    const std::string& name
)
{
    for (const auto& suggestion : plan.suggestionsAfter(statement)) {
        for (const auto& binding : suggestion.bindings) {
            if (binding.name == name) {
                return true;
            }
        }
    }
    return false;
}

const AstValueRecord* parameterRecord(
    const AstLifetimePlan& plan,
    const std::string& name
)
{
    for (const auto& record : plan.valueRecords) {
        if (record.name == name && record.parameter) {
            return &record;
        }
    }
    return nullptr;
}

void testLexicalShadowingUsesStableBindings()
{
    auto body = std::make_unique<BlockNode>();
    auto nested = std::make_unique<BlockNode>();
    auto declaration = std::make_unique<VarDeclNode>(
        "x", "int", number("7")
    );
    const VarDeclNode* declarationPtr = declaration.get();
    nested->statements.push_back(std::move(declaration));
    auto innerUse =
        std::make_unique<ExprStmtNode>(identifier("x"));
    const ExprStmtNode* innerUsePtr = innerUse.get();
    nested->statements.push_back(std::move(innerUse));
    body->statements.push_back(std::move(nested));

    auto outerUse =
        std::make_unique<ExprStmtNode>(identifier("x"));
    const ExprStmtNode* outerUsePtr = outerUse.get();
    body->statements.push_back(std::move(outerUse));

    FunctionNode function(
        "main", {ParameterInfo{"x", "int"}}, std::move(body)
    );
    const AstLifetimePlan plan =
        AstLifetimePlanner().planAfterConstantFolding(function);

    requireValid(plan, "shadowing plan should be valid");
    const auto records = plan.recordsForName("x");
    require(records.size() == 2, "shadowing should create two x bindings");
    require(records[0]->binding != records[1]->binding,
        "shadowed names reused a BindingId");
    require(records[0]->value != records[1]->value,
        "shadowed names reused a ValueId");
    require(records[0]->atom != records[1]->atom,
        "shadowed names reused an AtomId");
    require(records[1]->definition == declarationPtr,
        "inner binding lost its declaration anchor");
    require(scheduleContains(plan, innerUsePtr, "x"),
        "inner binding was not reclaimed after its last use");
    require(scheduleContains(plan, outerUsePtr, "x"),
        "outer binding was not restored after lexical scope exit");
}

void testIfOneArmUsePreservesMayLiveness()
{
    auto body = std::make_unique<BlockNode>();
    auto branch = std::make_unique<IfNode>();
    branch->condition = identifier("condition");
    auto thenUse =
        std::make_unique<ExprStmtNode>(identifier("x"));
    const ExprStmtNode* thenUsePtr = thenUse.get();
    branch->thenBranch = std::move(thenUse);
    branch->elseBranch = std::make_unique<BlockNode>();
    const IfNode* branchPtr = branch.get();
    body->statements.push_back(std::move(branch));

    FunctionNode function(
        "main",
        {ParameterInfo{"condition", "bool"}, ParameterInfo{"x", "int"}},
        std::move(body)
    );
    const AstLifetimePlan plan =
        AstLifetimePlanner().planAfterConstantFolding(function);

    requireValid(plan, "if plan should be valid");
    require(scheduleContains(plan, thenUsePtr, "x"),
        "then-arm last use should have a path-local cleanup point");
    require(!scheduleContains(plan, branchPtr, "x"),
        "condition block reclaimed x before the then arm");
    require(scheduleContains(plan, branchPtr, "condition"),
        "condition should be reclaimable after the whole if statement");

    const AstValueRecord* x = parameterRecord(plan, "x");
    require(x != nullptr, "missing x parameter identity");
    const BlockLiveness* entry = plan.analysis.findBlock(plan.cfg.entry);
    require(entry && entry->liveOut.contains(x->value),
        "branch successor union did not keep x may-live");
}

void testLoopBackedgeAndSummaryEscape()
{
    auto body = std::make_unique<BlockNode>();
    auto loop = std::make_unique<ForNode>();
    loop->target = "i";
    std::vector<std::unique_ptr<ExprNode>> rangeArguments;
    rangeArguments.push_back(number("0"));
    rangeArguments.push_back(number("3"));
    auto range = std::make_unique<CallNode>(
        "Range", std::move(rangeArguments)
    );
    range->isRangeCall = true;
    loop->iterable = std::move(range);
    loop->body = std::make_unique<BlockNode>();
    auto use = std::make_unique<ExprStmtNode>(identifier("x"));
    const ExprStmtNode* usePtr = use.get();
    loop->body->statements.push_back(std::move(use));
    const ForNode* loopPtr = loop.get();
    body->statements.push_back(std::move(loop));

    FunctionNode function(
        "main", {ParameterInfo{"x", "int"}}, std::move(body)
    );
    const AstLifetimePlan plan =
        AstLifetimePlanner().planAfterConstantFolding(function);

    requireValid(plan, "loop plan should be valid");
    const auto loopContext = plan.loops.find(loopPtr);
    require(loopContext != plan.loops.end() &&
                loopContext->second.iteration.valid() &&
                loopContext->second.summarized,
        "loop lacks a stable summary LoopIterationId");

    bool hasBackedge = false;
    for (const auto& block : plan.cfg.blocks) {
        for (BasicBlockId successor : block.successors) {
            hasBackedge = hasBackedge || successor.value() < block.id.value();
        }
    }
    require(hasBackedge, "loop CFG has no backedge");

    const AstValueRecord* x = parameterRecord(plan, "x");
    require(x != nullptr && plan.analysis.escapedAtoms.contains(x->atom),
        "value crossing the summary loop was not escaped");
    require(!scheduleContains(plan, usePtr, "x"),
        "escaped loop-carried value received a cleanup suggestion");

    const auto iterationRecords = plan.recordsForName("i");
    require(iterationRecords.size() == 1 &&
                iterationRecords[0]->context.loopIteration ==
                    loopContext->second.iteration,
        "loop target lost its LoopIterationId context");
}

void testDeleteConsumesWithoutSchedulingCleanup()
{
    auto body = std::make_unique<BlockNode>();
    std::vector<std::unique_ptr<ExprNode>> arguments;
    arguments.push_back(identifier("x"));
    auto deletion = std::make_unique<ExprStmtNode>(
        std::make_unique<CallNode>("Delete", std::move(arguments))
    );
    const ExprStmtNode* deletionPtr = deletion.get();
    body->statements.push_back(std::move(deletion));

    FunctionNode function(
        "main", {ParameterInfo{"x", "int"}}, std::move(body)
    );
    const AstLifetimePlan plan =
        AstLifetimePlanner().planAfterConstantFolding(function);

    requireValid(plan, "Delete plan should be valid");
    const AstValueRecord* x = parameterRecord(plan, "x");
    require(x != nullptr, "missing Delete parameter identity");
    bool foundConsumedKill = false;
    bool foundLvalueConsume = false;
    for (const auto& block : plan.cfg.blocks) {
        for (const auto& statement : block.statements) {
            for (const auto& use : statement.uses) {
                if (use.value == x->value &&
                    use.access == AccessKind::LValue &&
                    use.ownership == OwnershipEffect::Consume) {
                    foundLvalueConsume = true;
                }
            }
        }
    }
    for (const auto& kill : plan.analysis.kills) {
        if (kill.atom == x->atom && kill.reason == KillReason::Consumed &&
            !kill.requiresCleanup) {
            foundConsumedKill = true;
        }
    }
    require(foundConsumedKill,
        "Delete should be represented as a physically satisfied consume");
    require(foundLvalueConsume,
        "Delete must be both an lvalue access and a consume effect");
    require(plan.suggestionsAfter(deletionPtr).empty(),
        "Delete incorrectly requested a second cleanup");
}

void testAliasAndKeepEscapeShareStorageIdentity()
{
    auto body = std::make_unique<BlockNode>();
    auto alias = std::make_unique<VarDeclNode>(
        "alias", "int", identifier("source")
    );
    body->statements.push_back(std::move(alias));

    std::vector<std::unique_ptr<ExprNode>> arguments;
    arguments.push_back(identifier("alias"));
    auto keep = std::make_unique<ExprStmtNode>(
        std::make_unique<CallNode>("Keep", std::move(arguments))
    );
    const ExprStmtNode* keepPtr = keep.get();
    body->statements.push_back(std::move(keep));

    FunctionNode function(
        "main", {ParameterInfo{"source", "int"}}, std::move(body)
    );
    const AstLifetimePlan plan =
        AstLifetimePlanner().planAfterConstantFolding(function);

    requireValid(plan, "alias escape plan should be valid");
    const AstValueRecord* source = parameterRecord(plan, "source");
    const auto aliases = plan.recordsForName("alias");
    require(source && aliases.size() == 1 && aliases[0]->alias,
        "direct identifier initializer was not recorded as an alias");
    require(source->atom == aliases[0]->atom &&
                plan.values.sharesAtom(source->value, aliases[0]->value),
        "alias did not share its source AtomId");
    require(plan.analysis.escapedAtoms.contains(source->atom),
        "Keep did not conservatively escape the aliased atom");
    require(plan.suggestionsAfter(keepPtr).empty(),
        "escaped alias received a cleanup suggestion");
}

void testDynamicIndexAndPrivateCallContexts()
{
    ContractNode contract("C");

    auto helperBody = std::make_unique<BlockNode>();
    auto helper = std::make_unique<FunctionNode>(
        "helper",
        std::vector<ParameterInfo>{ParameterInfo{"value", "int"}},
        std::move(helperBody)
    );
    contract.members.push_back(std::move(helper));

    auto mainBody = std::make_unique<BlockNode>();
    auto dynamicRead = std::make_unique<ExprStmtNode>(
        std::make_unique<IndexAccessNode>(identifier("array"), identifier("i"))
    );
    mainBody->statements.push_back(std::move(dynamicRead));

    std::vector<std::unique_ptr<ExprNode>> callArguments;
    callArguments.push_back(identifier("array"));
    auto call = std::make_unique<CallNode>(
        "helper", std::move(callArguments)
    );
    const CallNode* callPtr = call.get();
    mainBody->statements.push_back(
        std::make_unique<ExprStmtNode>(std::move(call))
    );

    auto mainFunction = std::make_unique<FunctionNode>(
        "main",
        std::vector<ParameterInfo>{
            ParameterInfo{"array", "int[4]"}, ParameterInfo{"i", "int"}
        },
        std::move(mainBody)
    );
    const FunctionNode* mainPtr = mainFunction.get();
    contract.members.push_back(std::move(mainFunction));

    const AstLifetimePlan plan =
        AstLifetimePlanner(&contract).planAfterConstantFolding(*mainPtr);
    requireValid(plan, "dynamic-index/call plan should be valid");

    const AstValueRecord* array = parameterRecord(plan, "array");
    require(array && plan.analysis.escapedAtoms.contains(array->atom),
        "dynamic index did not escape the aggregate atom");

    const auto context = plan.inlineCalls.find(callPtr);
    require(context != plan.inlineCalls.end() &&
                context->second.callSite.valid() &&
                context->second.inlineFrame.valid() &&
                context->second.callee != nullptr &&
                context->second.conservativelyEscaped,
        "private call lacks stable conservative inline context");
}

void testAssignmentDefinitionsAndStaticReferenceKinds()
{
    auto body = std::make_unique<BlockNode>();
    auto assignment = std::make_unique<AssignNode>(
        identifier("x"), number("9")
    );
    const AssignNode* assignmentPtr = assignment.get();
    body->statements.push_back(std::move(assignment));

    auto fieldRead = std::make_unique<ExprStmtNode>(
        std::make_unique<FieldAccessNode>(identifier("record"), "field")
    );
    body->statements.push_back(std::move(fieldRead));
    auto indexRead = std::make_unique<ExprStmtNode>(
        std::make_unique<IndexAccessNode>(identifier("array"), number("0"))
    );
    body->statements.push_back(std::move(indexRead));
    auto xUse = std::make_unique<ExprStmtNode>(identifier("x"));
    body->statements.push_back(std::move(xUse));

    FunctionNode function(
        "main",
        {ParameterInfo{"x", "int"},
         ParameterInfo{"record", "Record"},
         ParameterInfo{"array", "int[2]"}},
        std::move(body)
    );
    const AstLifetimePlan plan =
        AstLifetimePlanner().planAfterConstantFolding(function);
    requireValid(
        plan, "assignment/static-reference plan should be valid"
    );

    const auto xRecords = plan.recordsForName("x");
    require(xRecords.size() == 2 &&
                xRecords[0]->binding == xRecords[1]->binding &&
                xRecords[0]->value != xRecords[1]->value &&
                xRecords[0]->atom != xRecords[1]->atom &&
                xRecords[1]->definition == assignmentPtr,
        "Assign did not create a new value/atom for the stable binding");

    bool oldValueIsLvalue = false;
    bool assignedValueDefined = false;
    bool fieldBorrowed = false;
    bool staticIndexBorrowed = false;
    const AstValueRecord* record = parameterRecord(plan, "record");
    const AstValueRecord* array = parameterRecord(plan, "array");
    for (const auto& block : plan.cfg.blocks) {
        for (const auto& statement : block.statements) {
            for (const auto& definition : statement.definitions) {
                assignedValueDefined = assignedValueDefined ||
                    definition.value == xRecords[1]->value;
            }
            for (const auto& use : statement.uses) {
                oldValueIsLvalue = oldValueIsLvalue ||
                    (use.value == xRecords[0]->value &&
                     use.access == AccessKind::LValue &&
                     use.ownership == OwnershipEffect::None);
                fieldBorrowed = fieldBorrowed ||
                    (record && use.value == record->value &&
                     use.access == AccessKind::Borrow &&
                     use.ownership == OwnershipEffect::None);
                staticIndexBorrowed = staticIndexBorrowed ||
                    (array && use.value == array->value &&
                     use.access == AccessKind::Borrow &&
                     use.ownership == OwnershipEffect::None);
            }
        }
    }
    require(oldValueIsLvalue && assignedValueDefined,
        "Assign must contain an LValue access and a new definition");
    require(fieldBorrowed && staticIndexBorrowed,
        "field/static-index reads were not classified as Borrow");
}

void testBranchAssignmentsUseConservativeJoinSummary()
{
    auto body = std::make_unique<BlockNode>();
    auto branch = std::make_unique<IfNode>();
    branch->condition = identifier("condition");
    branch->thenBranch = std::make_unique<AssignNode>(
        identifier("x"), number("1")
    );
    branch->elseBranch = std::make_unique<AssignNode>(
        identifier("x"), number("2")
    );
    body->statements.push_back(std::move(branch));
    auto postJoinUse =
        std::make_unique<ExprStmtNode>(identifier("x"));
    const ExprStmtNode* postJoinUsePtr = postJoinUse.get();
    body->statements.push_back(std::move(postJoinUse));

    FunctionNode function(
        "main",
        {ParameterInfo{"condition", "bool"}, ParameterInfo{"x", "int"}},
        std::move(body)
    );
    const AstLifetimePlan plan =
        AstLifetimePlanner().planAfterConstantFolding(function);
    requireValid(plan, "branch-assignment plan should be valid");

    const auto xRecords = plan.recordsForName("x");
    require(xRecords.size() == 4,
        "two branch definitions and one join summary were not recorded");
    const AstValueRecord* summary = xRecords.back();
    require(plan.analysis.escapedAtoms.contains(xRecords[1]->atom) &&
                plan.analysis.escapedAtoms.contains(xRecords[2]->atom) &&
                plan.analysis.escapedAtoms.contains(summary->atom),
        "branch versions/join summary were not conservatively escaped");
    require(!scheduleContains(plan, postJoinUsePtr, "x"),
        "escaped join summary received a cleanup suggestion");
}

} // namespace

int main()
{
    testLexicalShadowingUsesStableBindings();
    testIfOneArmUsePreservesMayLiveness();
    testLoopBackedgeAndSummaryEscape();
    testDeleteConsumesWithoutSchedulingCleanup();
    testAliasAndKeepEscapeShareStorageIdentity();
    testDynamicIndexAndPrivateCallContexts();
    testAssignmentDefinitionsAndStaticReferenceKinds();
    testBranchAssignmentsUseConservativeJoinSummary();
    std::cout << "ast_lifetime_planner_test: all checks passed\n";
    return 0;
}
