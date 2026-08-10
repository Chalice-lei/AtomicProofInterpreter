#include "ast_to_bytecode_visitor.h"

#include <algorithm>
#include <climits>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "../bytecode/bytecode_builtin_function.h"
#include "../bytecode/bytecode_builtin_struct.h"
#include "../bytecode/bytecode_helper_fun.h"
#include "../bytecode/bytecode_operation_calcu.h"
#include "../bytecode/bytecode_operation_functions.h"
#include "../bytecode/stack_permutation_planner.h"
#include "../bytecode/type_validator.h"
#include "../util/compiler_placeholder.h"
#include "../util/defer.h"
#include "../util/type_utils.h"
#include "control_flow_analysis.h"
#include "static_integer_evaluator.h"

using namespace tbc;

namespace
{

class ScopeRollbackGuard
{
public:
    ScopeRollbackGuard(
        const std::shared_ptr<Scope>& scope,
        bool enabled
    )
        : m_scope(scope)
    {
        if (enabled && m_scope) {
            m_snapshot = m_scope->captureControlFlowState();
        }
    }

    ~ScopeRollbackGuard() noexcept
    {
        if (!m_snapshot.has_value() || m_committed || !m_scope) {
            return;
        }
        try {
            m_scope->restoreControlFlowState(m_snapshot.value());
        } catch (const std::exception& error) {
            LOG_ERROR("Failed to restore Delete() scope state: ", error.what());
        } catch (...) {
            LOG_ERROR("Failed to restore Delete() scope state");
        }
    }

    ScopeRollbackGuard(const ScopeRollbackGuard&) = delete;
    ScopeRollbackGuard& operator=(const ScopeRollbackGuard&) = delete;

    void commit() noexcept
    {
        m_committed = true;
    }

private:
    std::shared_ptr<Scope> m_scope;
    std::optional<ControlFlowStateSnapshot> m_snapshot;
    bool m_committed{false};
};

using compiler_flow::ControlFlowOutcomes;
using compiler_flow::controlFlowOutcomes;
using compiler_flow::sequenceControlFlowOutcomes;

std::string declaredSymbolType(
    const SymbolTable& state,
    const std::string& name,
    const std::string& fallback
)
{
    auto it = std::find_if(
        state.m_currentScope.rbegin(),
        state.m_currentScope.rend(),
        [&](const auto& entry) { return entry.first == name; }
    );
    if (it == state.m_currentScope.rend()) {
        return fallback;
    }

    const std::string declaredType = it->second.m_stackElement.getType();
    return declaredType.empty() ? fallback : declaredType;
}

bool compatibleStackTypes(const std::string& lhs, const std::string& rhs)
{
    if (lhs.empty() || rhs.empty() || lhs == rhs) {
        return true;
    }
    auto isNumeric = [](const std::string& type) {
        return type == "num" || type == "number" || type == "int" ||
               type == "uint64";
    };
    return isNumeric(lhs) && isNumeric(rhs);
}

enum class StorageLocation
{
    Main,
    Alt,
    Unknown
};

StorageLocation storageAfterStatement(
    const StmtNode* stmt,
    const std::string& symbol,
    StorageLocation initial
)
{
    if (!stmt) {
        return initial;
    }

    if (auto exprStmt = dynamic_cast<const ExprStmtNode*>(stmt)) {
        auto call = dynamic_cast<const CallNode*>(exprStmt->expr.get());
        if (!call || call->args.empty()) {
            return initial;
        }
        auto identifier =
            dynamic_cast<const IdentifierNode*>(call->args.front().get());
        if (!identifier || identifier->name != symbol) {
            return initial;
        }
        if (call->funcName == "SetAlt") {
            return StorageLocation::Alt;
        }
        if (call->funcName == "SetMain") {
            return StorageLocation::Main;
        }
        return initial;
    }

    if (auto block = dynamic_cast<const BlockNode*>(stmt)) {
        StorageLocation current = initial;
        for (const auto& inner : block->statements) {
            current = storageAfterStatement(inner.get(), symbol, current);
        }
        return current;
    }

    if (auto ifNode = dynamic_cast<const IfNode*>(stmt)) {
        const StorageLocation thenLocation = storageAfterStatement(
            ifNode->thenBranch.get(), symbol, initial
        );
        const StorageLocation elseLocation = storageAfterStatement(
            ifNode->elseBranch.get(), symbol, initial
        );
        return thenLocation == elseLocation ? thenLocation
                                            : StorageLocation::Unknown;
    }

    if (auto forNode = dynamic_cast<const ForNode*>(stmt)) {
        // The iteration count may depend on an outer fixed binding. Preserve
        // the possible zero-iteration path in this context-free prediction.
        const StorageLocation afterBody =
            storageAfterStatement(forNode->body.get(), symbol, initial);
        return afterBody == initial ? initial : StorageLocation::Unknown;
    }

    return initial;
}

std::optional<size_t> inlineReturnArity(const StmtNode* stmt)
{
    if (!stmt) {
        return std::nullopt;
    }
    if (auto returnNode = dynamic_cast<const ReturnNode*>(stmt)) {
        if (!returnNode->isValueReturn || !returnNode->expr) {
            return std::nullopt;
        }
        if (auto brace =
                dynamic_cast<const BraceExprNode*>(returnNode->expr.get())) {
            return brace->elements.size();
        }
        return size_t{1};
    }
    if (auto block = dynamic_cast<const BlockNode*>(stmt)) {
        for (const auto& inner : block->statements) {
            if (auto arity = inlineReturnArity(inner.get()); arity.has_value()) {
                return arity;
            }
        }
        return std::nullopt;
    }
    if (auto ifNode = dynamic_cast<const IfNode*>(stmt)) {
        auto thenArity = inlineReturnArity(ifNode->thenBranch.get());
        auto elseArity = inlineReturnArity(ifNode->elseBranch.get());
        if (thenArity.has_value() && elseArity.has_value() &&
            thenArity.value() != elseArity.value()) {
            return std::nullopt;
        }
        return thenArity.has_value() ? thenArity : elseArity;
    }
    if (auto forNode = dynamic_cast<const ForNode*>(stmt)) {
        return inlineReturnArity(forNode->body.get());
    }
    return std::nullopt;
}

bool isImmutableSuffixStatement(const StmtNode* statement)
{
    const auto* expressionStatement =
        dynamic_cast<const ExprStmtNode*>(statement);
    if (!expressionStatement || !expressionStatement->expr) {
        return false;
    }

    const auto* call =
        dynamic_cast<const CallNode*>(expressionStatement->expr.get());
    return call && call->funcName == "Push";
}

bool isDirectScriptReturn(const StmtNode* statement)
{
    const auto* returnStatement =
        dynamic_cast<const ReturnNode*>(statement);
    return returnStatement && !returnStatement->isValueReturn;
}

size_t encodedScriptByteSize(const std::string& encoding)
{
    size_t offset = 0;
    if (encoding.size() >= 2 && encoding[0] == '0' &&
        (encoding[1] == 'x' || encoding[1] == 'X')) {
        offset = 2;
    }
    return (encoding.size() - offset) / 2;
}

size_t rollByteCost(size_t depth)
{
    if (depth == 0) {
        return 0;
    }
    if (depth <= 2) {
        return 1;
    }
    return encodedScriptByteSize(
               numberToScriptHex(static_cast<int64_t>(depth))
           ) +
           1;
}

struct ArgumentLayoutAnalysis
{
    std::vector<size_t> argumentSlots;
    size_t legacyBytes{0};
    std::optional<StackPermutationPlan> strictMovePlan;

    size_t selectedBytes() const
    {
        return strictMovePlan.has_value() ? strictMovePlan->serializedBytes
                                          : legacyBytes;
    }
};

std::optional<ArgumentLayoutAnalysis> analyzeArgumentLayout(
    Scope& scope,
    const std::vector<StackElement>& arguments
)
{
    if (arguments.empty()) {
        return ArgumentLayoutAnalysis{};
    }

    ArgumentLayoutAnalysis analysis;
    analysis.argumentSlots.reserve(arguments.size());
    for (auto argument : arguments) {
        if (isScript(argument.getName())) {
            return std::nullopt;
        }
        auto position = scope.getPos(argument);
        if (!position.has_value() || position.value() < 0) {
            return std::nullopt;
        }
        const size_t slot = static_cast<size_t>(position.value());
        if (std::find(
                analysis.argumentSlots.begin(),
                analysis.argumentSlots.end(),
                slot
            ) != analysis.argumentSlots.end()) {
            return std::nullopt;
        }
        analysis.argumentSlots.push_back(slot);
    }

    bool alreadyInOpcodeOrder = true;
    for (size_t i = 0; i < analysis.argumentSlots.size(); ++i) {
        if (analysis.argumentSlots[i] !=
            analysis.argumentSlots.size() - 1 - i) {
            alreadyInOpcodeOrder = false;
            break;
        }
    }
    if (alreadyInOpcodeOrder) {
        analysis.legacyBytes = 0;
    } else if (analysis.argumentSlots.size() == 2 &&
               ((analysis.argumentSlots[0] == 3 &&
                 analysis.argumentSlots[1] == 1) ||
                (analysis.argumentSlots[0] == 5 &&
                 analysis.argumentSlots[1] == 3))) {
        // Mirror the two pair shortcuts in adjustStackToMatch().
        analysis.legacyBytes = 1;
    } else {
        std::vector<size_t> remaining(scope.size());
        for (size_t i = 0; i < remaining.size(); ++i) {
            remaining[i] = i;
        }

        for (size_t index = 0; index < analysis.argumentSlots.size(); ++index) {
            auto found = std::find(
                remaining.begin(),
                remaining.end(),
                analysis.argumentSlots[index]
            );
            if (found == remaining.end()) {
                return std::nullopt;
            }
            const size_t symbolicPosition =
                static_cast<size_t>(std::distance(remaining.begin(), found));
            analysis.legacyBytes += rollByteCost(symbolicPosition + index);
            remaining.erase(found);
        }
    }

    const size_t windowSize =
        *std::max_element(
            analysis.argumentSlots.begin(), analysis.argumentSlots.end()
        ) +
        1;
    if (windowSize > StackPermutationPlanner::MAX_MOVE_WINDOW ||
        analysis.legacyBytes == 0) {
        return analysis;
    }

    // Physical depths are stable, call-local slot ids.  Names are deliberately
    // excluded because StackElement equality is name-based and may alias.
    std::vector<uint64_t> currentTopFirst;
    currentTopFirst.reserve(windowSize);
    for (size_t depth = 0; depth < windowSize; ++depth) {
        currentTopFirst.push_back(static_cast<uint64_t>(depth));
    }

    std::vector<uint64_t> targetTopFirst;
    targetTopFirst.reserve(windowSize);
    std::vector<bool> isArgumentSlot(windowSize, false);
    for (auto slot = analysis.argumentSlots.rbegin();
         slot != analysis.argumentSlots.rend();
         ++slot) {
        targetTopFirst.push_back(static_cast<uint64_t>(*slot));
        isArgumentSlot[*slot] = true;
    }
    for (size_t depth = 0; depth < windowSize; ++depth) {
        if (!isArgumentSlot[depth]) {
            targetTopFirst.push_back(static_cast<uint64_t>(depth));
        }
    }

    auto movePlan = StackPermutationPlanner::planMoveOnly(
        currentTopFirst, targetTopFirst
    );
    if (movePlan.has_value() &&
        movePlan->serializedBytes < analysis.legacyBytes) {
        analysis.strictMovePlan = std::move(movePlan);
    }
    return analysis;
}

std::optional<size_t> estimateArgumentLayoutCost(
    Scope& scope,
    const std::vector<StackElement>& arguments
)
{
    const auto analysis = analyzeArgumentLayout(scope, arguments);
    if (!analysis.has_value()) {
        return std::nullopt;
    }
    return analysis->selectedBytes();
}

void emitStackPlan(
    BytecodeGenerator& generator,
    const StackPermutationPlan& plan
)
{
    for (const auto& instruction : plan.encodedInstructions()) {
        generator.emit(instruction);
    }
}

void applyMoveOnlyStep(Scope& scope, const StackPlanStep& step)
{
    switch (step.op) {
        case StackPlanOp::Swap:
            scope.roll(1);
            return;
        case StackPlanOp::Rot:
            scope.roll(2);
            return;
        case StackPlanOp::TwoSwap:
            scope.roll(3);
            scope.roll(3);
            return;
        case StackPlanOp::TwoRot:
            scope.roll(5);
            scope.roll(5);
            return;
        case StackPlanOp::Roll:
            scope.roll(static_cast<int>(step.depth));
            return;
        default:
            throw std::logic_error("non-move step in move-only stack plan");
    }
}

void emitAndApplyMoveOnlyPlan(
    BytecodeGenerator& generator,
    Scope& scope,
    const StackPermutationPlan& plan
)
{
    for (const auto& step : plan.steps) {
        applyMoveOnlyStep(scope, step);
        for (const auto& instruction : step.encodedInstructions()) {
            generator.emit(instruction);
        }
    }
}

StackPermutationPlan makeStackPlan(std::vector<StackPlanStep> steps)
{
    StackPermutationPlan plan;
    plan.steps = std::move(steps);
    for (const auto& step : plan.steps) {
        plan.serializedBytes += step.serializedByteSize();
        plan.emittedInstructions += step.emittedInstructionCount();
        plan.maximumTouchedDepth =
            std::max(plan.maximumTouchedDepth, step.touchedDepth());
    }
    return plan;
}

StackPermutationPlan buildLegacyCopyAssignmentPlan(
    size_t sourceDepth,
    size_t targetDepth
)
{
    if (sourceDepth == targetDepth) {
        throw std::invalid_argument("copy assignment requires distinct slots");
    }

    std::vector<StackPlanStep> steps;
    if (targetDepth < sourceDepth) {
        for (size_t i = 0; i < targetDepth; ++i) {
            steps.push_back({StackPlanOp::ToAltStack, 0});
        }
        steps.push_back({StackPlanOp::Drop, 0});
        steps.push_back(
            {StackPlanOp::Pick, sourceDepth - targetDepth - 1}
        );
        for (size_t i = 0; i < targetDepth; ++i) {
            steps.push_back({StackPlanOp::FromAltStack, 0});
        }
    } else if (sourceDepth == 0 && targetDepth == 1) {
        steps.push_back({StackPlanOp::Nip, 0});
        steps.push_back({StackPlanOp::Dup, 0});
    } else {
        steps.push_back({StackPlanOp::Pick, sourceDepth});
        for (size_t i = 0; i < targetDepth; ++i) {
            steps.push_back({StackPlanOp::Swap, 1});
            steps.push_back({StackPlanOp::ToAltStack, 0});
        }
        steps.push_back({StackPlanOp::ToAltStack, 0});
        steps.push_back({StackPlanOp::Drop, 0});
        for (size_t i = 0; i <= targetDepth; ++i) {
            steps.push_back({StackPlanOp::FromAltStack, 0});
        }
    }
    return makeStackPlan(std::move(steps));
}

struct CopyAssignmentEmission
{
    size_t legacyBytes{0};
    size_t emittedBytes{0};
    bool usedPlanner{false};
};

CopyAssignmentEmission emitCopyAssignment(
    BytecodeGenerator& generator,
    size_t sourceDepth,
    size_t targetDepth
)
{
    const size_t legacyBytes =
        StackPermutationPlanner::legacyCopyAssignmentByteCost(
            sourceDepth, targetDepth
        );
    auto chosenPlan = StackPermutationPlanner::planCopyAssignment(
        sourceDepth, targetDepth, legacyBytes
    );
    const bool usedPlanner = chosenPlan.has_value();
    if (!chosenPlan.has_value()) {
        chosenPlan =
            buildLegacyCopyAssignmentPlan(sourceDepth, targetDepth);
        if (chosenPlan->serializedBytes != legacyBytes) {
            throw std::logic_error(
                "legacy copy plan size disagrees with planner cost model"
            );
        }
    }
    emitStackPlan(generator, *chosenPlan);
    return {legacyBytes, chosenPlan->serializedBytes, usedPlanner};
}

class ScopedStringOverride
{
public:
    ScopedStringOverride(std::string& target, std::string replacement)
        : m_target(target), m_original(target)
    {
        m_target = std::move(replacement);
    }

    ~ScopedStringOverride()
    {
        m_target = std::move(m_original);
    }

    ScopedStringOverride(const ScopedStringOverride&) = delete;
    ScopedStringOverride& operator=(const ScopedStringOverride&) = delete;

private:
    std::string& m_target;
    std::string m_original;
};

} // namespace

void ASTToBytecodeVisitor::visit(ContractNode& node)
{
    LOG_DEBUG("Visiting contract node start. name: " + node.name);

    collectSelfPlaceholderLengths(node);

    const BlockNode* previousImmutableSuffixBlock = m_immutableSuffixBlock;
    m_immutableSuffixBlock = nullptr;
    DEFER_BLOCK(m_immutableSuffixBlock = previousImmutableSuffixBlock;);

    // Public functions are emitted in member order. Only the final emitted
    // function can append immutable bytes at the physical script end.
    for (auto it = node.members.rbegin(); it != node.members.rend(); ++it) {
        const auto* function = dynamic_cast<const FunctionNode*>(it->get());
        if (!function || function->fromLibrary || function->name.empty() ||
            function->name[0] == '_') {
            continue;
        }
        m_immutableSuffixBlock = function->block.get();
        break;
    }

    // 副栈可在同一合约的相邻 public 函数之间中继状态，但不能跨越
    // 独立合约/编译会话。函数级 clean 会保留它，合约入口统一清空。
    m_scopePtr->getCurrentSymtab().clearSharedAltStack();
    m_escapedAltArrays.clear();

#ifdef ENABLE_DEBUGGER
    if (m_debugInfoGen) {
        m_debugInfoGen->setContractName(node.name);
    }
#endif

    for (const auto& member : node.members) {
        member->accept(*this);
    }
#ifdef ENABLE_DEBUGGER
    if (m_debugInfoGen) {
        m_debugInfoGen->finalizeScopes();
    }
#endif
    LOG_DEBUG("Visiting contract node end. name: " + node.name);
}

void ASTToBytecodeVisitor::collectSelfPlaceholderLengths(
    const ContractNode& node
)
{
    m_selfPlaceholderLengths.clear();

    for (const auto& member : node.members) {
        const auto* constructor =
            dynamic_cast<const ConstructorNode*>(member.get());
        if (!constructor || !constructor->block) {
            continue;
        }

        std::unordered_map<std::string, std::string> paramTypes;
        for (const auto& param : constructor->parameters) {
            paramTypes[param.name] = param.type;
        }

        collectSelfPlaceholderLengthsFromStmt(
            constructor->block.get(), paramTypes
        );
    }
}

void ASTToBytecodeVisitor::collectSelfPlaceholderLengthsFromStmt(
    const StmtNode* stmt,
    const std::unordered_map<std::string, std::string>& paramTypes
)
{
    if (!stmt) {
        return;
    }

    if (const auto* block = dynamic_cast<const BlockNode*>(stmt)) {
        for (const auto& child : block->statements) {
            collectSelfPlaceholderLengthsFromStmt(child.get(), paramTypes);
        }
        return;
    }

    if (const auto* assign = dynamic_cast<const AssignNode*>(stmt)) {
        auto selfPath = extractSelfPath(assign->name.get());
        auto paramName = extractIdentifierName(assign->value.get());
        if (!selfPath.has_value() || !paramName.has_value()) {
            return;
        }

        auto typeIt = paramTypes.find(paramName.value());
        if (typeIt == paramTypes.end()) {
            return;
        }

        auto byteLength = fixedByteLengthFromType(typeIt->second);
        if (!byteLength.has_value()) {
            return;
        }

        m_selfPlaceholderLengths[selfPath.value()] = byteLength.value();
        LOG_DEBUG(
            "Registered self placeholder length: ",
            selfPath.value(),
            " -> ",
            byteLength.value(),
            " bytes"
        );
        return;
    }

    if (const auto* ifNode = dynamic_cast<const IfNode*>(stmt)) {
        collectSelfPlaceholderLengthsFromStmt(
            ifNode->thenBranch.get(), paramTypes
        );
        collectSelfPlaceholderLengthsFromStmt(
            ifNode->elseBranch.get(), paramTypes
        );
        return;
    }

    if (const auto* forNode = dynamic_cast<const ForNode*>(stmt)) {
        collectSelfPlaceholderLengthsFromStmt(forNode->body.get(), paramTypes);
    }
}

std::optional<std::string> ASTToBytecodeVisitor::extractSelfPath(
    const ExprNode* expr
)
{
    if (!expr) {
        return std::nullopt;
    }

    if (const auto* identifier = dynamic_cast<const IdentifierNode*>(expr)) {
        if (identifier->name == "self") {
            return std::string("self");
        }
        return std::nullopt;
    }

    if (const auto* field = dynamic_cast<const FieldAccessNode*>(expr)) {
        auto basePath = extractSelfPath(field->base.get());
        if (!basePath.has_value()) {
            return std::nullopt;
        }
        return basePath.value() + "." + field->field;
    }

    return std::nullopt;
}

std::optional<std::string> ASTToBytecodeVisitor::extractIdentifierName(
    const ExprNode* expr
)
{
    if (!expr) {
        return std::nullopt;
    }
    if (const auto* identifier = dynamic_cast<const IdentifierNode*>(expr)) {
        return identifier->name;
    }
    return std::nullopt;
}

std::optional<size_t> ASTToBytecodeVisitor::fixedByteLengthFromType(
    const std::string& type
)
{
    auto parseHexLength = [](const std::string& value)
        -> std::optional<size_t> {
        if (value.rfind("hex", 0) != 0 || value.size() <= 3) {
            return std::nullopt;
        }
        const std::string digits = value.substr(3);
        if (digits.empty() ||
            !std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            return std::nullopt;
        }
        try {
            const size_t parsed = std::stoull(digits);
            return parsed == 0 ? std::nullopt
                               : std::optional<size_t>(parsed);
        } catch (...) {
            return std::nullopt;
        }
    };

    if (auto arrayType = apc::util::parseFixedArrayType(type)) {
        const auto elementLength =
            arrayType->elementType == "uint64"
                ? std::optional<size_t>(8)
                : parseHexLength(arrayType->elementType);
        if (!elementLength.has_value() ||
            arrayType->size >
                std::numeric_limits<size_t>::max() /
                    elementLength.value()) {
            return std::nullopt;
        }
        return arrayType->size * elementLength.value();
    }

    if (auto hexLength = parseHexLength(type)) {
        return hexLength.value();
    }

    if (type == "uint64") {
        return 8;
    }
    if (type == "bool" || type == "boolean") {
        return 1;
    }
    if (type == "ripemd160" || type == "pubkeyhash" || type == "sha1" ||
        type == "address") {
        return 20;
    }
    if (type == "sha256" || type == "privkey") {
        return 32;
    }

    return std::nullopt;
}

std::string ASTToBytecodeVisitor::appendSelfPlaceholderLengths(
    const std::string& script,
    const ASTNode& node
) const
{
    auto startsWith = [](const std::string& value,
                         const std::string& prefix) {
        return value.size() >= prefix.size() &&
               value.compare(0, prefix.size(), prefix) == 0;
    };
    auto endsWith = [](const std::string& value,
                       const std::string& suffix) {
        return value.size() >= suffix.size() &&
               value.compare(
                   value.size() - suffix.size(), suffix.size(), suffix
               ) == 0;
    };

    std::string result;
    result.reserve(script.size() + 8);

    size_t i = 0;
    while (i < script.size()) {
        if (script[i] != '<') {
            result += script[i++];
            continue;
        }

        const size_t end = script.find('>', i + 1);
        if (end == std::string::npos) {
            result += script.substr(i);
            break;
        }

        std::string label = script.substr(i + 1, end - i - 1);
        if (!startsWith(label, "self.")) {
            result += script.substr(i, end - i + 1);
            i = end + 1;
            continue;
        }

        auto lengthIt = m_selfPlaceholderLengths.find(label);
        if (lengthIt == m_selfPlaceholderLengths.end()) {
            size_t digitStart = label.size();
            while (digitStart > 0 &&
                   std::isdigit(static_cast<unsigned char>(
                       label[digitStart - 1]
                   )) != 0) {
                --digitStart;
            }
            if (digitStart < label.size() && digitStart > 0) {
                const std::string baseLabel = label.substr(0, digitStart);
                auto baseIt = m_selfPlaceholderLengths.find(baseLabel);
                if (baseIt != m_selfPlaceholderLengths.end()) {
                    const std::string suffix = label.substr(digitStart);
                    try {
                        size_t parsedChars = 0;
                        const size_t suffixLength =
                            std::stoull(suffix, &parsedChars, 10);
                        if (parsedChars == suffix.size() &&
                            suffixLength == baseIt->second) {
                            lengthIt = baseIt;
                        }
                    } catch (...) {
                    }
                }
            }
        }

        if (lengthIt == m_selfPlaceholderLengths.end()) {
            if (m_isEmittingImmutableSuffix) {
                result += "<" + label + ">";
                i = end + 1;
                continue;
            }

            SourceLocation loc = getNodeLocation(node);
            SEMANTIC_ERROR(
                "placeholder '<" + label +
                    ">' appears in Push() but has no fixed byte length "
                    "declaration",
                loc,
                "Declare the constructor source parameter with a fixed-size "
                "type such as hex20 before using Push(self.x)"
            );
            LOG_ERROR(
                "Push placeholder missing fixed byte length: <", label, ">"
            );
            throw std::runtime_error(
                "Push placeholder missing fixed byte length: <" + label + ">"
            );
        }

        const std::string lengthSuffix = std::to_string(lengthIt->second);
        if (!endsWith(label, lengthSuffix)) {
            label += lengthSuffix;
        }
        result += "<" + label + ">";
        i = end + 1;
    }

    return result;
}

void ASTToBytecodeVisitor::visit(FunctionNode& node)
{
    LOG_DEBUG("Visiting function node start. name: " + node.name);
    if ((!node.name.empty() && node.name[0] == '_') || node.fromLibrary) {
        LOG_DEBUG(
            "Private function detected: " + node.name +
            (node.fromLibrary ? " (from library)" : "")
        );
        // 私有 + 库函数都进此表: 供后续内联调用且不污染 ABI.
        m_privateFunctions[node.name] = &node;
        return;
    }

    m_lastFlowResult = FlowResult::FallsThrough;

#ifdef ENABLE_DEBUGGER
    size_t startPC = m_generator.getCurrentPC();
    LOG_DEBUG("Function enter startPC: " + std::to_string(startPC));
    if (m_debugInfoGen) {
        apc_debug::SourceLocation loc = extractDebugLocation(node);
        LOG_DEBUG("Function enter location: " + loc.toString());
        bool isPublic = !node.name.empty() && node.name[0] != '_';
        m_debugInfoGen->onEnterFunction(node.name, isPublic, loc, startPC);

        for (const auto& param : node.parameters) {
            size_t paramIndex = &param - node.parameters.data();
            int stackOffset = static_cast<int>(
                node.parameters.size() - 1 - paramIndex
            );
            apc_debug::SourceLocation paramLoc = extractDebugLocation(node);
            m_debugInfoGen->onVariableDecl(
                param.name,
                param.type,
                paramLoc,
                true, // isStackVar
                stackOffset,
                true  // isParameter
            );
        }
    }
#endif

    m_scopePtr->clean();

    std::vector<ReturnNode*> returnNodes;
    if (node.block) {
        findAllReturnNodes(node.block.get(), returnNodes);
    }

    if (!returnNodes.empty()) {
        m_lastReturnNode = returnNodes.back();
        LOG_DEBUG(
            "Found " + std::to_string(returnNodes.size()) +
            " return statement(s) in function: " + node.name
        );
    } else {
        m_lastReturnNode = nullptr;
    }

    for (const auto& param : node.parameters) {
        const std::string& paramName = param.name;
        const std::string& paramType = param.type;

        LOG_DEBUG(
            "Processing function parameter: ",
            paramName,
            " of type: ",
            paramType
        );

        bool isArrayType = apc::util::isFixedArrayType(paramType);

        if (isArrayType) {
            LOG_DEBUG(
                "Parameter '",
                paramName,
                "' is an array type '",
                paramType,
                "', expanding..."
            );
            expandArrayParameter(
                paramName, paramType, m_structDefinitions, getNodeLocation(node)
            );
        } else if (m_structDefinitions.find(paramType) !=
                   m_structDefinitions.end()) {
            LOG_DEBUG(
                "Parameter '",
                paramName,
                "' is a struct of type '",
                paramType,
                "', expanding..."
            );
            expandStructParameter(paramName, paramType, m_structDefinitions);
        } else {
            LOG_DEBUG(
                "Parameter '", paramName, "' is a basic type '", paramType, "'"
            );
            m_scopePtr->defineSymbol(paramName, paramType);
            m_scopePtr->push(paramName, paramType, paramName);
            m_generator.emitUnlock("<" + paramName + ">");
        }
    }
    m_generator.emitUnlockName(node.name);
    m_generator.mergeSubUnoverall();

    std::string previousReturnType = m_currentFunctionReturnType;
    m_currentFunctionReturnType = node.returnType;
    const auto* previousLifetimePlan = m_currentLifetimePlan;
    m_currentLifetimePlan = lifetimePlanFor(node);
    DEFER_BLOCK(m_currentLifetimePlan = previousLifetimePlan;);
    if (node.block) {
        const BlockNode* previousPublicFunctionBlock = m_publicFunctionBlock;
        m_publicFunctionBlock = node.block.get();
        DEFER_BLOCK(m_publicFunctionBlock = previousPublicFunctionBlock;);
        node.block->accept(*this);
    }
    m_currentFunctionReturnType = previousReturnType;

#ifdef ENABLE_DEBUGGER
    size_t endPC = m_generator.getCurrentPC();
    LOG_DEBUG("Function exit endPC: " + std::to_string(endPC));
    if (m_debugInfoGen) {
        m_debugInfoGen->onExitFunction(endPC);
    }
#endif

    m_lastReturnNode = nullptr;
    m_lastFlowResult = FlowResult::FallsThrough;


    LOG_DEBUG("Visiting function node end. name: " + node.name);
}

void ASTToBytecodeVisitor::visit(BlockNode& node)
{
    DEFER([]() { LOG_DEBUG("Visiting block node end."); });
    LOG_DEBUG("Visiting block node start.");

#ifdef ENABLE_DEBUGGER
    std::shared_ptr<apc_debug::ScopeDebugInfo> debugScope;
    setCurrentLocationForGenerator(node);

    size_t startPC = m_generator.getCurrentPC();
    if (m_debugInfoGen) {
        apc_debug::SourceLocation loc = extractDebugLocation(node);
        debugScope = m_debugInfoGen->onEnterScope("block", loc, startPC);
    }

    // 调试作用域由 AST 块的生命周期决定，不能依赖是否产生运行时栈槽。
    // 守卫覆盖正常结束、提前 return 和异常展开，确保恰好退出一次。
    DEFER_BLOCK(
        if (debugScope && m_debugInfoGen) {
            m_debugInfoGen->onExitScope(
                debugScope, m_generator.getCurrentPC()
            );
        }
    );
#endif

    const SymbolTable blockEntryState = m_scopePtr->getCurrentSymtab();
    std::set<std::string> outerWholeArrays;
    for (const auto& [arrayName, unusedInfo] : m_wholeArrayElements) {
        (void)unusedInfo;
        outerWholeArrays.insert(arrayName);
    }

    m_scopePtr->enterScope();

    const bool preservesAltOutputs =
        m_publicFunctionBlock == &node || !m_activePrivateFunctions.empty();

    executeStatements(node.statements);

    SymbolTable& symbolTable = m_scopePtr->getCurrentSymtab();

    if (symbolTable.m_newSymbol.empty()) {
        LOG_DEBUG("No new symbols added");
        return;
    }
    LOG_INFO("When exiting the code scope, data defined within the scope needs "
             "to be handled.");
    std::string newSymbol;
    std::map<int, std::string> isAltStackMap;

    // 第一遍: 收集需清理的主栈变量及对应副栈变量.
    std::vector<std::string> mainStackSymbols;
    for (auto it : symbolTable.m_newSymbol) {
        LOG_DEBUG(it, " is new symbol");
        newSymbol += " " + it;

        // push 只表示当前块出现了新栈槽，不代表变量在当前块声明。
        // 外层变量重新绑定后也会产生新栈槽；它必须跨越当前块继续存活。
        std::string lookupName = it;
        if (symbolTable.symbolExists(lookupName) &&
            !symbolTable.isDeclaredInCurrentScope(it)) {
            LOG_DEBUG(
                it,
                " is an outer-scope symbol rebound in the current scope; "
                "skip cleanup"
            );
            continue;
        }

        // Splitting an outer fixed array inside a nested block creates
        // element stack labels such as amount[0x00]. They are new runtime
        // slots, but they still belong to the outer array and must survive
        // this block (notably across statically unrolled loop iterations).
        const size_t bracketPos = it.find('[');
        if (bracketPos != std::string::npos) {
            const std::string arrayName = it.substr(0, bracketPos);
            const bool belongedToOuterArray =
                blockEntryState.isArraySymbol(arrayName) ||
                outerWholeArrays.count(arrayName) > 0;
            if (belongedToOuterArray) {
                LOG_DEBUG(
                    it,
                    " belongs to outer-scope array ",
                    arrayName,
                    "; skip cleanup"
                );
                continue;
            }
        }

        bool isKeepSymtabFlag = false;
        for (auto keepIt : symbolTable.m_keepSymbol) {
            if (keepIt == it) {
                isKeepSymtabFlag = true;
                break;
            }
        }
        if (isKeepSymtabFlag) {
            continue;
        }
        auto stackPosOpt = symbolTable.getPos(it);
        if (!stackPosOpt.has_value()) {
            auto altStackPosOpt = symbolTable.getPos(it, true);
            if (!altStackPosOpt.has_value()) {
                SourceLocation loc("", 0, 0);
                std::string warningMsg = "cannot find location of variable '" +
                                         it + "' on stack during scope cleanup";
                COMPILER_WARNING(warningMsg, loc);
                LOG_WARNING(warningMsg);
            } else {
                if (preservesAltOutputs) {
                    LOG_DEBUG(
                        it,
                        " remains on alt stack as a public/private-function "
                        "handoff value"
                    );
                    continue;
                }
                isAltStackMap
                    .emplace(static_cast<int>(altStackPosOpt.value()), it);
            }
            continue;
        }
        mainStackSymbols.push_back(it);
    }

    // 按栈位置升序: 栈顶=0 优先, 消除 ROLL.
    std::sort(
        mainStackSymbols.begin(),
        mainStackSymbols.end(),
        [&](const std::string& a, const std::string& b) {
            auto posA = symbolTable.getPos(a);
            auto posB = symbolTable.getPos(b);
            if (!posA.has_value())
                return false;
            if (!posB.has_value())
                return true;
            return posA.value() < posB.value();
        }
    );

    // 第二遍: 按序生成清理, 用 OP_NIP/OP_2DROP 减少移动.
    int pendingDrops = 0; // 待批量合并的连续 OP_DROP 数.
    auto flushPendingDrops = [&]() {
        while (pendingDrops >= 2) {
            m_generator.emit(tbc::BytOpcode::OP_2DROP);
            pendingDrops -= 2;
        }
        if (pendingDrops == 1) {
            m_generator.emit(tbc::BytOpcode::OP_DROP);
            pendingDrops = 0;
        }
    };

    for (const auto& it : mainStackSymbols) {
        auto stackPosOpt = symbolTable.getPos(it);
        if (!stackPosOpt.has_value()) {
            continue;
        }
        auto pos = stackPosOpt.value();

        if (pos == STACK_TOP_POS) {
            // 栈顶: 累 DROP, 后续合并为 OP_2DROP.
            if (symbolTable.m_stackPtr && !symbolTable.m_stackPtr->empty()) {
                symbolTable.m_stackPtr->pop();
            }
            pendingDrops++;
        } else if (pos == 1) {
            // 次栈顶: OP_NIP 等价 [1]ROLL+DROP, 省 2 字节.
            flushPendingDrops();
            symbolTable.dropAt(1);
            m_generator.emit(tbc::BytOpcode::OP_NIP);
        } else {
            // 深处: ROLL+DROP (pos==2 自动 ROT+DROP).
            flushPendingDrops();
            symbolTable.roll(pos);
            emitRoll(pos);
            if (symbolTable.m_stackPtr && !symbolTable.m_stackPtr->empty()) {
                symbolTable.m_stackPtr->pop();
            }
            m_generator.emit(tbc::BytOpcode::OP_DROP);
        }
    }
    flushPendingDrops();

    std::string keepSymbol;
    for (auto keepIt : symbolTable.m_keepSymbol) {
        keepSymbol += " " + keepIt;
    }
    LOG_DEBUG("new symbol:", newSymbol);
    LOG_DEBUG("keep symbol:", keepSymbol);

    for (auto it : isAltStackMap) {
        auto altStackPosOpt = symbolTable.getPos(it.second, true);
        if (altStackPosOpt.has_value()) {
            auto num = symbolTable.setMain(it.second);

            for (int i = 0; i < num; i++) {
                m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
            }
            m_generator.emit(tbc::BytOpcode::OP_DROP);

            // setMain 已把 it.second 从副栈搬回主栈, drop 栈顶即可.
            if (symbolTable.m_stackPtr && !symbolTable.m_stackPtr->empty()) {
                symbolTable.m_stackPtr->pop();
            }

            for (int i = 0; i < num; i++) {
                m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                symbolTable.m_altStackPtr->moveTopToStack(
                    *symbolTable.m_stackPtr.get(),
                    true // 共享副栈下强制允许.
                );
            }
        }
    }

}

ASTToBytecodeVisitor::AltStackSnapshot
ASTToBytecodeVisitor::captureAltStack() const
{
    AltStackSnapshot snapshot;
    auto altStack = m_scopePtr->getCurrentSymtab().getSharedAltStack();
    if (altStack) {
        snapshot.elements = altStack->getStackContent();
        snapshot.combinedStackSize = altStack->getCombinedStackSize();
    }
    return snapshot;
}

void ASTToBytecodeVisitor::restoreAltStack(const AltStackSnapshot& snapshot)
{
    auto altStack = m_scopePtr->getCurrentSymtab().getSharedAltStack();
    altStack->replaceStackContent(snapshot.elements);
    altStack->setCombinedStackSize(snapshot.combinedStackSize);
}

std::optional<std::string> ASTToBytecodeVisitor::getAssignmentStorageName(
    const ExprNode* expr
) const
{
    if (!expr) {
        return std::nullopt;
    }

    if (auto identifier = dynamic_cast<const IdentifierNode*>(expr)) {
        return identifier->name;
    }

    if (auto field = dynamic_cast<const FieldAccessNode*>(expr)) {
        auto baseName = getAssignmentStorageName(field->base.get());
        if (baseName.has_value()) {
            return baseName.value() + "." + field->field;
        }
        return std::nullopt;
    }

    if (auto index = dynamic_cast<const IndexAccessNode*>(expr)) {
        auto baseName = getAssignmentStorageName(index->base.get());
        const auto indexValue = index->index
                                    ? resolveCompileTimeIndex(*index->index)
                                    : std::nullopt;
        if (!baseName.has_value() || !indexValue.has_value() ||
            indexValue.value() < 0) {
            return std::nullopt;
        }
        return baseName.value() + "[" +
               numberToScriptHex(indexValue.value()) + "]";
    }

    return std::nullopt;
}

void ASTToBytecodeVisitor::collectAssignedStorageNames(
    const StmtNode* stmt,
    std::vector<std::string>& names
) const
{
    if (!stmt) {
        return;
    }

    auto appendUnique = [&](const std::string& name) {
        if (!name.empty() &&
            std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    };

    if (auto exprStmt = dynamic_cast<const ExprStmtNode*>(stmt)) {
        auto call = dynamic_cast<const CallNode*>(exprStmt->expr.get());
        if (!call ||
            (call->funcName != "SetAlt" && call->funcName != "SetMain")) {
            return;
        }

        // SetAlt/SetMain 不产生新值，却会改变外层变量所在的运行时栈。
        // 它们必须像赋值一样参与 if 分支规范化，否则一条路径把 x
        // 留在 main、另一条路径把 x 留在 alt，后续生成的栈偏移会失真。
        for (const auto& arg : call->args) {
            auto storageName = getAssignmentStorageName(arg.get());
            if (storageName.has_value()) {
                appendUnique(storageName.value());
            }
        }
        return;
    }

    if (auto assign = dynamic_cast<const AssignNode*>(stmt)) {
        auto storageName = getAssignmentStorageName(assign->name.get());
        if (storageName.has_value()) {
            appendUnique(storageName.value());
        }
        return;
    }

    if (auto destructure = dynamic_cast<const DestructureAssignNode*>(stmt)) {
        for (const auto& target : destructure->targets) {
            appendUnique(target);
        }
        return;
    }

    if (auto block = dynamic_cast<const BlockNode*>(stmt)) {
        for (const auto& innerStmt : block->statements) {
            collectAssignedStorageNames(innerStmt.get(), names);
        }
        return;
    }

    if (auto ifNode = dynamic_cast<const IfNode*>(stmt)) {
        collectAssignedStorageNames(ifNode->thenBranch.get(), names);
        collectAssignedStorageNames(ifNode->elseBranch.get(), names);
        return;
    }

    if (auto forNode = dynamic_cast<const ForNode*>(stmt)) {
        appendUnique(forNode->target);
        collectAssignedStorageNames(forNode->body.get(), names);
    }
}

std::vector<std::string> ASTToBytecodeVisitor::collectIfMergeSymbols(
    const IfNode& node,
    const SymbolTable& entryState,
    const AltStackSnapshot& entryAltStack
) const
{
    std::vector<std::string> assignedNames;
    collectAssignedStorageNames(node.thenBranch.get(), assignedNames);
    collectAssignedStorageNames(node.elseBranch.get(), assignedNames);

    std::vector<std::string> candidates;
    auto appendCandidate = [&](const std::string& name) {
        if (!name.empty() &&
            std::find(candidates.begin(), candidates.end(), name) ==
                candidates.end()) {
            candidates.push_back(name);
        }
    };

    for (const auto& [name, _] : entryState.m_currentScope) {
        appendCandidate(name);
    }
    if (entryState.m_fixedStackPtr) {
        for (const auto& element :
             entryState.m_fixedStackPtr->getStackContent()) {
            appendCandidate(element.getName());
        }
    }
    if (entryState.m_stackPtr) {
        for (const auto& element : entryState.m_stackPtr->getStackContent()) {
            appendCandidate(element.getName());
        }
    }
    for (const auto& element : entryAltStack.elements) {
        appendCandidate(element.getName());
    }

    auto isRepresented = [&](const std::string& name) {
        if (entryState.getPos(name).has_value()) {
            return true;
        }
        if (entryState.m_fixedStackPtr) {
            for (const auto& element :
                 entryState.m_fixedStackPtr->getStackContent()) {
                if (element.getName() == name) {
                    return true;
                }
            }
        }
        return std::any_of(
            entryAltStack.elements.begin(),
            entryAltStack.elements.end(),
            [&](const StackElement& element) {
                return element.getName() == name;
            }
        );
    };

    auto matchesAssignedTarget = [&](const std::string& candidate) {
        return std::any_of(
            assignedNames.begin(),
            assignedNames.end(),
            [&](const std::string& target) {
                return candidate == target ||
                       candidate.starts_with(target + ".") ||
                       candidate.starts_with(target + "[");
            }
        );
    };

    std::vector<std::string> mergeSymbols;
    for (const auto& candidate : candidates) {
        if (!matchesAssignedTarget(candidate)) {
            continue;
        }

        if (!isRepresented(candidate)) {
            // 复合变量自身可能只承载元数据，实际值存放在其字段槽中。
            const bool hasRepresentedChild = std::any_of(
                candidates.begin(),
                candidates.end(),
                [&](const std::string& other) {
                    return other != candidate && isRepresented(other) &&
                           (other.starts_with(candidate + ".") ||
                            other.starts_with(candidate + "["));
                }
            );
            if (hasRepresentedChild) {
                continue;
            }
        }

        mergeSymbols.push_back(candidate);
    }

    return mergeSymbols;
}

void ASTToBytecodeVisitor::materializeBranchSymbols(
    const std::vector<std::string>& symbols,
    const IfNode& node,
    const AltStackSnapshot& desiredAltStack
)
{
    for (const auto& symbol : symbols) {
        SymbolTable& state = m_scopePtr->getCurrentSymtab();
        const bool keepOnAlt = std::any_of(
            desiredAltStack.elements.begin(),
            desiredAltStack.elements.end(),
            [&](const StackElement& element) {
                return element.getName() == symbol;
            }
        );
        auto normalizeType = [&](StackElement& element) {
            element.setType(
                declaredSymbolType(state, symbol, element.getType())
            );
        };

        auto mainPos = m_scopePtr->getPos(symbol);
        if (mainPos.has_value()) {
            normalizeType(m_scopePtr->stacktop(mainPos.value()));
            if (keepOnAlt) {
                std::string mutableSymbol = symbol;
                const int32_t position =
                    m_scopePtr->setAlt(mutableSymbol, true);
                if (position != STACK_TOP_POS) {
                    emitRoll(position);
                }
                m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                continue;
            }
            if (mainPos.value() != STACK_TOP_POS) {
                emitRoll(mainPos.value());
                m_scopePtr->roll(mainPos.value());
            }
            continue;
        }

        std::string mutableSymbol = symbol;
        auto fixedElement = m_scopePtr->getFixed(mutableSymbol);
        if (fixedElement.has_value()) {
            StackElement materialized = fixedElement.value();
            materialized.setName(symbol);
            normalizeType(materialized);
            m_generator.emit(fixedElement->getData());
            m_scopePtr->removeFixed(mutableSymbol);
            if (keepOnAlt) {
                m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                state.m_altStackPtr->push(materialized);
            } else {
                m_scopePtr->push(materialized);
            }
            continue;
        }

        auto altPos = m_scopePtr->getPos(symbol, true);
        if (!altPos.has_value()) {
            // The value may have been consumed on this branch. Defer the
            // decision until both branch layouts are available: consuming it
            // on both paths is valid, while a one-sided consume is rejected by
            // validateBranchMerge() as a real runtime-stack mismatch.
            LOG_DEBUG(
                "Merge candidate '",
                symbol,
                "' has no runtime value after branch at line ",
                node.pos.first,
                "; defer to branch-layout validation"
            );
            continue;
        }

        auto mainStack = state.m_stackPtr;
        auto altStack = state.m_altStackPtr;
        const int64_t position = altPos.value();

        normalizeType(altStack->stacktop(position));
        if (keepOnAlt) {
            continue;
        }

        // 先把目标及其上方元素全部移回主栈，目标此时位于主栈顶。
        for (int64_t i = 0; i <= position; ++i) {
            m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
            StackElement topElement = altStack->top();
            altStack->pop();
            mainStack->push(topElement);
        }

        // 将原本位于目标上方的元素按原顺序放回副栈，目标留在主栈顶。
        for (int64_t i = 0; i < position; ++i) {
            emitRoll(1);
            mainStack->swap(0, 1);
            m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
            StackElement topElement = mainStack->top();
            mainStack->pop();
            altStack->push(topElement);
        }
    }
}

ASTToBytecodeVisitor::FlowResult
ASTToBytecodeVisitor::statementFlow(const StmtNode* stmt) const
{
    const ControlFlowOutcomes outcomes = controlFlowOutcomes(stmt);
    if (outcomes.fallsThrough) {
        return FlowResult::FallsThrough;
    }
    return outcomes.inlineReturns ? FlowResult::InlineReturn
                                  : FlowResult::ScriptTerminate;
}

void ASTToBytecodeVisitor::validateBranchMerge(
    const IfNode& node,
    const SymbolTable& entryState,
    const SymbolTable& thenState,
    const AltStackSnapshot& thenAltStack,
    const SymbolTable& elseState,
    const AltStackSnapshot& elseAltStack,
    const std::vector<std::string>& mergeSymbols
) const
{
    auto fail = [&](const std::string& detail) {
        std::ostringstream oss;
        oss << "stack state inconsistency detected in if-else branches at line "
            << node.pos.first << ", column " << node.pos.second << ": "
            << detail;
        SourceLocation loc = getNodeLocation(node);
        SYNTAX_ERROR(
            oss.str(), loc, "Make both branches leave the same stack layout"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    };

    auto validateStorageState = [&fail](
        const SymbolTable& state,
        const AltStackSnapshot& altSnapshot,
        const char* branchName
    ) {
        auto contentSize = [](const std::vector<StackElement>& elements) {
            size_t total = 0;
            for (const auto& element : elements) {
                total += element.getMemoryUsage();
            }
            return total;
        };

        if (!state.m_stackPtr || !state.m_fixedStackPtr) {
            fail(std::string(branchName) + " branch contains a null stack");
        }
        if (state.m_stackPtr->getCombinedStackSize() !=
            contentSize(state.m_stackPtr->getStackContent())) {
            fail(
                std::string(branchName) +
                " branch main-stack memory accounting is inconsistent"
            );
        }
        if (state.m_fixedStackPtr->getCombinedStackSize() !=
            contentSize(state.m_fixedStackPtr->getStackContent())) {
            fail(
                std::string(branchName) +
                " branch fixed-stack memory accounting is inconsistent"
            );
        }
        if (altSnapshot.combinedStackSize !=
            contentSize(altSnapshot.elements)) {
            fail(
                std::string(branchName) +
                " branch alt-stack memory accounting is inconsistent"
            );
        }

        auto validateUniqueNames = [&fail, branchName](
            const std::vector<StackElement>& elements,
            std::set<std::string>& storageNames,
            const char* storageName
        ) {
            for (const auto& element : elements) {
                if (!element.getName().empty() &&
                    !storageNames.insert(element.getName()).second) {
                    fail(
                        std::string(branchName) + " branch stores symbol '" +
                        element.getName() + "' more than once in " +
                        storageName
                    );
                }
            }
        };
        std::set<std::string> runtimeNames;
        validateUniqueNames(
            state.m_stackPtr->getStackContent(),
            runtimeNames,
            "the runtime stacks"
        );
        validateUniqueNames(
            altSnapshot.elements, runtimeNames, "the runtime stacks"
        );

        // 重新绑定期间 fixed 与 runtime 可以暂时保留同名旧值，合并阶段
        // 会按新运行时值移除 fixed；fixed 自身仍不得出现重复槽。
        std::set<std::string> fixedNames;
        validateUniqueNames(
            state.m_fixedStackPtr->getStackContent(),
            fixedNames,
            "the fixed stack"
        );
    };

    validateStorageState(thenState, thenAltStack, "then");
    validateStorageState(elseState, elseAltStack, "else");

    auto sameLayout = [](
                          const std::vector<StackElement>& lhs,
                          const std::vector<std::string>& lhsKept,
                          const std::vector<StackElement>& rhs,
                          const std::vector<std::string>& rhsKept
                      ) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (size_t i = 0; i < lhs.size(); ++i) {
            const bool bothAreInlineReturns =
                std::find(lhsKept.begin(), lhsKept.end(), lhs[i].getName()) !=
                    lhsKept.end() &&
                std::find(rhsKept.begin(), rhsKept.end(), rhs[i].getName()) !=
                    rhsKept.end();
            if ((!bothAreInlineReturns &&
                 lhs[i].getName() != rhs[i].getName()) ||
                !compatibleStackTypes(
                    lhs[i].getType(), rhs[i].getType()
                )) {
                return false;
            }
        }
        return true;
    };

    const std::vector<StackElement> emptyStack;
    const auto& thenMain = thenState.m_stackPtr
                               ? thenState.m_stackPtr->getStackContent()
                               : emptyStack;
    const auto& elseMain = elseState.m_stackPtr
                               ? elseState.m_stackPtr->getStackContent()
                               : emptyStack;
    auto describeLayout = [](const std::vector<StackElement>& elements) {
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < elements.size(); ++i) {
            if (i != 0) {
                oss << ", ";
            }
            oss << elements[i].getName() << ":" << elements[i].getType();
        }
        oss << "]";
        return oss.str();
    };
    if (!sameLayout(
            thenMain,
            thenState.m_keepSymbol,
            elseMain,
            elseState.m_keepSymbol
        )) {
        fail(
            "main stack layouts differ after branch materialization: then=" +
            describeLayout(thenMain) + ", else=" + describeLayout(elseMain)
        );
    }
    if (!sameLayout(
            thenAltStack.elements,
            thenState.m_keepSymbol,
            elseAltStack.elements,
            elseState.m_keepSymbol
        )) {
        fail(
            "alternative stack layouts differ: then=" +
            describeLayout(thenAltStack.elements) +
            ", else=" + describeLayout(elseAltStack.elements)
        );
    }

    auto isMergeSymbol = [&](const std::string& name) {
        return std::find(mergeSymbols.begin(), mergeSymbols.end(), name) !=
               mergeSymbols.end();
    };
    auto findFixed = [](const SymbolTable& state, const std::string& name)
        -> std::optional<StackElement> {
        if (!state.m_fixedStackPtr) {
            return std::nullopt;
        }
        for (const auto& element : state.m_fixedStackPtr->getStackContent()) {
            if (element.getName() == name) {
                return element;
            }
        }
        return std::nullopt;
    };

    if (entryState.m_fixedStackPtr) {
        for (const auto& entryElement :
             entryState.m_fixedStackPtr->getStackContent()) {
            const std::string name = entryElement.getName();
            if (isMergeSymbol(name)) {
                continue;
            }
            auto thenElement = findFixed(thenState, name);
            auto elseElement = findFixed(elseState, name);
            if (!thenElement.has_value() || !elseElement.has_value() ||
                thenElement->getType() != elseElement->getType() ||
                thenElement->getData() != elseElement->getData()) {
                fail("fixed-area value for '" + name + "' differs");
            }
        }
    }
}

SymbolTable ASTToBytecodeVisitor::buildMergedBranchState(
    const SymbolTable& entryState,
    const SymbolTable& continuingState,
    const std::vector<std::string>& mergeSymbols
) const
{
    SymbolTable mergedState = continuingState;

    // 分支局部声明不能泄漏到父作用域；父层的归属和绑定元数据来自入口。
    mergedState.m_currentScope = entryState.m_currentScope;
    mergedState.m_declaredSymbols = entryState.m_declaredSymbols;
    mergedState.m_newSymbol = entryState.m_newSymbol;
    mergedState.m_keepSymbol = entryState.m_keepSymbol;
    mergedState.m_bindSymbol = entryState.m_bindSymbol;

    // 固定区同样从入口重建；分支合并变量已经物化到运行时主栈。
    mergedState.m_fixedStackPtr = std::make_shared<OpStack>();
    if (entryState.m_fixedStackPtr) {
        for (const auto& element :
             entryState.m_fixedStackPtr->getStackContent()) {
            mergedState.m_fixedStackPtr->push(element);
        }
    }

    auto appendNewSymbol = [&](const std::string& name) {
        if (std::find(
                mergedState.m_newSymbol.begin(),
                mergedState.m_newSymbol.end(),
                name
            ) == mergedState.m_newSymbol.end()) {
            mergedState.m_newSymbol.push_back(name);
        }
    };

    for (const auto& symbol : mergeSymbols) {
        std::string mutableSymbol = symbol;
        mergedState.removeFixed(mutableSymbol);
        if (mergedState.getPos(symbol).has_value()) {
            appendNewSymbol(symbol);
        }
        if (continuingState.isSymbolInitialized(symbol)) {
            mergedState.markSymbolInitialized(symbol);
        }
    }

    // lowercase return 产生的 keep 槽可能来自分支局部变量。虽然其声明
    // 不能泄漏到父作用域，返回槽本身必须跨越分支与外层块清理继续存活。
    for (const auto& kept : continuingState.m_keepSymbol) {
        const bool represented =
            continuingState.getPos(kept).has_value() ||
            continuingState.getPos(kept, true).has_value();
        if (represented &&
            std::find(
                mergedState.m_keepSymbol.begin(),
                mergedState.m_keepSymbol.end(),
                kept
            ) == mergedState.m_keepSymbol.end()) {
            mergedState.m_keepSymbol.push_back(kept);
        }
    }

    // 防止同一名字同时残留在固定区和主栈。
    if (mergedState.m_stackPtr) {
        for (const auto& element :
             mergedState.m_stackPtr->getStackContent()) {
            std::string name = element.getName();
            mergedState.removeFixed(name);
        }
    }

    return mergedState;
}

void ASTToBytecodeVisitor::visit(IfNode& node)
{
    DEFER([]() { LOG_DEBUG("Visiting if node end."); });
    LOG_DEBUG("Visiting if node start.");
    ++m_lifetimeControlFlowDepth;
    DEFER_BLOCK(--m_lifetimeControlFlowDepth;);

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    if (!node.thenBranch) {
        SourceLocation loc = getNodeLocation(node);
        LOG_ERROR(
            "Syntax error at line ",
            node.pos.first,
            ", column ",
            node.pos.second,
            " - if statement missing then branch"
        );
        SYNTAX_ERROR(
            "if statement missing then branch",
            loc,
            "Add a then branch after the condition"
        );
        return;
    }

    // Statically expanded Range targets and ordinary fixed numeric bindings
    // can make the condition constant. Emit only the reachable branch; a
    // false condition without else is an empty statement.
    if (node.condition) {
        auto compileTimeCondition =
            resolveCompileTimeCondition(*node.condition);
        if (compileTimeCondition.has_value()) {
            StmtNode* selectedBranch = compileTimeCondition.value() != 0
                                           ? node.thenBranch.get()
                                           : node.elseBranch.get();
            if (!selectedBranch) {
                m_lastFlowResult = FlowResult::FallsThrough;
                return;
            }

            m_lastFlowResult = FlowResult::FallsThrough;
            selectedBranch->accept(*this);
            const FlowResult selectedFlow = m_lastFlowResult;

            // BlockNode keeps its selected child scope active. Commit the
            // emitted state and discard only the saved entry snapshot.
            m_scopePtr->popScopeStack();
            m_lastFlowResult = selectedFlow;
            return;
        }
    }

    // 优化: if (a != b) 用 OP_EQUAL+OP_NOTIF 替 OP_EQUAL+OP_NOT+OP_IF (省 1 字节).
    auto* condOpNode = dynamic_cast<OpNode*>(node.condition.get());
    bool useNotIf = condOpNode && condOpNode->op == "!=" &&
                    condOpNode->lhs != nullptr;

    const auto conditionStorageName =
        getAssignmentStorageName(node.condition.get());

    if (useNotIf) {
        // 临时把 != 改 ==, 仅生成 OP_EQUAL. The scoped override restores
        // the shared AST even when lowering the condition throws.
        ScopedStringOverride emitAsEqual(condOpNode->op, "==");
        visitExpr(*node.condition);
    } else {
        visitExpr(*node.condition);
    }
    const auto conditionValue = m_scopePtr->pop();

    // Identifier/field/static-index visitors push a virtual reference while
    // leaving the real slot in place. OP_IF/OP_NOTIF consumes that real value
    // at runtime, so consume the corresponding compiler-stack slot as well.
    // Compound expressions already replace their operands with a temporary
    // result and therefore do not enter this path.
    if (conditionStorageName.has_value()) {
        auto conditionPos =
            m_scopePtr->getPos(conditionStorageName.value());
        if (conditionPos.has_value()) {
            if (conditionPos.value() != STACK_TOP_POS) {
                emitRoll(conditionPos.value());
                m_scopePtr->roll(conditionPos.value());
            }
            m_scopePtr->pop();
        } else if (conditionValue.has_value() &&
                   isScript(conditionValue->getName())) {
            const std::string& conditionData = conditionValue->getData();
            m_generator.emit(
                conditionData.empty() ? conditionValue->getName()
                                      : conditionData
            );
        }
    }

#ifdef ENABLE_DEBUGGER
    // Visiting the condition updates the generator location. Attribute the
    // actual control-flow opcode to the if statement instead.
    setCurrentLocationForGenerator(node);
#endif
    m_generator.emit(
        useNotIf ? tbc::BytOpcode::OP_NOTIF : tbc::BytOpcode::OP_IF
    );

    const SymbolTable entryState = m_scopePtr->getCurrentSymtab();
    const AltStackSnapshot entryAltStack = captureAltStack();
    const ControlFlowStateSnapshot entryControlState =
        m_scopePtr->captureControlFlowState();

    auto reportImplicitJoinError = [this, &node](
                                       const ControlFlowJoinResult& result
                                   ) {
        SourceLocation loc = getNodeLocation(node);
        std::string stateName = "compiler storage";
        switch (result.mismatch) {
            case ControlFlowMismatch::MAIN_STACK_DEPTH:
            case ControlFlowMismatch::MAIN_STACK_LAYOUT:
                stateName = "main-stack";
                break;
            case ControlFlowMismatch::ALT_STACK_DEPTH:
            case ControlFlowMismatch::ALT_STACK_LAYOUT:
                stateName = "alternative-stack";
                break;
            case ControlFlowMismatch::LEXICAL_SCOPE:
                stateName = "lexical-scope";
                break;
            default:
                break;
        }

        std::ostringstream oss;
        oss << "if without else changes " << stateName
            << " state at the join point";
        if (!result.detail.empty()) {
            oss << " - " << result.detail;
        }
        const std::string message = oss.str();
        SEMANTIC_ERROR(
            message,
            loc,
            "Restore the entry state before leaving the if branch, or add an "
            "else branch that produces a compatible state"
        );
        LOG_ERROR(message);
        throw std::runtime_error(message);
    };

    const auto mergeSymbols =
        collectIfMergeSymbols(node, entryState, entryAltStack);
    AltStackSnapshot desiredAltStack = entryAltStack;
    auto entryLocation = [&](const std::string& symbol) {
        return std::any_of(
                   entryAltStack.elements.begin(),
                   entryAltStack.elements.end(),
                   [&](const StackElement& element) {
                       return element.getName() == symbol;
                   }
               )
                   ? StorageLocation::Alt
                   : StorageLocation::Main;
    };
    for (const auto& symbol : mergeSymbols) {
        const StorageLocation initial = entryLocation(symbol);
        const StorageLocation thenLocation = storageAfterStatement(
            node.thenBranch.get(), symbol, initial
        );
        const StorageLocation elseLocation = storageAfterStatement(
            node.elseBranch.get(), symbol, initial
        );
        if (thenLocation == StorageLocation::Unknown ||
            thenLocation != elseLocation) {
            continue;
        }

        desiredAltStack.elements.erase(
            std::remove_if(
                desiredAltStack.elements.begin(),
                desiredAltStack.elements.end(),
                [&](const StackElement& element) {
                    return element.getName() == symbol;
                }
            ),
            desiredAltStack.elements.end()
        );
        if (thenLocation == StorageLocation::Alt) {
            desiredAltStack.elements.emplace_back(
                symbol,
                declaredSymbolType(entryState, symbol, ""),
                symbol
            );
        }
    }
    auto contextualBranchFlow = [&](const StmtNode* branch) {
        const FlowResult directFlow = statementFlow(branch);
        if (directFlow != FlowResult::FallsThrough ||
            !m_inlineContinuationStatements) {
            return directFlow;
        }

        const ControlFlowOutcomes branchOutcomes =
            controlFlowOutcomes(branch);
        const ControlFlowOutcomes continuationOutcomes =
            sequenceControlFlowOutcomes(
                *m_inlineContinuationStatements,
                m_inlineContinuationStart
            );
        // 嵌套分支的部分路径已 lowercase return、其余路径会落到祖先
        // block 的 continuation；当 continuation 的所有路径也都返回时，
        // 对当前 if 来说该分支已等价为完整 InlineReturn。纯 fallthrough
        // 分支不能提升，否则当前 if 将失去生成 guard 的机会。
        if (branchOutcomes.inlineReturns && branchOutcomes.fallsThrough &&
            !branchOutcomes.scriptTerminates &&
            continuationOutcomes.inlineReturns &&
            !continuationOutcomes.fallsThrough &&
            !continuationOutcomes.scriptTerminates) {
            return FlowResult::InlineReturn;
        }
        return directFlow;
    };
    const FlowResult thenFlow = contextualBranchFlow(node.thenBranch.get());
    const FlowResult elseFlow = contextualBranchFlow(node.elseBranch.get());
    const bool thenReachesEnd = thenFlow != FlowResult::ScriptTerminate;
    const bool elseReachesEnd = elseFlow != FlowResult::ScriptTerminate;
    const bool hasOneSidedInlineReturn =
        (thenFlow == FlowResult::InlineReturn &&
         elseFlow == FlowResult::FallsThrough) ||
        (thenFlow == FlowResult::FallsThrough &&
         elseFlow == FlowResult::InlineReturn);
    const auto* inlineContinuationStatements =
        m_inlineContinuationStatements;
    const size_t inlineContinuationStart = m_inlineContinuationStart;
    size_t oneSidedReturnArity = 0;
    std::string inlineGuardName;
    std::vector<std::string> inlineDummyNames;
    if (hasOneSidedInlineReturn) {
        if (!inlineContinuationStatements) {
            SourceLocation loc = getNodeLocation(node);
            std::ostringstream oss;
            oss << "lowercase return in only one if branch at line "
                << node.pos.first << ", column " << node.pos.second
                << " has no continuation that can produce the other return "
                   "value";
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Return on both branches or add a return after the if"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }
        const StmtNode* returningBranch =
            thenFlow == FlowResult::InlineReturn ? node.thenBranch.get()
                                                 : node.elseBranch.get();
        auto returnArity = inlineReturnArity(returningBranch);
        if (!returnArity.has_value() || returnArity.value() == 0) {
            throw std::runtime_error(
                "cannot determine lowercase return arity for conditional "
                "private-function return"
            );
        }
        oneSidedReturnArity = returnArity.value();
        inlineGuardName = CompilerPlaceholder().toString();
        for (size_t i = 0; i < oneSidedReturnArity; ++i) {
            inlineDummyNames.push_back(CompilerPlaceholder().toString());
        }
    }

    auto appendInlineGuardState = [&](FlowResult branchFlow) {
        if (!hasOneSidedInlineReturn) {
            return;
        }
        SymbolTable& state = m_scopePtr->getCurrentSymtab();
        if (branchFlow == FlowResult::InlineReturn) {
            const std::vector<std::string> emptyBase;
            const auto& baseStorageNames =
                m_privateFunctionBaseStorageNames.empty()
                    ? emptyBase
                    : m_privateFunctionBaseStorageNames.back();
            auto shouldRetain = [&](const std::string& name) {
                return std::find(
                           baseStorageNames.begin(),
                           baseStorageNames.end(),
                           name
                       ) != baseStorageNames.end() ||
                       std::find(
                           state.m_keepSymbol.begin(),
                           state.m_keepSymbol.end(),
                           name
                       ) != state.m_keepSymbol.end();
            };

            // 早返回路径不会执行函数尾部的普通局部清理，因此在分支内
            // 先移除该私有函数新增且未作为返回值保留的运行时槽。
            std::vector<std::string> mainLocals;
            if (state.m_stackPtr) {
                for (const auto& element :
                     state.m_stackPtr->getStackContent()) {
                    if (!shouldRetain(element.getName())) {
                        mainLocals.push_back(element.getName());
                    }
                }
            }
            std::sort(
                mainLocals.begin(),
                mainLocals.end(),
                [&](const std::string& lhs, const std::string& rhs) {
                    return state.getPos(lhs).value_or(INT64_MAX) <
                           state.getPos(rhs).value_or(INT64_MAX);
                }
            );
            for (const auto& localName : mainLocals) {
                auto position = state.getPos(localName);
                if (!position.has_value()) {
                    continue;
                }
                if (position.value() != STACK_TOP_POS) {
                    emitRoll(position.value());
                    state.roll(position.value());
                }
                state.pop();
                m_generator.emit(tbc::BytOpcode::OP_DROP);
            }

            std::vector<std::string> altLocals;
            if (state.m_altStackPtr) {
                for (const auto& element :
                     state.m_altStackPtr->getStackContent()) {
                    if (!shouldRetain(element.getName())) {
                        altLocals.push_back(element.getName());
                    }
                }
            }
            std::sort(
                altLocals.begin(),
                altLocals.end(),
                [&](const std::string& lhs, const std::string& rhs) {
                    return state.getPos(lhs, true).value_or(INT64_MAX) <
                           state.getPos(rhs, true).value_or(INT64_MAX);
                }
            );
            for (const auto& localName : altLocals) {
                auto position = state.getPos(localName, true);
                if (!position.has_value()) {
                    continue;
                }
                for (int64_t i = 0; i <= position.value(); ++i) {
                    m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
                    StackElement topElement = state.m_altStackPtr->top();
                    state.m_altStackPtr->pop();
                    state.m_stackPtr->push(topElement);
                }
                state.pop();
                m_generator.emit(tbc::BytOpcode::OP_DROP);
                for (int64_t i = 0; i < position.value(); ++i) {
                    m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                    StackElement topElement = state.m_stackPtr->top();
                    state.m_stackPtr->pop();
                    state.m_altStackPtr->push(topElement);
                }
            }

            // 返回槽统一放到主栈顶部，后接 true 标记。
            for (const auto& returnName : state.m_keepSymbol) {
                auto position = state.getPos(returnName);
                if (position.has_value() &&
                    position.value() != STACK_TOP_POS) {
                    emitRoll(position.value());
                    state.roll(position.value());
                }
            }
            m_generator.emit(tbc::BytOpcode::OP_1);
        } else {
            // 未返回路径先放等宽占位槽，再放 false 标记。后续 OP_NOTIF
            // 只在该路径删除占位并执行余下语句。
            for (const auto& dummyName : inlineDummyNames) {
                m_generator.emit(tbc::BytOpcode::OP_0);
                state.push(dummyName, "", "0x00");
            }
            m_generator.emit(tbc::BytOpcode::OP_0);
        }
        state.push(inlineGuardName, "bool", inlineGuardName);
    };

    m_lastFlowResult = FlowResult::FallsThrough;
    node.thenBranch->accept(*this);
    const FlowResult emittedThenFlow = m_lastFlowResult;

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif
    if (node.elseBranch && thenReachesEnd) {
        materializeBranchSymbols(mergeSymbols, node, desiredAltStack);
    }
    if (node.elseBranch) {
        appendInlineGuardState(thenFlow);
    } else if (emittedThenFlow == FlowResult::InlineReturn) {
        reportImplicitJoinError(
            {ControlFlowMismatch::SYMBOL_METADATA,
             "lowercase return state differs at the join point"}
        );
    }
    LOG_DEBUG("End of if branch");
    SymbolTable thenState = m_scopePtr->exitScope();
    const AltStackSnapshot thenAltStack = captureAltStack();

    // else 必须从与 then 相同的入口状态开始，不能继承 then 的编译期固定值。
    m_scopePtr->replaceCurrentSymtab(entryState);
    restoreAltStack(entryAltStack);

    if (!node.elseBranch) {
        if (emittedThenFlow == FlowResult::FallsThrough) {
            ControlFlowStateSnapshot thenControlState;
            thenControlState.symbolTable = thenState;
            thenControlState.altStack = thenAltStack.elements;
            thenControlState.mainCombinedSize =
                thenState.m_stackPtr
                    ? thenState.m_stackPtr->getCombinedStackSize()
                    : 0;
            thenControlState.altCombinedSize =
                thenAltStack.combinedStackSize;
            thenControlState.lexicalDepth = entryControlState.lexicalDepth;

            const auto joinResult = m_scopePtr->compareControlFlowStates(
                entryControlState,
                thenControlState,
                ControlFlowJoinPolicy::IMPLICIT_EMPTY_BRANCH
            );
            if (!joinResult.compatible()) {
                reportImplicitJoinError(joinResult);
            }
        }

        m_generator.emit(tbc::BytOpcode::OP_ENDIF);
        m_scopePtr->replaceCurrentSymtab(entryState);
        restoreAltStack(entryAltStack);
        m_lastFlowResult = FlowResult::FallsThrough;
        return;
    }

    m_generator.emit(tbc::BytOpcode::OP_ELSE);
    m_lastFlowResult = FlowResult::FallsThrough;
    node.elseBranch->accept(*this);

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif
    if (elseReachesEnd) {
        materializeBranchSymbols(mergeSymbols, node, desiredAltStack);
    }
    appendInlineGuardState(elseFlow);
    SymbolTable elseState = m_scopePtr->exitScope();
    const AltStackSnapshot elseAltStack = captureAltStack();
    LOG_DEBUG("End of else branch");

    SymbolTable mergedState = entryState;
    AltStackSnapshot mergedAltStack = entryAltStack;

    if (hasOneSidedInlineReturn) {
        // 原 if 的两条路径先汇合为 [返回槽..., returned]。随后 NOTIF
        // 仅在未返回路径删除占位槽并执行函数余下语句。这样 lowercase
        // return 不需要运行时跳转操作码，也不会错误执行 return 后的代码。
        m_generator.emit(tbc::BytOpcode::OP_ENDIF);

        const bool thenIsReturn = thenFlow == FlowResult::InlineReturn;
        SymbolTable returningState = thenIsReturn ? thenState : elseState;
        AltStackSnapshot returningAlt =
            thenIsReturn ? thenAltStack : elseAltStack;
        SymbolTable fallingState = thenIsReturn ? elseState : thenState;
        AltStackSnapshot fallingAlt =
            thenIsReturn ? elseAltStack : thenAltStack;

        auto removeGuard = [&](SymbolTable& state) {
            auto guard = state.pop();
            if (!guard.has_value() ||
                guard->getName() != inlineGuardName) {
                throw std::logic_error(
                    "conditional inline-return guard is missing from stack"
                );
            }
        };
        removeGuard(returningState);

        m_scopePtr->replaceCurrentSymtab(fallingState);
        restoreAltStack(fallingAlt);
        auto runtimeGuard = m_scopePtr->pop();
        if (!runtimeGuard.has_value() ||
            runtimeGuard->getName() != inlineGuardName) {
            throw std::logic_error(
                "conditional inline-return fallthrough guard is missing"
            );
        }
        m_generator.emit(tbc::BytOpcode::OP_NOTIF);
        for (size_t i = 0; i < oneSidedReturnArity; ++i) {
            auto dummy = m_scopePtr->pop();
            if (!dummy.has_value()) {
                throw std::logic_error(
                    "conditional inline-return dummy slot is missing"
                );
            }
            m_generator.emit(tbc::BytOpcode::OP_DROP);
        }

        // 允许余下语句中的下一个单边 return 建立自己的 continuation。
        m_inlineContinuationStatements = nullptr;
        m_inlineContinuationStart = 0;
        m_lastFlowResult = FlowResult::FallsThrough;
        executeStatements(
            *inlineContinuationStatements, inlineContinuationStart
        );
        const FlowResult continuationFlow = m_lastFlowResult;
        if (continuationFlow == FlowResult::FallsThrough) {
            SourceLocation loc = getNodeLocation(node);
            std::ostringstream oss;
            oss << "not all paths after lowercase return at line "
                << node.pos.first << ", column " << node.pos.second
                << " produce a private-function return value";
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Add a lowercase return on the remaining path"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }

        SymbolTable continuationState = m_scopePtr->getCurrentSymtab();
        AltStackSnapshot continuationAlt = captureAltStack();
        m_generator.emit(tbc::BytOpcode::OP_ENDIF);

        if (continuationFlow == FlowResult::InlineReturn) {
            validateBranchMerge(
                node,
                entryState,
                returningState,
                returningAlt,
                continuationState,
                continuationAlt,
                {}
            );
            mergedState = buildMergedBranchState(
                entryState, continuationState, {}
            );
            mergedAltStack = continuationAlt;
        } else {
            // continuation 以 OP_RETURN 终止时，仅早返回路径能到达后续。
            mergedState = buildMergedBranchState(
                entryState, returningState, {}
            );
            mergedAltStack = returningAlt;
        }

        m_scopePtr->replaceCurrentSymtab(mergedState);
        restoreAltStack(mergedAltStack);
        m_lastFlowResult = FlowResult::InlineReturn;
        return;
    }

    if (thenReachesEnd && elseReachesEnd) {
        validateBranchMerge(
            node,
            entryState,
            thenState,
            thenAltStack,
            elseState,
            elseAltStack,
            mergeSymbols
        );
        mergedState =
            buildMergedBranchState(entryState, thenState, mergeSymbols);
        mergedAltStack = thenAltStack;
    } else if (thenReachesEnd) {
        // else 已终止，只有 then 会到达 OP_ENDIF 后的代码。
        mergedState =
            buildMergedBranchState(entryState, thenState, mergeSymbols);
        mergedAltStack = thenAltStack;
    } else if (elseReachesEnd) {
        // then 已终止，只有 else 会到达 OP_ENDIF 后的代码。
        mergedState =
            buildMergedBranchState(entryState, elseState, mergeSymbols);
        mergedAltStack = elseAltStack;
    } else {
        LOG_DEBUG(
            "Both if branches terminate; continuation state is unreachable"
        );
    }

    m_generator.emit(tbc::BytOpcode::OP_ENDIF);
    m_scopePtr->replaceCurrentSymtab(mergedState);
    restoreAltStack(mergedAltStack);

    if (thenFlow != FlowResult::FallsThrough &&
        elseFlow != FlowResult::FallsThrough) {
        m_lastFlowResult = thenFlow == FlowResult::InlineReturn ||
                                   elseFlow == FlowResult::InlineReturn
                               ? FlowResult::InlineReturn
                               : FlowResult::ScriptTerminate;
    } else {
        m_lastFlowResult = FlowResult::FallsThrough;
    }
}

void ASTToBytecodeVisitor::visit(ForNode& node)
{
    LOG_DEBUG("Visiting for node start.");
    ++m_lifetimeControlFlowDepth;
    DEFER_BLOCK(--m_lifetimeControlFlowDepth;);

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    auto* rangeCall = dynamic_cast<CallNode*>(node.iterable.get());
    if (!rangeCall || rangeCall->funcName != "Range" ||
        rangeCall->args.empty() || rangeCall->args.size() > 3) {
        SourceLocation loc = getNodeLocation(node);
        SEMANTIC_ERROR(
            "invalid static Range loop reached bytecode lowering",
            loc,
            "Use Range with one to three compile-time integer arguments"
        );
        throw std::runtime_error("invalid static Range loop");
    }

    std::vector<int64_t> bounds;
    bounds.reserve(rangeCall->args.size());
    for (const auto& argument : rangeCall->args) {
        const auto value = evaluateCompileTimeInteger(*argument, true);
        if (!value.isKnown()) {
            SourceLocation loc = getNodeLocation(*argument);
            std::string message =
                "range() arguments must remain compile-time integers during "
                "bytecode lowering";
            if (value.isError()) {
                message += ": " + value.diagnostic;
            }
            SEMANTIC_ERROR(
                message,
                loc,
                "Use literals, fixed integer bindings, or outer loop targets"
            );
            throw std::runtime_error(
                "range argument lost its compile-time binding"
            );
        }
        bounds.push_back(value.value);
    }

    int64_t start = 0;
    int64_t stop = 0;
    int64_t step = 1;
    if (bounds.size() == 1) {
        stop = bounds[0];
    } else if (bounds.size() == 2) {
        start = bounds[0];
        stop = bounds[1];
    } else {
        start = bounds[0];
        stop = bounds[1];
        step = bounds[2];
    }

    auto planResult = apc::compiler::RangePlan::build(
        start,
        stop,
        step,
        apc::compiler::RangeLimits{
            apc::compiler::kMaxStaticRangeIterations}
    );
    if (!planResult) {
        SourceLocation loc = getNodeLocation(node);
        SEMANTIC_ERROR(
            planResult.error().message,
            loc,
            "Reduce the static range or raise the compiler loop limit"
        );
        throw std::runtime_error(planResult.error().message);
    }
    const auto& plan = planResult.value();

    if (plan.empty()) {
        LOG_DEBUG("static RangePlan is empty; skipping loop body");
        m_lastFlowResult = FlowResult::FallsThrough;
        return;
    }

    const SymbolTable parentState = m_scopePtr->getCurrentSymtab();
    std::set<std::string> loopOuterWholeArrays;
    for (const auto& [arrayName, unusedInfo] : m_wholeArrayElements) {
        (void)unusedInfo;
        loopOuterWholeArrays.insert(arrayName);
    }
    // loop target 属于 for scope；每次展开的 body 又属于独立 BlockNode
    // scope。这样 body 局部变量能在下一轮重新声明，外层变量的更新则从
    // 上一轮延续。
    m_scopePtr->enterScope();

    auto storageBelongsTo = [](const std::string& storageName,
                               const std::string& symbolName) {
        if (storageName == symbolName) {
            return true;
        }
        if (storageName.size() <= symbolName.size() ||
            storageName.compare(0, symbolName.size(), symbolName) != 0) {
            return false;
        }
        const char separator = storageName[symbolName.size()];
        return separator == '.' || separator == '[';
    };

    auto removeBodyLocals = [&](SymbolTable state,
                                const SymbolTable& iterationEntry,
                                bool preserveKeptLocals) {
        std::set<std::string> entrySymbols;
        for (const auto& entry : iterationEntry.m_currentScope) {
            entrySymbols.insert(entry.first);
        }

        std::vector<std::string> bodyLocalSymbols;
        for (const auto& entry : state.m_currentScope) {
            const bool materializedOuterWholeArray =
                loopOuterWholeArrays.count(entry.first) > 0;
            if (entrySymbols.count(entry.first) == 0 &&
                !materializedOuterWholeArray) {
                bool isKept = false;
                if (preserveKeptLocals) {
                    for (const auto& keptName : state.m_keepSymbol) {
                        if (storageBelongsTo(keptName, entry.first)) {
                            isKept = true;
                            break;
                        }
                    }
                }
                if (isKept) {
                    continue;
                }
                bodyLocalSymbols.push_back(entry.first);
            }
        }

        auto isBodyLocalStorage = [&](const std::string& storageName) {
            for (const auto& localName : bodyLocalSymbols) {
                if (storageBelongsTo(storageName, localName)) {
                    return true;
                }
            }
            return false;
        };

        for (const auto& localName : bodyLocalSymbols) {
            state.removeSymbol(localName);
        }

        if (state.m_fixedStackPtr) {
            std::vector<StackElement> retainedFixed;
            for (const auto& element :
                 state.m_fixedStackPtr->getStackContent()) {
                if (!isBodyLocalStorage(element.getName())) {
                    retainedFixed.push_back(element);
                }
            }
            state.m_fixedStackPtr->replaceStackContent(retainedFixed);
        }

        // BlockNode 已负责发射运行时局部值的清理操作。这里同步清除它
        // 留下的声明/新槽元数据，同时保留外层变量在 body 中重新绑定后
        // 产生的新槽记录。
        state.m_declaredSymbols = iterationEntry.m_declaredSymbols;
        state.m_declaredSymbols.erase(
            std::remove_if(
                state.m_declaredSymbols.begin(),
                state.m_declaredSymbols.end(),
                [&](const std::string& name) {
                    return !state.hasScopeEntry(name);
                }
            ),
            state.m_declaredSymbols.end()
        );
        state.m_newSymbol.erase(
            std::remove_if(
                state.m_newSymbol.begin(),
                state.m_newSymbol.end(),
                isBodyLocalStorage
            ),
            state.m_newSymbol.end()
        );
        state.m_keepSymbol.erase(
            std::remove_if(
                state.m_keepSymbol.begin(),
                state.m_keepSymbol.end(),
                isBodyLocalStorage
            ),
            state.m_keepSymbol.end()
        );

        return state;
    };

    FlowResult loopFlow = FlowResult::FallsThrough;

    for (uint64_t idx = 0; idx < plan.count(); ++idx) {
        if (auto budgetError = m_loopExpansionBudget.consume()) {
            SourceLocation loc = getNodeLocation(node);
            SEMANTIC_ERROR(
                budgetError->message,
                loc,
                "Reduce nested static loop expansion"
            );
            throw std::runtime_error(budgetError->message);
        }

        const auto iterationResult = plan.valueAt(idx);
        const auto* iterationValue = std::get_if<int64_t>(&iterationResult);
        if (!iterationValue) {
            const auto& error =
                std::get<apc::compiler::RangeError>(iterationResult);
            SourceLocation loc = getNodeLocation(node);
            INTERNAL_ERROR(
                error.message,
                loc,
                "Report this invalid RangePlan value calculation"
            );
            throw std::runtime_error(error.message);
        }

        bindStaticLoopTarget(node, *iterationValue);

        FlowResult bodyFlow = FlowResult::FallsThrough;
        if (node.body) {
            const SymbolTable iterationEntry =
                m_scopePtr->getCurrentSymtab();

            // 走 BlockNode visitor，既生成每轮独立的词法/调试作用域，也
            // 复用统一的运行时局部清理逻辑。
            m_lastFlowResult = FlowResult::FallsThrough;
            node.body->accept(*this);
            bodyFlow = m_lastFlowResult;
            SymbolTable iterationExit = m_scopePtr->getCurrentSymtab();

            // BlockNode::visit() 的 enterScope 只建立清理边界；for 在此
            // 显式弹出该边界，并将过滤掉本轮局部声明后的状态带入下一轮。
            m_scopePtr->exitScope();
            m_scopePtr->replaceCurrentSymtab(
                removeBodyLocals(
                    std::move(iterationExit),
                    iterationEntry,
                    bodyFlow != FlowResult::FallsThrough
                )
            );
        }

        if (m_generator.getCurrentPC() >
            apc::compiler::kMaxGeneratedLoopInstructions) {
            SourceLocation loc = getNodeLocation(node);
            const std::string message =
                "generated instruction count exceeds static expansion limit "
                + std::to_string(
                    apc::compiler::kMaxGeneratedLoopInstructions);
            SEMANTIC_ERROR(
                message,
                loc,
                "Reduce the loop body or static iteration count"
            );
            throw std::runtime_error(message);
        }

        if (bodyFlow != FlowResult::FallsThrough) {
            loopFlow = bodyFlow;
            break;
        }
    }

    SymbolTable loopExitState = m_scopePtr->getCurrentSymtab();
    m_scopePtr->exitScope();

    // 恢复父作用域的声明归属；栈/fixed/alt 内容来自最后一轮，确保外层
    // 变量的重新绑定跨迭代、跨 for 继续生效。
    loopExitState.m_declaredSymbols = parentState.m_declaredSymbols;
    std::vector<std::string> mergedNewSymbols = parentState.m_newSymbol;
    for (const auto& name : loopExitState.m_newSymbol) {
        if (std::find(
                mergedNewSymbols.begin(), mergedNewSymbols.end(), name
            ) == mergedNewSymbols.end()) {
            mergedNewSymbols.push_back(name);
        }
    }
    loopExitState.m_newSymbol = std::move(mergedNewSymbols);
    m_scopePtr->replaceCurrentSymtab(loopExitState);
    m_lastFlowResult = loopFlow;

    LOG_DEBUG("Visiting for node end.");
}

void ASTToBytecodeVisitor::bindStaticLoopTarget(
    ForNode& loop,
    int64_t value
)
{
    const bool isNewSymbol = !m_scopePtr->symbolExists(loop.target);
    if (!isNewSymbol) {
        const auto symbols =
            m_scopePtr->getCurrentSymtab().getCurrentScopeSymbols();
        const auto existing = std::find_if(
            symbols.rbegin(),
            symbols.rend(),
            [&loop](const tbc::SymbolInfo& symbol) {
                return symbol.getSymbolName() == loop.target;
            }
        );
        const std::string existingType =
            existing == symbols.rend()
                ? std::string()
                : existing->m_stackElement.getType();
        const bool isNumericScalar =
            existing != symbols.rend() && !existing->isArray() &&
            !existing->isCompoundType() &&
            apc::compiler::isCompatibleLoopTargetType(existingType);
        if (!isNumericScalar) {
            SourceLocation loc = getNodeLocation(loop);
            const std::string message =
                "for loop target '" + loop.target +
                "' must be a numeric scalar";
            SEMANTIC_ERROR(
                message,
                loc,
                "Use a new loop target or an existing integer variable"
            );
            throw std::runtime_error(message);
        }
    }
    if (isNewSymbol &&
        !m_scopePtr->defineSymbol(loop.target, loop.getInferredType())) {
        throw std::runtime_error(
            "failed to define static loop target '" + loop.target + "'"
        );
    }

    // A body assignment may have materialized the target on either runtime
    // stack. The next induction assignment removes that slot before restoring
    // a compiler-only fixed binding.
    if (m_scopePtr->getPos(loop.target, true).has_value() &&
        !moveAltElementToMain(loop.target)) {
        throw std::runtime_error(
            "failed to restore loop target from alternative stack"
        );
    }

    if (auto position = m_scopePtr->getPos(loop.target)) {
        if (*position != STACK_TOP_POS) {
            emitRoll(*position);
            m_scopePtr->roll(*position);
        }
        m_generator.emit(tbc::BytOpcode::OP_DROP);
        m_scopePtr->pop();
    }

    m_scopePtr->setFixed(tbc::StackElement(
        loop.target, "num", numberToScriptHex(value)
    ));
    m_scopePtr->markSymbolInitialized(loop.target);

#ifdef ENABLE_DEBUGGER
    if (isNewSymbol && m_debugInfoGen) {
        m_debugInfoGen->onVariableDecl(
            loop.target,
            loop.getInferredType(),
            extractDebugLocation(loop),
            false,
            -1,
            false
        );
    }
#endif
}

void ASTToBytecodeVisitor::visit(AssignNode& node)
{
    LOG_DEBUG("Visiting assign node start.");

    std::string leftVarName;
    if (auto identifierNode = dynamic_cast<IdentifierNode*>(node.name.get())) {
        leftVarName = identifierNode->name;
    }

    auto generalLamd = [this](
                           ASTNode& node,
                           std::optional<StackElement> elementOpt,
                           const std::string& side
                       ) -> std::optional<StackElement> {
        if (!elementOpt.has_value()) {
            std::ostringstream errorStream;
            errorStream << "No processing results for " << side
                        << " expression at line " << node.pos.first
                        << ", column " << node.pos.second;
            SourceLocation loc = getNodeLocation(node);
            SEMANTIC_ERROR(
                errorStream.str(),
                loc,
                "Check the expression syntax and operands"
            );
            LOG_ERROR(errorStream.str());
            throw std::runtime_error(errorStream.str());
        }

        auto element = elementOpt.value();
        auto elementStr = element.getName();

        if (isScript(elementStr)) {
            return std::nullopt;
        }

        return element;
    };

    // 结构体大括号赋值: data = {a, b, ...}.
    if (auto braceExpr = dynamic_cast<BraceExprNode*>(node.value.get())) {
        if (!leftVarName.empty()) {
            tryHandleStructBraceAssignment(node, leftVarName, *braceExpr);
        }
        return;
    }

    // 数组定义赋值: RHS 是 array def 时, LHS 必须是数组且元素数一致.
    if (auto arrayDef = dynamic_cast<ArrayDefNode*>(node.value.get())) {
        if (!leftVarName.empty()) {
            tryHandleArrayDefAssignment(node, leftVarName, *arrayDef);
        }
        return;
    }

    LOG_DEBUG("Evaluating assignment expression");
    visitExpr(*node.value);

    auto valueElementOpt = m_scopePtr->pop();
    const auto* rhsCall = dynamic_cast<const CallNode*>(node.value.get());
    const bool isPrivateCallResult =
        rhsCall && m_privateFunctions.find(rhsCall->funcName) !=
                       m_privateFunctions.end();
    auto rightHandSideElement =
        generalLamd(*node.value, valueElementOpt, "right-hand side");
    // A private/library call may return a literal. Its lowercase-return path
    // has already emitted the literal, so the script-shaped descriptor is a
    // real runtime result rather than a compile-time fixed value.
    if (!rightHandSideElement.has_value() && isPrivateCallResult &&
        valueElementOpt.has_value()) {
        rightHandSideElement = valueElementOpt;
    }
    if (!rightHandSideElement.has_value()) {
        SourceLocation loc = getNodeLocation(*node.value);
        std::ostringstream oss;
        oss << "assigning a generated script element to variable may not be "
               "meaningful. Element: "
            << valueElementOpt.value().getName();
        COMPILER_WARNING(oss.str(), loc);
        LOG_WARNING(oss.str());
    }

    // Delete removes a field/element's stack slot and symbol-table entry while
    // preserving the parent compound/array declaration. For a subsequent
    // rebinding, evaluating that missing LHS as an rvalue would fail before
    // the normal zero-cost rename path can restore the slot. Recognize only
    // statically known members of a still-declared parent here.
    const auto targetStorageName = getAssignmentStorageName(node.name.get());
    std::string restoredTargetType;
    bool restoringDeletedTarget = false;

    auto targetHasStoredValue = [&](const std::string& targetName) {
        if (m_scopePtr->getPos(targetName).has_value() ||
            m_scopePtr->getPos(targetName, true).has_value()) {
            return true;
        }
        std::string mutableName = targetName;
        return m_scopePtr->getFixed(mutableName).has_value();
    };

    if (targetStorageName.has_value() &&
        !targetHasStoredValue(targetStorageName.value())) {
        if (auto* field =
                dynamic_cast<FieldAccessNode*>(node.name.get())) {
            const auto parentName =
                getAssignmentStorageName(field->base.get());
            if (parentName.has_value()) {
                if (m_scopePtr->isCompoundTypeSymbol(parentName.value()) &&
                    m_scopePtr->isCompoundTypeSplitted(parentName.value())) {
                    const auto info =
                        m_scopePtr->getCompoundTypeInfo(parentName.value());
                    if (info.has_value()) {
                        const auto fieldIt = std::find_if(
                            info->fields.begin(),
                            info->fields.end(),
                            [&](const CompoundFieldInfo& item) {
                                return item.name == field->field;
                            }
                        );
                        if (fieldIt != info->fields.end()) {
                            restoredTargetType = fieldIt->type;
                            restoringDeletedTarget = true;
                        }
                    }
                }

                // Function parameters and ordinary struct variables are
                // represented by a typed root symbol plus flattened field
                // slots rather than CompoundTypeInfo. The root declaration
                // remains after Delete(root.field), so use its declared type
                // to validate the missing field before restoring it.
                if (!restoringDeletedTarget) {
                    std::string parentType;
                    const auto symbols = m_scopePtr->getCurrentSymtab()
                                             .getCurrentScopeSymbols();
                    for (auto it = symbols.rbegin(); it != symbols.rend();
                         ++it) {
                        if (it->getSymbolName() == parentName.value()) {
                            parentType = it->m_stackElement.getType();
                            break;
                        }
                    }

                    const auto structIt =
                        m_structDefinitions.find(parentType);
                    if (structIt != m_structDefinitions.end()) {
                        const auto fieldIt = std::find_if(
                            structIt->second.begin(),
                            structIt->second.end(),
                            [&](const auto& item) {
                                return item.first == field->field;
                            }
                        );
                        if (fieldIt != structIt->second.end()) {
                            restoredTargetType =
                                fieldIt->second.getTypeString();
                            restoringDeletedTarget = true;
                        }
                    }
                }
            }
        } else if (auto* index =
                       dynamic_cast<IndexAccessNode*>(node.name.get())) {
            const auto baseName =
                getAssignmentStorageName(index->base.get());
            const auto* literal =
                dynamic_cast<const LiteralNode*>(index->index.get());
            if (baseName.has_value() && literal &&
                literal->type == LiteralNode::Type::Number) {
                try {
                    const int64_t parsedIndex = std::stoll(literal->value);
                    if (parsedIndex >= 0) {
                        const size_t elementIndex =
                            static_cast<size_t>(parsedIndex);
                        if (const auto arrayInfo =
                                m_scopePtr->getArrayInfo(baseName.value());
                            arrayInfo.has_value() &&
                            arrayInfo->isValidIndex(elementIndex) &&
                            m_structDefinitions.find(arrayInfo->elementType) ==
                                m_structDefinitions.end()) {
                            // A missing scalar element can be recreated as one
                            // slot. A struct-array element is represented by a
                            // flattened/packed composite layout and must keep
                            // using the ordinary assignment path; treating an
                            // unmaterialized element as a deleted scalar would
                            // incorrectly require the RHS type to equal the
                            // struct name (for example Input vs hex36).
                            restoredTargetType = arrayInfo->elementType;
                            restoringDeletedTarget = true;
                        } else if (const auto wholeInfo =
                                       getWholeArrayInfo(baseName.value());
                                   wholeInfo.has_value() &&
                                   elementIndex < wholeInfo->first) {
                            restoredTargetType = "uint64";
                            restoringDeletedTarget = true;
                        }
                    }
                } catch (const std::exception&) {
                    // Invalid literal indexes keep the normal diagnostic path.
                }
            }
        }
    }

    if (restoringDeletedTarget && !restoredTargetType.empty()) {
        StackElement valueForTypeCheck = valueElementOpt.value();
        if (valueForTypeCheck.getType().empty()) {
            const auto valueStorageName =
                getAssignmentStorageName(node.value.get());
            if (valueStorageName.has_value()) {
                const auto valuePos =
                    m_scopePtr->getPos(valueStorageName.value());
                if (valuePos.has_value()) {
                    valueForTypeCheck =
                        m_scopePtr->stacktop(valuePos.value());
                } else {
                    std::string mutableValueName =
                        valueStorageName.value();
                    const auto fixedValue =
                        m_scopePtr->getFixed(mutableValueName);
                    if (fixedValue.has_value()) {
                        valueForTypeCheck = fixedValue.value();
                    }
                }
            }
        }
        validateDeclaredType(
            "type mismatch while rebinding deleted member",
            restoredTargetType,
            valueForTypeCheck,
            node
        );
    }

    std::optional<StackElement> nameElementOpt;
    if (restoringDeletedTarget) {
        nameElementOpt = StackElement(
            targetStorageName.value(),
            restoredTargetType,
            targetStorageName.value()
        );
    } else {
        visitExpr(*node.name);
        nameElementOpt = m_scopePtr->pop();
    }

    auto leftHandSideElement =
        generalLamd(*node.name, nameElementOpt, "left-hand side");

    // 重新绑定语义: LHS 是已知变量名时即便返回了数据值也允许赋值.
    bool isValidLeftSide = false;
    bool isFirstBinding = false;
    std::string nameElementStr;

    if (!leftVarName.empty() && m_scopePtr->symbolExists(leftVarName)) {
        isValidLeftSide = true;
        nameElementStr = leftVarName;

        if (!m_scopePtr->isSymbolInitialized(leftVarName)) {
            isFirstBinding = true;
            m_scopePtr->markSymbolInitialized(leftVarName);
            LOG_DEBUG("First binding for variable: " + leftVarName);
        } else {
            validateRebinding(leftVarName, node);
            LOG_DEBUG("Rebinding detected for variable: " + leftVarName);
        }
    } else if (leftHandSideElement.has_value()) {
        auto nameElement = leftHandSideElement.value();
        nameElementStr = nameElement.getName();

        // Struct parameters are bound leaf-by-leaf. Store field assignments
        // under the caller-visible identity so a later `return param` finds
        // the updated field in main/fixed storage instead of a stale
        // `param.field` alias.
        if (dynamic_cast<FieldAccessNode*>(node.name.get()) != nullptr) {
            nameElementStr = m_scopePtr->getCurrentSymtab().resolveBindSymbol(
                nameElementStr
            );
        }

        if (!CompilerPlaceholder::isPlaceholder(nameElementStr) &&
            !isScript(nameElementStr)) {
            isValidLeftSide = true;

            if (!leftVarName.empty()) {
                if (!m_scopePtr->isSymbolInitialized(leftVarName)) {
                    isFirstBinding = true;
                    m_scopePtr->markSymbolInitialized(leftVarName);
                    LOG_DEBUG("First binding for variable: " + leftVarName);
                } else {
                    validateRebinding(leftVarName, node);
                    LOG_DEBUG(
                        "Rebinding detected for variable: " + leftVarName
                    );
                }
            }
        }
    }

    if (!isValidLeftSide) {
        std::ostringstream errorStream;
        errorStream << "Cannot assign to rvalue " << " at line "
                    << node.pos.first << ", column " << node.pos.second << ": "
                    << (nameElementOpt.has_value()
                            ? nameElementOpt.value().getName()
                            : "unknown");
        SourceLocation loc = getNodeLocation(*node.name);
        SEMANTIC_ERROR(
            errorStream.str(),
            loc,
            "Left-hand side of assignment must be a variable"
        );
        LOG_ERROR(errorStream.str());
        throw std::runtime_error(errorStream.str());
    }

    if (CompilerPlaceholder::isPlaceholder(nameElementStr)) {
        std::ostringstream errorStream;
        errorStream << "Cannot assign to rvalue " << " at line "
                    << node.pos.first << ", column " << node.pos.second << ": "
                    << nameElementStr;
        SourceLocation loc = getNodeLocation(*node.name);
        SEMANTIC_ERROR(
            errorStream.str(),
            loc,
            "Left-hand side of assignment must be a variable name"
        );
        LOG_ERROR(errorStream.str());
        throw std::runtime_error(errorStream.str());
    }
    
    // 隐式变量声明: LHS 是简单标识符且不在符号表则自动声明.
    if (!leftVarName.empty() && !m_scopePtr->symbolExists(leftVarName)) {
        std::string inferredType;
        if (rightHandSideElement.has_value()) {
            inferredType = rightHandSideElement.value().getType();
        } else if (valueElementOpt.has_value()) {
            inferredType = valueElementOpt.value().getType();
        }
        m_scopePtr->defineSymbol(leftVarName, inferredType);
        m_scopePtr->markSymbolInitialized(leftVarName);
        LOG_DEBUG("Implicit declaration via assignment: " + leftVarName);
    }

    auto valueElement = valueElementOpt.value();
    auto valueElementStr = valueElement.getName();

    // A lowercase struct return leaves every flattened field in place and
    // supplies a compiler-only root descriptor. It must never fall through to
    // the scalar assignment paths below: those would create a ghost root slot
    // while leaving the actual fields under their old identity.
    const bool isReturnedStructDescriptor =
        m_structDefinitions.find(valueElement.getType()) !=
        m_structDefinitions.end();
    if (isReturnedStructDescriptor) {
        if (leftVarName.empty()) {
            SourceLocation loc = getNodeLocation(node);
            const std::string errorMsg =
                "a returned struct can only be assigned to a named variable";
            SEMANTIC_ERROR(
                errorMsg, loc, "Assign the result to a struct variable"
            );
            LOG_ERROR(errorMsg);
            throw std::runtime_error(errorMsg);
        }

        const auto symbols =
            m_scopePtr->getCurrentSymtab().getCurrentScopeSymbols();
        for (auto it = symbols.rbegin(); it != symbols.rend(); ++it) {
            if (it->getSymbolName() != leftVarName) {
                continue;
            }
            const std::string targetType = it->m_stackElement.getType();
            if (!targetType.empty() && targetType != valueElement.getType()) {
                SourceLocation loc = getNodeLocation(node);
                std::ostringstream oss;
                oss << "cannot assign returned struct type '"
                    << valueElement.getType() << "' to variable '"
                    << leftVarName << "' of type '" << targetType << "'";
                SEMANTIC_ERROR(
                    oss.str(), loc, "Use a variable of the same struct type"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }
            break;
        }

        if (valueElementStr == leftVarName) {
            m_scopePtr->markSymbolInitialized(leftVarName);
            LOG_INFO(
                "Preserved returned struct \"",
                leftVarName,
                "\" without scalar rebinding"
            );
            LOG_DEBUG("Visiting assign node end (returned struct identity).");
            return;
        }

        if (!isFirstBinding) {
            SourceLocation loc = getNodeLocation(node);
            std::ostringstream oss;
            oss << "cannot overwrite initialized struct '" << leftVarName
                << "' with returned struct '" << valueElementStr << "'";
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Assign the result to a new or uninitialized struct variable"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }

        if (transferCompositeIdentity(valueElementStr, leftVarName)) {
            LOG_DEBUG("Visiting assign node end (returned struct transfer).");
            return;
        }

        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "failed to transfer returned struct '" << valueElementStr
            << "' to '" << leftVarName << "'";
        SEMANTIC_ERROR(
            oss.str(), loc, "Ensure all returned struct fields are available"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    const bool isRuntimeExpressionResult =
        CompilerPlaceholder::isPlaceholder(valueElementStr) ||
        isPrivateCallResult;

    // 复合类型变量不能直接承接 script 元素 (常量): 否则常量进入 fixed 区
    // 顶替复合变量名, 后续 a.field 在 FieldAccessNode 中无法识别.
    if (!nameElementStr.empty() &&
        m_scopePtr->isCompoundTypeSymbol(nameElementStr) &&
        isScript(valueElementStr)) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "compound type variable '" << nameElementStr
            << "' cannot be assigned a constant directly";
        SEMANTIC_ERROR(
            oss.str(),
            loc,
            "Assign compound type fields individually via field access "
            "(e.g. var.field = ...)"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    // 复合变量整体身份转移: a=b 首次绑定 + RHS 是具名复合变量, 把 b 的栈槽
    // 和元数据搬到 a, 跳过标量专用流程 (ROLL+DROP/重命名/setFixed 对复合
    // 变量会产生错误语义).
    if (isFirstBinding && !leftVarName.empty() && !valueElementStr.empty() &&
        !isRuntimeExpressionResult &&
        !isScript(valueElementStr) && valueElementStr != leftVarName) {
        if (transferCompositeIdentity(valueElementStr, leftVarName)) {
            LOG_DEBUG("Visiting assign node end (composite transfer).");
            return;
        }
    }

    auto elementPosOpt = m_scopePtr->getPos(valueElementStr);
    auto comElementPos = m_scopePtr->getPos(nameElementStr);

    // 栈到栈精确拷贝: b = a, 二者均在主栈上.
    bool isStackToStackCopy =
        !isRuntimeExpressionResult &&
        elementPosOpt.has_value() && comElementPos.has_value();

    if (isStackToStackCopy) {
        if (!nameElementStr.empty()) {
            m_scopePtr->removeFixed(nameElementStr);
        }
        int64_t posA = elementPosOpt.value();
        int64_t posB = comElementPos.value();

        if (posB == posA) {
            // 自赋值 b = b: 语义无意义.
            std::ostringstream errorStream;
            errorStream << "Self-assignment of variable '" << nameElementStr
                        << "' at line " << node.pos.first << ", column "
                        << node.pos.second;
            SourceLocation loc = getNodeLocation(*node.name);
            SEMANTIC_ERROR(
                errorStream.str(), loc, "Self-assignment is not allowed"
            );
            LOG_ERROR(errorStream.str());
            throw std::runtime_error(errorStream.str());
        }

#ifdef ENABLE_DEBUGGER
        setCurrentLocationForGenerator(node);
#endif
        const auto copyEmission = emitCopyAssignment(
            m_generator,
            static_cast<size_t>(posA),
            static_cast<size_t>(posB)
        );
        if (copyEmission.usedPlanner) {
            LOG_DEBUG(
                "Shortest shallow copy-assignment plan selected: ",
                copyEmission.legacyBytes,
                " -> ",
                copyEmission.emittedBytes,
                " bytes"
            );
        }
        LOG_INFO(
            "Variable \"",
            valueElementStr,
            "\" copy-assigned to \"",
            nameElementStr,
            "\" at exact stack position ",
            posB
        );
    } else {
        // 非栈到栈: 占位符结果 / 非栈元素 (含 script) / 新变量.

        // 零成本赋值: LHS 是新变量、RHS 已在主栈时直接重命名原槽位免 PICK.
        bool isZeroCostAssignment =
            !comElementPos.has_value() && elementPosOpt.has_value() &&
            !isRuntimeExpressionResult;

        if (comElementPos.has_value()) {
            auto vmElementPos = 0;
            // PICK/占位符结果在栈顶+1, 目标槽位下移 1; RHS 不在主栈时不偏移.
            if (isRuntimeExpressionResult || elementPosOpt.has_value()) {
                vmElementPos = comElementPos.value() + 1;
            } else {
                vmElementPos = comElementPos.value();
            }

            LOG_INFO(
                "The value of the variable \"",
                nameElementStr,
                "\" has been changed"
            );

            if (vmElementPos == 1) {
                m_generator.emit(tbc::BytOpcode::OP_NIP);
            } else if (STACK_TOP_POS != vmElementPos) {
                emitRoll(vmElementPos);
                m_generator.emit(tbc::BytOpcode::OP_DROP);
            } else {
                m_generator.emit(tbc::BytOpcode::OP_DROP);
            }
        }

        // 更新作用域模型 (script/非栈元素均经 valueElement 携带数据).
        if (!comElementPos.has_value()) {
            if (isZeroCostAssignment) {
                // 3a: 非栈左 = 栈上右 -> 原地重命名, 零字节码.
                m_scopePtr->removeFixed(nameElementStr);
                m_scopePtr->renameAtPosition(
                    static_cast<int>(elementPosOpt.value()), nameElementStr
                );
                LOG_INFO(
                    "Zero-cost assignment: renamed \"",
                    valueElementStr,
                    "\" -> \"",
                    nameElementStr,
                    "\" at stack position ",
                    elementPosOpt.value()
                );
            } else if (isRuntimeExpressionResult) {
                // 3b: 非栈左 = 运行时表达式结果 -> push 左值并绑定栈顶槽.
                m_scopePtr->removeFixed(nameElementStr);
                m_scopePtr->push(
                    nameElementStr,
                    rightHandSideElement.value().getType(),
                    rightHandSideElement.value().getData()
                );
                LOG_INFO(
                    "Non-stack to placeholder assignment: pushed \"",
                    nameElementStr,
                    "\" onto compiler stack (bound to placeholder result)"
                );
            } else {
                // 4: 非栈左 = 非栈右 -> 直接存入固定区, 无运行时操作.
                tbc::StackElement fixedElement(
                    nameElementStr,
                    valueElement.getType(),
                    valueElement.getData()
                );
                m_scopePtr->setFixed(fixedElement);
                LOG_INFO(
                    "Non-stack to non-stack assignment: stored \"",
                    nameElementStr,
                    "\" in fixed area with data: ",
                    valueElement.getData()
                );
            }
        } else {
            if (isRuntimeExpressionResult) {
                // 1b: 栈左 = 运行时表达式结果 -> 字节码已含 +1 偏移
                // ROLL+DROP，编译器层 roll LHS 到栈顶作为新内容。
                m_scopePtr->removeFixed(nameElementStr);
                m_scopePtr->roll(comElementPos.value());
                LOG_INFO(
                    "Stack to placeholder assignment: rolled \"",
                    nameElementStr,
                    "\" to top of compiler stack"
                );
            } else {
                // 2: 栈左 = 非栈右 -> 字节码已 ROLL+DROP 移除左值槽位,
                // 编译器层 roll 到顶后 pop, 再把左值存入固定区.
                m_scopePtr->roll(comElementPos.value());
                m_scopePtr->pop();
                tbc::StackElement fixedElement(
                    nameElementStr,
                    valueElement.getType(),
                    valueElement.getData()
                );
                m_scopePtr->setFixed(fixedElement);
                LOG_INFO(
                    "Stack to non-stack assignment: removed \"",
                    nameElementStr,
                    "\" from compiler stack and stored in fixed area with "
                    "data: ",
                    valueElement.getData()
                );
            }
        }
    }

    if (restoringDeletedTarget) {
        std::string targetName = targetStorageName.value();
        if (!m_scopePtr->symbolExists(targetName)) {
            if (restoredTargetType.empty() && valueElementOpt.has_value()) {
                restoredTargetType = valueElementOpt->getType();
            }
            m_scopePtr->defineSymbol(targetName, restoredTargetType);
            m_scopePtr->markSymbolInitialized(targetName);
        }
    }

    LOG_DEBUG("Visiting assign node end.");
}

void ASTToBytecodeVisitor::visit(ExprStmtNode& node)
{
    LOG_DEBUG("Visiting exprstmt node start.");
    // 表达式语句结果无接收者, 立即 DROP 减小后续 ROLL 深度.
    // 仅对纯读/纯算做提前 DROP: Call/MethodCall 编译器栈与运行时栈的
    // placeholder 可能不同步 (SetAlt 把真实值搬到副栈), 立即 DROP 会误伤
    // 其它栈元素. 遗留 placeholder 由作用域退出时统一 cleanup.
    const bool isCall = dynamic_cast<CallNode*>(node.expr.get()) != nullptr ||
                        dynamic_cast<MethodCallNode*>(node.expr.get()) != nullptr;
    const size_t preSize = m_scopePtr->size();
    const std::string preBytecode = m_generator.subStr();
    visitExpr(*node.expr);
    const size_t postSize = m_scopePtr->size();
    const bool producedRuntimeValue = m_generator.subStr() != preBytecode;
    if (isCall && !m_scopePtr->empty() &&
        m_structDefinitions.find(m_scopePtr->top().getType()) !=
            m_structDefinitions.end()) {
        SourceLocation loc = getNodeLocation(node);
        const std::string errorMsg =
            "returned struct value is unused in expression statement";
        SEMANTIC_ERROR(
            errorMsg,
            loc,
            "Assign, return, or pass the returned struct to another function"
        );
        LOG_ERROR(errorMsg);
        throw std::runtime_error(errorMsg);
    }
    if (!isCall && postSize > preSize) {
        const size_t delta = postSize - preSize;
        for (size_t k = 0; k < delta; ++k) {
            m_scopePtr->pop();
            // 字面量和 fixed 标识符只产生编译期伪值，并没有向运行时栈
            // 写入数据；这种情况下只清理符号模型，不能生成幽灵 DROP。
            if (producedRuntimeValue) {
                m_generator.emit(tbc::BytOpcode::OP_DROP);
            }
        }
    }
    LOG_DEBUG("Visiting exprstmt node end.");
}

bool ASTToBytecodeVisitor::moveAltElementToMain(
    const std::string& name,
    bool resolveBinding
)
{
    SymbolTable& symbolTable = m_scopePtr->getCurrentSymtab();
    auto positionOpt = resolveBinding
                           ? symbolTable.getPos(name, true)
                           : symbolTable.getPhysicalPos(name, true);
    if (!positionOpt.has_value()) {
        return false;
    }

    const int64_t position = positionOpt.value();

    // Pull the target and everything above it onto the main stack. The target
    // is now on top. SWAP+TOALTSTACK restores each displaced element while
    // leaving the target on main:
    //   alt [..., target, a1, a0] -> main [..., target], alt [..., a1, a0].
    for (int64_t i = 0; i <= position; ++i) {
        m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
    }
    symbolTable.setMain(static_cast<int32_t>(position + 1));

    for (int64_t i = 0; i < position; ++i) {
        emitRoll(1);
        symbolTable.roll(1);
        m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
        symbolTable.m_altStackPtr->moveTopToStack(
            *symbolTable.m_stackPtr.get(),
            true // The altstack is shared by all active scopes.
        );
    }

    return true;
}

std::optional<ASTToBytecodeVisitor::ResolvedCompositeArrayElement>
ASTToBytecodeVisitor::resolveCompositeArrayElement(
    ExprNode& expression,
    const std::string& functionName,
    const SourceLocation& loc
)
{
    auto* access = dynamic_cast<IndexAccessNode*>(&expression);
    if (!access || !access->base || !access->index) {
        return std::nullopt;
    }
    auto* base = dynamic_cast<IdentifierNode*>(access->base.get());
    if (!base) {
        return std::nullopt;
    }

    const std::string arrayName = base->name;
    auto lexicalArray = m_scopePtr->getArrayInfo(arrayName);
    SymbolTable& symtab = m_scopePtr->getCurrentSymtab();
    const bool lexicalNameExists = symtab.hasScopeEntry(arrayName);
    if (lexicalNameExists && !lexicalArray.has_value()) {
        // A scalar/struct with the same spelling shadows the escaped channel.
        // Let the ordinary expression path diagnose the invalid index access.
        return std::nullopt;
    }
    auto escapedIt = m_escapedAltArrays.find(arrayName);
    const bool usesEscapedView = !lexicalArray.has_value() ||
                                 symtab.isExternalArrayView(arrayName);

    std::optional<ArrayInfo> arrayInfo = lexicalArray;
    if (!arrayInfo.has_value() && functionName == "SetMain" &&
        escapedIt != m_escapedAltArrays.end()) {
        arrayInfo = escapedIt->second.arrayInfo;
    }
    if (!arrayInfo.has_value()) {
        return std::nullopt;
    }
    if (m_structDefinitions.find(arrayInfo->elementType) ==
        m_structDefinitions.end()) {
        return std::nullopt;
    }

    auto index = resolveCompileTimeIndex(*access->index);
    if (!index.has_value()) {
        const std::string message =
            functionName + "() requires a compile-time index for composite "
            "array '" + arrayName + "'";
        SEMANTIC_ERROR(
            message, loc,
            "Use a constant index or a statically unrolled Range loop"
        );
        throw std::runtime_error(message);
    }
    if (index.value() < 0 ||
        static_cast<uint64_t>(index.value()) >= arrayInfo->size) {
        const std::string message =
            "Array index " + std::to_string(index.value()) +
            " is out of bounds for '" + arrayName + "' of length " +
            std::to_string(arrayInfo->size);
        SEMANTIC_ERROR(message, loc, "Use an index within the array bounds");
        throw std::runtime_error(message);
    }

    ResolvedCompositeArrayElement group;
    group.arrayName = arrayName;
    group.arrayInfo = arrayInfo.value();
    group.elementLabel = arrayInfo->getElementLabel(
        static_cast<size_t>(index.value())
    );
    group.usesEscapedView = usesEscapedView;

    // SetMain must restore exactly the physical slots captured by SetAlt.
    if (functionName == "SetMain" && usesEscapedView &&
        escapedIt != m_escapedAltArrays.end()) {
        auto layoutsIt =
            escapedIt->second.elementLayouts.find(group.elementLabel);
        if (layoutsIt != escapedIt->second.elementLayouts.end()) {
            SymbolTable& currentSymtab = m_scopePtr->getCurrentSymtab();
            // A channel may be produced repeatedly with different actual
            // arguments. Select the newest complete layout still live on the
            // altstack. A partially present newest batch is always an error.
            for (auto layout = layoutsIt->second.rbegin();
                 layout != layoutsIt->second.rend();
                 ++layout) {
                const size_t liveLeafCount = static_cast<size_t>(
                    std::count_if(
                        layout->begin(),
                        layout->end(),
                        [&](const StackElement& slot) {
                            return currentSymtab
                                .getPhysicalPos(slot.getName(), true)
                                .has_value();
                        }
                    )
                );
                if (liveLeafCount == layout->size()) {
                    group.slots = *layout;
                    return group;
                }
                if (liveLeafCount != 0) {
                    const std::string message =
                        "Alternate-stack binding for composite element '" +
                        group.elementLabel + "' is only partially live (" +
                        std::to_string(liveLeafCount) + "/" +
                        std::to_string(layout->size()) + " leaves)";
                    SEMANTIC_ERROR(
                        message, loc,
                        "Move and restore the complete struct-array element "
                        "as one ownership group"
                    );
                    throw std::runtime_error(message);
                }
            }
        }
    }

    if (functionName == "SetMain" && usesEscapedView) {
        const std::string message =
            "No live alternate-stack binding was produced for '" +
            group.elementLabel + "'";
        SEMANTIC_ERROR(
            message, loc,
            "Call the private producer before restoring this array element"
        );
        throw std::runtime_error(message);
    }

    const bool fromAlt = functionName == "SetMain";
    auto onExpectedStack = [&](const std::string& name) {
        return symtab.getPos(name, fromAlt).has_value();
    };
    auto onOppositeStack = [&](const std::string& name) {
        return symtab.getPos(name, !fromAlt).has_value();
    };

    auto fields = getStructFieldsExpanded(
        arrayInfo->elementType, group.elementLabel, m_structDefinitions
    );
    const bool rootExpected = onExpectedStack(group.elementLabel);
    const bool rootOpposite = onOppositeStack(group.elementLabel);
    size_t expectedFields = 0;
    size_t oppositeFields = 0;
    for (const auto& [fieldName, fieldType] : fields) {
        (void)fieldType;
        expectedFields += onExpectedStack(fieldName) ? 1 : 0;
        oppositeFields += onOppositeStack(fieldName) ? 1 : 0;
    }

    if (rootExpected && !rootOpposite && expectedFields == 0 &&
        oppositeFields == 0) {
        const std::string physicalRoot =
            symtab.resolveBindSymbol(group.elementLabel);
        group.slots.emplace_back(
            physicalRoot, arrayInfo->elementType, physicalRoot
        );
        return group;
    }

    if (!rootExpected && !rootOpposite && !fields.empty() &&
        expectedFields == fields.size() && oppositeFields == 0) {
        for (const auto& [fieldName, fieldType] : fields) {
            const std::string physicalField =
                symtab.resolveBindSymbol(fieldName);
            group.slots.emplace_back(
                physicalField, fieldType, physicalField
            );
        }
        return group;
    }

    const std::string message =
        "Composite array element '" + group.elementLabel +
        "' is missing, ambiguous, or split across main and alternate stacks";
    SEMANTIC_ERROR(
        message, loc,
        "Move or consume the complete struct-array element as one value"
    );
    throw std::runtime_error(message);
}

void ASTToBytecodeVisitor::moveCompositeArrayElementToAlt(
    const ResolvedCompositeArrayElement& group,
    const SourceLocation& loc
)
{
    SymbolTable& symtab = m_scopePtr->getCurrentSymtab();
    std::unordered_set<std::string> uniqueNames;
    for (const auto& slot : group.slots) {
        const std::string& name = slot.getName();
        if (!uniqueNames.insert(name).second ||
            !symtab.getPhysicalPos(name).has_value() ||
            symtab.getPhysicalPos(name, true).has_value()) {
            const std::string message =
                "Cannot move composite element '" + group.elementLabel +
                "' to alternate stack: leaf '" + name +
                "' is missing or has inconsistent residency";
            SEMANTIC_ERROR(
                message, loc,
                "Ensure every composite leaf is present on the main stack"
            );
            throw std::runtime_error(message);
        }
    }

    // Move top-most canonical leaves first. The resulting altstack has the
    // first declaration-order leaf on top, so SetMain can restore leaves in
    // canonical order without reversing the runtime value.
    for (auto it = group.slots.rbegin(); it != group.slots.rend(); ++it) {
        auto position = symtab.getPhysicalPos(it->getName());
        if (!position.has_value()) {
            throw std::runtime_error(
                "composite SetAlt preflight became invalid"
            );
        }
        emitRoll(position.value());
        if (position.value() > 0) {
            m_scopePtr->roll(position.value());
        }
        m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
        symtab.m_altStackPtr->moveTopToStack(
            *symtab.m_stackPtr.get(), true
        );
    }
}

void ASTToBytecodeVisitor::moveCompositeArrayElementToMain(
    const ResolvedCompositeArrayElement& group,
    const SourceLocation& loc
)
{
    SymbolTable& symtab = m_scopePtr->getCurrentSymtab();
    std::unordered_set<std::string> uniqueNames;
    for (const auto& slot : group.slots) {
        const std::string& name = slot.getName();
        if (!uniqueNames.insert(name).second ||
            !symtab.getPhysicalPos(name, true).has_value() ||
            symtab.getPhysicalPos(name).has_value()) {
            const std::string message =
                "Cannot restore composite element '" + group.elementLabel +
                "': leaf '" + name +
                "' is missing or has inconsistent residency";
            SEMANTIC_ERROR(
                message, loc,
                "Restore each alternate-stack binding exactly once"
            );
            throw std::runtime_error(message);
        }
    }

    for (const auto& slot : group.slots) {
        if (!moveAltElementToMain(slot.getName(), false)) {
            throw std::runtime_error(
                "composite SetMain preflight became invalid"
            );
        }
    }
}

void ASTToBytecodeVisitor::deleteCompositeArrayElement(
    const ResolvedCompositeArrayElement& group,
    const SourceLocation& loc
)
{
    SymbolTable& symtab = m_scopePtr->getCurrentSymtab();
    std::unordered_set<std::string> uniqueNames;
    for (const auto& slot : group.slots) {
        if (!uniqueNames.insert(slot.getName()).second ||
            !symtab.getPhysicalPos(slot.getName()).has_value()) {
            const std::string message =
                "Cannot delete composite element '" + group.elementLabel +
                "': leaf '" + slot.getName() + "' is missing";
            SEMANTIC_ERROR(
                message, loc,
                "Restore the complete element before deleting it"
            );
            throw std::runtime_error(message);
        }
    }

    for (auto it = group.slots.rbegin(); it != group.slots.rend(); ++it) {
        auto position = symtab.getPhysicalPos(it->getName());
        if (!position.has_value()) {
            throw std::runtime_error(
                "composite Delete preflight became invalid"
            );
        }
        emitRoll(position.value());
        if (position.value() > 0) {
            m_scopePtr->roll(position.value());
        }
        m_scopePtr->pop();
        m_generator.emit(tbc::BytOpcode::OP_DROP);
    }
}

bool ASTToBytecodeVisitor::tryProcessCompositeArrayBuiltin(
    const std::string& functionName,
    const std::vector<std::unique_ptr<ExprNode>>& args,
    const ExprNode& node
)
{
    if ((functionName != "SetAlt" && functionName != "SetMain" &&
         functionName != "Delete") ||
        args.size() != 1) {
        return false;
    }

    const SourceLocation loc = getNodeLocation(node);
    auto group = resolveCompositeArrayElement(
        *args[0], functionName, loc
    );
    if (!group.has_value()) {
        return false;
    }

    if (functionName == "SetAlt") {
        auto existing = m_escapedAltArrays.find(group->arrayName);
        if (existing != m_escapedAltArrays.end() &&
            (existing->second.arrayInfo.elementType !=
                 group->arrayInfo.elementType ||
             existing->second.arrayInfo.size != group->arrayInfo.size)) {
            const std::string message =
                "Conflicting alternate-stack array schema for '" +
                group->arrayName + "'";
            SEMANTIC_ERROR(
                message, loc,
                "Use a unique channel name for each fixed-array shape"
            );
            throw std::runtime_error(message);
        }

        moveCompositeArrayElementToAlt(group.value(), loc);

        auto [it, inserted] = m_escapedAltArrays.emplace(
            group->arrayName,
            EscapedAltArrayView{group->arrayInfo, {}}
        );
        (void)inserted;
        it->second.elementLayouts[group->elementLabel].push_back(
            group->slots
        );
        return true;
    }

    if (functionName == "SetMain") {
        moveCompositeArrayElementToMain(group.value(), loc);
        if (group->usesEscapedView &&
            !m_scopePtr->importExternalArrayView(
                group->arrayName,
                group->arrayInfo.elementType,
                group->arrayInfo.size,
                group->arrayInfo.isFixedSize
            )) {
            const std::string message =
                "Cannot materialize escaped array view '" +
                group->arrayName + "' in the current scope";
            SEMANTIC_ERROR(
                message, loc,
                "Rename the local array that shadows this altstack channel"
            );
            throw std::runtime_error(message);
        }
        if (group->usesEscapedView) {
            SymbolTable& symtab = m_scopePtr->getCurrentSymtab();
            auto bindExternalLeaf = [&](const std::string& logical,
                                        const std::string& physical) {
                // Repeated SetMain calls in one consumer must replace the
                // previous batch's logical-to-physical mapping.
                symtab.removeBindSymbol(logical);
                if (logical != physical) {
                    std::pair<std::string, std::string> binding(
                        logical, physical
                    );
                    symtab.addBindSymbol(binding);
                }
            };
            if (group->slots.size() == 1) {
                bindExternalLeaf(
                    group->elementLabel, group->slots[0].getName()
                );
            } else {
                auto logicalFields = getStructFieldsExpanded(
                    group->arrayInfo.elementType,
                    group->elementLabel,
                    m_structDefinitions
                );
                if (logicalFields.size() != group->slots.size()) {
                    const std::string message =
                        "Escaped array layout mismatch for '" +
                        group->elementLabel + "'";
                    SEMANTIC_ERROR(
                        message, loc,
                        "Restore the element using the schema captured by "
                        "SetAlt"
                    );
                    throw std::runtime_error(message);
                }
                for (size_t i = 0; i < logicalFields.size(); ++i) {
                    bindExternalLeaf(
                        logicalFields[i].first,
                        group->slots[i].getName()
                    );
                }
            }
        }
        return true;
    }

    deleteCompositeArrayElement(group.value(), loc);
    return true;
}

void ASTToBytecodeVisitor::preserveStructReturn(
    const std::string& rootName,
    const std::string& structType,
    const ReturnNode& node,
    bool descriptorAlreadyOnStack
)
{
    SymbolTable& symbolTable = m_scopePtr->getCurrentSymtab();
    auto symbolicFields = getStructFieldsExpanded(
        structType, rootName, m_structDefinitions
    );
    if (symbolicFields.empty()) {
        std::ostringstream oss;
        oss << "cannot return struct '" << structType
            << "': it has no flattened fields";
        SourceLocation loc("", node.pos.first, node.pos.second);
        SEMANTIC_ERROR(oss.str(), loc, "Check the struct definition");
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    // Struct parameters are bound leaf-by-leaf. Recover the caller-visible
    // root from the first resolved leaf, then enumerate the whole returned
    // value under that root.
    const std::string& firstSymbolicField = symbolicFields.front().first;
    const std::string firstResolvedField =
        symbolTable.resolveBindSymbol(firstSymbolicField);
    const std::string firstSuffix = firstSymbolicField.substr(rootName.size());
    std::string resolvedRoot = rootName;
    if (firstResolvedField.size() >= firstSuffix.size() &&
        firstResolvedField.compare(
            firstResolvedField.size() - firstSuffix.size(),
            firstSuffix.size(),
            firstSuffix
        ) == 0) {
        resolvedRoot = firstResolvedField.substr(
            0, firstResolvedField.size() - firstSuffix.size()
        );
    }

    auto returnedFields = getStructFieldsExpanded(
        structType, resolvedRoot, m_structDefinitions
    );

    // A struct parameter is represented by leaf bindings rather than a root
    // symbol. Record that origin explicitly: caller and callee may legally
    // reuse the same root name, so name equality alone cannot distinguish a
    // caller-owned value from a local composite.
    if (!m_structReturnFrames.empty()) {
        const std::string parameterPrefix = rootName + ".";
        const auto activeBindingBegin =
            symbolTable.m_bindSymbol.begin() +
            static_cast<std::ptrdiff_t>(
                symbolTable.activeBindSymbolStart()
            );
        m_structReturnFrames.back().returnedBoundComposite = std::any_of(
            activeBindingBegin,
            symbolTable.m_bindSymbol.end(),
            [&](const auto& binding) {
                return binding.first.size() > parameterPrefix.size() &&
                       binding.first.compare(
                           0, parameterPrefix.size(), parameterPrefix
                       ) == 0;
            }
        );
    }

    auto keepOnce = [&](const std::string& valueName) {
        if (std::find(
                symbolTable.m_keepSymbol.begin(),
                symbolTable.m_keepSymbol.end(),
                valueName
            ) == symbolTable.m_keepSymbol.end()) {
            symbolTable.m_keepSymbol.push_back(valueName);
        }
    };

    for (const auto& [fieldPath, fieldType] : returnedFields) {
        const std::string resolvedField =
            symbolTable.resolveBindSymbol(fieldPath);

        bool materialized = symbolTable.getPos(resolvedField).has_value();

        if (!materialized) {
            std::string fixedName = resolvedField;
            if (auto fixed = symbolTable.getFixed(fixedName)) {
                std::string script = fixed->getData();
                if (script.empty() || script == resolvedField) {
                    script = fixed->getName();
                }
                if (script.empty() || script == resolvedField) {
                    std::ostringstream oss;
                    oss << "incomplete struct return for '" << resolvedRoot
                        << "': fixed field '" << resolvedField
                        << "' has no materializable value";
                    SourceLocation loc("", node.pos.first, node.pos.second);
                    SEMANTIC_ERROR(
                        oss.str(),
                        loc,
                        "Initialize every returned struct field"
                    );
                    LOG_ERROR(oss.str());
                    throw std::runtime_error(oss.str());
                }

                m_generator.emit(script);
                symbolTable.removeFixed(fixedName);
                symbolTable.push(tbc::StackElement(
                    resolvedField,
                    fixed->getType().empty() ? fieldType : fixed->getType(),
                    fixed->getData()
                ));
                materialized = true;
            }
        }

        if (!materialized) {
            materialized = moveAltElementToMain(resolvedField);
        }

        if (!materialized ||
            !symbolTable.getPos(resolvedField).has_value()) {
            std::ostringstream oss;
            oss << "incomplete struct return for '" << resolvedRoot
                << "': field '" << resolvedField
                << "' is no longer available";
            SourceLocation loc("", node.pos.first, node.pos.second);
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Preserve or Clone every field before returning the struct"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }

        if (!m_structReturnFrames.empty()) {
            m_structReturnFrames.back().returnedFields.insert(resolvedField);
        }
        keepOnce(resolvedField);
    }

    if (descriptorAlreadyOnStack) {
        // A nested private call publishes its descriptor after its own cleanup.
        // Consume that compiler-only slot before this function performs any
        // cleanup, otherwise its non-runtime position would skew ROLL/NIP.
        if (m_scopePtr->empty() ||
            m_scopePtr->top().getName() != rootName ||
            m_scopePtr->top().getType() != structType) {
            std::ostringstream oss;
            oss << "invalid returned struct descriptor for '" << rootName
                << "'";
            SourceLocation loc("", node.pos.first, node.pos.second);
            SEMANTIC_ERROR(oss.str(), loc, "Check the nested struct return");
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }
        m_scopePtr->pop();
    }

    if (!m_structReturnFrames.empty()) {
        if (m_structReturnFrames.back().valueReturnCount > 1) {
            std::ostringstream oss;
            oss << "a struct-returning function cannot contain additional "
                   "lowercase return values";
            SourceLocation loc("", node.pos.first, node.pos.second);
            SEMANTIC_ERROR(
                oss.str(), loc, "Return the complete struct as the only value"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }

        // Publish this descriptor only after block/local/parameter cleanup in
        // privateFunctionResolution(). It has no corresponding runtime value.
        m_structReturnFrames.back().descriptor = tbc::StackElement(
            resolvedRoot, structType, resolvedRoot
        );
    }

    LOG_DEBUG(
        "Preserving returned struct '" + resolvedRoot + "' with " +
        std::to_string(returnedFields.size()) + " flattened field(s)"
    );
}

void ASTToBytecodeVisitor::visit(ReturnNode& node)
{
    LOG_DEBUG("Visiting return node start.");

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    m_currentReturnNode = &node;
    m_lastFlowResult = node.isValueReturn ? FlowResult::InlineReturn
                                          : FlowResult::ScriptTerminate;

    // 大写 Return: 计算表达式并生成返回值; 小写 return: 值已在栈顶, 仅标记.
    if (!node.isValueReturn) {
        if (node.expr) {
            bool isStackVariable = false;

            if (auto identifierNode =
                    dynamic_cast<IdentifierNode*>(node.expr.get())) {
                std::string varName = identifierNode->name;
                auto varPosOpt = m_scopePtr->getPos(varName);

                if (varPosOpt.has_value()) {
                    if (STACK_TOP_POS != varPosOpt.value()) {
                        m_scopePtr->roll(varPosOpt.value());
                        emitRoll(varPosOpt.value());
                    }
                    validateDeclaredType(
                        "return type mismatch",
                        m_currentFunctionReturnType,
                        m_scopePtr->top(),
                        node
                    );
                    LOG_DEBUG("Return stack variable '" + varName + "'");
                    isStackVariable = true;
                } else if (!m_scopePtr->getFixed(varName).has_value()) {
                    // 既不在栈上也不在固定区.
                    std::ostringstream oss;
                    oss << "Return statement references undefined variable '"
                        << varName << "'";
                    SourceLocation loc("", node.pos.first, node.pos.second);
                    SEMANTIC_ERROR(
                        oss.str(), loc, "Variable must be defined before use"
                    );
                    LOG_ERROR(oss.str());
                    throw std::runtime_error(oss.str());
                }
            }

            if (!isStackVariable) {
                bool isLiteralOrFixedVar =
                    dynamic_cast<LiteralNode*>(node.expr.get()) ||
                    (dynamic_cast<IdentifierNode*>(node.expr.get()) &&
                     m_scopePtr
                         ->getFixed(dynamic_cast<IdentifierNode*>(node.expr.get(
                                                                  ))
                                        ->name)
                         .has_value());

                visitExpr(*node.expr);

                if (isLiteralOrFixedVar) {
                    // 固定区变量/字面量: 取栈顶值生成字节码.
                    auto topElement = m_scopePtr->pop();
                    if (topElement.has_value()) {
                        validateDeclaredType(
                            "return type mismatch",
                            m_currentFunctionReturnType,
                            topElement.value(),
                            node
                        );
                        const std::string& data = topElement.value().getData();
                        LOG_DEBUG(
                            "Return expression, emitting bytecode: " + data
                        );
                        m_generator.emit(data);
                    } else {
                        std::ostringstream oss;
                        oss << "Failed to pop value from stack for return "
                               "statement";
                        SourceLocation loc("", node.pos.first, node.pos.second);
                        SEMANTIC_ERROR(
                            oss.str(), loc, "Internal compiler error"
                        );
                        LOG_ERROR(oss.str());
                        throw std::runtime_error(oss.str());
                    }
                } else if (!m_scopePtr->empty()) {
                    const auto& topElement = m_scopePtr->top();
                    validateDeclaredType(
                        "return type mismatch",
                        m_currentFunctionReturnType,
                        topElement,
                        node
                    );

                    const std::string topName = topElement.getName();
                    if (!CompilerPlaceholder::isPlaceholder(topName) &&
                        isScript(topName)) {
                        auto scriptElement = m_scopePtr->pop();
                        if (scriptElement.has_value()) {
                            const std::string& data =
                                scriptElement.value().getData();
                            LOG_DEBUG(
                                "Return script expression, emitting bytecode: " +
                                data
                            );
                            m_generator.emit(data);
                        }
                    }
                }
            }
        } else {
            LOG_DEBUG("Return statement without expression");
        }

        m_generator.emit(tbc::BytOpcode::OP_RETURN);

        // 最终 padding 由 BytecodeFinalizePass 在 peephole 优化后统一处理.
        if (m_currentReturnNode == m_lastReturnNode) {
            LOG_DEBUG("This is the last return statement, deferring padding");
            m_generator.mergeSubOverall();
        } else {
            LOG_DEBUG("This is not the last return statement, skipping padding"
            );
            m_generator.mergeSubOverall();
        }
    } else {
        LOG_DEBUG(
            "Value-return statement (lowercase return): preserving its value"
        );

        if (!m_structReturnFrames.empty()) {
            if (m_structReturnFrames.back().descriptor.has_value()) {
                std::ostringstream oss;
                oss << "a struct-returning function cannot contain additional "
                       "lowercase return values";
                SourceLocation loc("", node.pos.first, node.pos.second);
                SEMANTIC_ERROR(
                    oss.str(),
                    loc,
                    "Return the complete struct as the only value"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }
            ++m_structReturnFrames.back().valueReturnCount;
        }

        // 小写 return 不生成 OP_RETURN，但表达式结果必须跨过当前作用域
        // 清理。已有变量保留原槽；字面量和计算表达式先物化再 keep。
        SymbolTable& symbolTable = m_scopePtr->getCurrentSymtab();
        auto reportNoValue = [&]() {
            std::ostringstream oss;
            oss << "lowercase 'return' expression produced no value";
            SourceLocation loc("", node.pos.first, node.pos.second);
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Return a literal, variable, or value-producing expression"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        };

        auto appendKeep = [&](const std::string& valueName) {
            if (std::find(
                    symbolTable.m_keepSymbol.begin(),
                    symbolTable.m_keepSymbol.end(),
                    valueName
                ) == symbolTable.m_keepSymbol.end()) {
                symbolTable.m_keepSymbol.push_back(valueName);
            }
            if (!m_structReturnFrames.empty() &&
                std::find(
                    m_structReturnFrames.back().returnedValues.begin(),
                    m_structReturnFrames.back().returnedValues.end(),
                    valueName
                ) == m_structReturnFrames.back().returnedValues.end()) {
                m_structReturnFrames.back().returnedValues.push_back(
                    valueName
                );
            }
        };

        auto preserveReturnSymbol = [&](const std::string& varName) {
            auto mainPosOpt = symbolTable.getPos(varName);
            if (mainPosOpt.has_value()) {
                auto& element =
                    symbolTable.m_stackPtr->stacktop(mainPosOpt.value());
                element.setType(declaredSymbolType(
                    symbolTable, varName, element.getType()
                ));
                if (mainPosOpt.value() != STACK_TOP_POS) {
                    emitRoll(mainPosOpt.value());
                    symbolTable.roll(mainPosOpt.value());
                }
                appendKeep(varName);
                return;
            }

            auto altPosOpt = symbolTable.getPos(varName, true);
            if (altPosOpt.has_value()) {
                auto mainStack = symbolTable.m_stackPtr;
                auto altStack = symbolTable.m_altStackPtr;
                auto& element = altStack->stacktop(altPosOpt.value());
                element.setType(declaredSymbolType(
                    symbolTable, varName, element.getType()
                ));

                // lower return 是函数调用结果，必须位于主栈顶部。只移动
                // 目标，并将原先位于目标上方的副栈元素按原序放回。
                for (int64_t i = 0; i <= altPosOpt.value(); ++i) {
                    m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
                    StackElement topElement = altStack->top();
                    altStack->pop();
                    mainStack->push(topElement);
                }
                for (int64_t i = 0; i < altPosOpt.value(); ++i) {
                    emitRoll(1);
                    mainStack->swap(0, 1);
                    m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                    StackElement topElement = mainStack->top();
                    mainStack->pop();
                    altStack->push(topElement);
                }
                appendKeep(varName);
                return;
            }

            std::string mutableName = varName;
            auto fixedElement = symbolTable.getFixed(mutableName);
            if (fixedElement.has_value()) {
                StackElement materialized = fixedElement.value();
                materialized.setName(varName);
                materialized.setType(declaredSymbolType(
                    symbolTable, varName, materialized.getType()
                ));
                if (materialized.getData().empty()) {
                    throw std::runtime_error(
                        "cannot materialize empty fixed return value '" +
                        varName + "'"
                    );
                }
                m_generator.emit(materialized.getData());
                symbolTable.removeFixed(mutableName);
                symbolTable.push(materialized);
                appendKeep(varName);
                return;
            }

            std::ostringstream oss;
            oss << "lowercase return references unavailable variable '"
                << varName << "'";
            SourceLocation loc("", node.pos.first, node.pos.second);
            SEMANTIC_ERROR(
                oss.str(), loc, "Return an existing variable from this scope"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        };

        auto rejectKnownVoidCall = [&](ExprNode& expr) {
            std::string functionName;
            size_t argCount = 0;
            if (auto* call = dynamic_cast<CallNode*>(&expr)) {
                functionName = call->funcName;
                argCount = call->args.size();
            } else if (auto* method = dynamic_cast<MethodCallNode*>(&expr)) {
                functionName = method->methodName;
                argCount = method->args.size();
            } else {
                return;
            }

            if (functionName == "Range") {
                reportNoValue();
            }

            if (auto privateFunction = m_privateFunctions.find(functionName);
                privateFunction != m_privateFunctions.end()) {
                const FunctionNode* function = privateFunction->second;
                const auto arity =
                    function && function->block
                        ? inlineReturnArity(function->block.get())
                        : std::nullopt;
                if (!arity.has_value() || arity.value() == 0) {
                    reportNoValue();
                }
                return;
            }

            if (auto opFunction = tbc::OpFunctionFactory::createFunction(
                    functionName, argCount
                )) {
                if (opFunction->getReturnCount() == 0) {
                    reportNoValue();
                }
                return;
            }

            if (auto builtinFunction =
                    tbc::BuiltinFunctionFactory::createFunction(
                        functionName, argCount
                    )) {
                if (builtinFunction->getReturnCount() == 0) {
                    reportNoValue();
                }
            }
        };

        auto keepExistingOrMaterialize = [&](ExprNode& expr) {
            if (auto* identifier = dynamic_cast<IdentifierNode*>(&expr)) {
                const std::string& varName = identifier->name;

                // Struct values are represented by flattened field slots, not
                // by one runtime root slot. The active function signature is
                // authoritative for parameters; local structs are recovered
                // from the current semantic symbol table.
                std::optional<std::string> structType;
                if (!m_activePrivateFunctions.empty()) {
                    const FunctionNode* activeFunction =
                        m_activePrivateFunctions.back();
                    for (const auto& parameter : activeFunction->parameters) {
                        if (parameter.name == varName &&
                            m_structDefinitions.find(parameter.type) !=
                                m_structDefinitions.end()) {
                            structType = parameter.type;
                            break;
                        }
                    }
                }

                const SymbolTable& currentSymtab =
                    m_scopePtr->getCurrentSymtab();
                if (!structType.has_value()) {
                    const auto symbols = currentSymtab.getCurrentScopeSymbols();
                    for (auto it = symbols.rbegin(); it != symbols.rend(); ++it) {
                        if (it->getSymbolName() != varName) {
                            continue;
                        }
                        const std::string& candidateType =
                            it->m_stackElement.getType();
                        if (m_structDefinitions.find(candidateType) !=
                            m_structDefinitions.end()) {
                            structType = candidateType;
                            break;
                        }
                    }
                }

                if (structType.has_value()) {
                    preserveStructReturn(
                        varName, structType.value(), node, false
                    );
                    return;
                }

                preserveReturnSymbol(identifier->name);
                return;
            }

            auto hasStoredValue = [&](const std::string& name) {
                if (symbolTable.getPos(name).has_value() ||
                    symbolTable.getPos(name, true).has_value()) {
                    return true;
                }
                std::string mutableName = name;
                return symbolTable.getFixed(mutableName).has_value();
            };

            const auto storageName = getAssignmentStorageName(&expr);
            if (storageName.has_value() &&
                hasStoredValue(storageName.value())) {
                preserveReturnSymbol(storageName.value());
                return;
            }

            rejectKnownVoidCall(expr);
            visitExpr(expr);
            if (m_scopePtr->empty()) {
                reportNoValue();
            }

            // Field/index visitors use a virtual reference to an existing
            // stack slot. Remove that model-only reference and preserve the
            // real slot; otherwise block cleanup would DROP the sole value.
            if (storageName.has_value() &&
                m_scopePtr->top().getName() == storageName.value()) {
                const auto reference = m_scopePtr->pop();
                if (hasStoredValue(storageName.value())) {
                    preserveReturnSymbol(storageName.value());
                    return;
                }
                if (reference.has_value()) {
                    m_scopePtr->push(reference.value());
                }
            }

            const StackElement& valueElement = m_scopePtr->top();
            const std::string valueName = valueElement.getName();
            if (m_structDefinitions.find(valueElement.getType()) !=
                m_structDefinitions.end()) {
                // A nested private call already left its compiler-only root
                // descriptor on top. Propagate ownership into this call's
                // return frame without creating a second descriptor.
                preserveStructReturn(
                    valueName, valueElement.getType(), node, true
                );
                return;
            }
            if (valueName.empty()) {
                reportNoValue();
            }
            if (isScript(valueName)) {
                const std::string& data = valueElement.getData();
                m_generator.emit(data.empty() ? valueName : data);
            }
            appendKeep(valueName);
            LOG_DEBUG("Keeping materialized return value: " + valueName);
        };

        if (!node.expr) {
            reportNoValue();
        } else if (auto identifierNode =
                       dynamic_cast<IdentifierNode*>(node.expr.get())) {
            const std::string& varName = identifierNode->name;
            LOG_DEBUG("Marking return value variable as keep: " + varName);
            keepExistingOrMaterialize(*identifierNode);
        }
        // 大括号: 多返回值.
        else if (auto braceExpr = dynamic_cast<BraceExprNode*>(node.expr.get()
                 )) {
            LOG_DEBUG(
                "Processing multi-value return with " +
                std::to_string(braceExpr->elements.size()) + " elements"
            );

            if (braceExpr->elements.empty()) {
                reportNoValue();
            }
            for (const auto& element : braceExpr->elements) {
                if (!element) {
                    reportNoValue();
                }
                keepExistingOrMaterialize(*element);
                if (!m_structReturnFrames.empty() &&
                    m_structReturnFrames.back().descriptor.has_value()) {
                    std::ostringstream oss;
                    oss << "struct values cannot participate in a lowercase "
                           "multi-value return";
                    SourceLocation loc("", node.pos.first, node.pos.second);
                    SEMANTIC_ERROR(
                        oss.str(),
                        loc,
                        "Return the complete struct as the only value"
                    );
                    LOG_ERROR(oss.str());
                    throw std::runtime_error(oss.str());
                }
            }
        } else {
            keepExistingOrMaterialize(*node.expr);
        }
    }

    LOG_DEBUG("Visiting return node end.");
}

void ASTToBytecodeVisitor::visit(VarDeclNode& node)
{
    LOG_DEBUG(
        "Visiting variable declaration node start. Variable name: " +
        node.name + ", type: " + node.type
    );

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);

    // 变量真正定义后再记录调试信息, 确保 stackOffset 正确.
    auto emitVarDebugInfo = [&](const std::string& varName) {
        if (!m_debugInfoGen) {
            return;
        }

        apc_debug::SourceLocation loc = extractDebugLocation(node);

        int stackOffset = -1;
        auto posOpt = m_scopePtr->getPos(varName);
        if (posOpt.has_value()) {
            stackOffset = static_cast<int>(posOpt.value());
        }

        bool isStackVar = stackOffset >= 0;

        m_debugInfoGen->onVariableDecl(
            varName,
            node.type,
            loc,
            isStackVar,
            stackOffset,
            false // isParameter
        );
    };
#endif

    if (node.isCompoundType) {
        LOG_DEBUG("Processing compound type variable: " + node.name);

        CompoundTypeInfo compoundInfo(
            node.name,
            node.compoundFields,
            false // 非结构体字段.
        );

        if (!m_scopePtr->defineCompoundType(compoundInfo)) {
            SourceLocation loc = getNodeLocation(node);
            std::string errorMsg = "Failed to define compound type variable '" +
                                   node.name + "'";
            SEMANTIC_ERROR(errorMsg, loc, "Variable may already be defined");
            LOG_ERROR(errorMsg);
            throw std::runtime_error(errorMsg);
        }

        LOG_DEBUG(
            "Compound type variable defined: " + node.name + " with " +
            std::to_string(node.compoundFields.size()) + " fields"
        );

        if (node.initValue) {
            LOG_DEBUG(
                "Processing initialization value for compound type: " +
                node.name
            );

            visitExpr(*node.initValue);

            auto valueElementOpt = m_scopePtr->pop();
            if (!valueElementOpt.has_value()) {
                std::ostringstream errorStream;
                errorStream
                    << "No processing results for initialization expression"
                    << " at line " << node.pos.first << ", column "
                    << node.pos.second;
                SourceLocation loc = getNodeLocation(node);
                SEMANTIC_ERROR(
                    errorStream.str(),
                    loc,
                    "Check the initialization expression syntax"
                );
                LOG_ERROR(errorStream.str());
                throw std::runtime_error(errorStream.str());
            }

            auto valueElement = valueElementOpt.value();
            auto valueElementStr = valueElement.getName();

            // 复合类型不 push 到栈上, 用零成本重命名保留在原位.
            bool renameSuccess = m_scopePtr->rename(valueElementStr, node.name);
            if (renameSuccess) {
                LOG_DEBUG(
                    "Compound type zero-cost rename: \"",
                    valueElementStr,
                    "\" -> \"",
                    node.name,
                    "\""
                );
                m_scopePtr->markSymbolInitialized(node.name);
            } else if (CompilerPlaceholder::isPlaceholder(valueElementStr)) {
                m_scopePtr->push(
                    node.name, valueElement.getType(), valueElement.getData()
                );
                LOG_DEBUG(
                    "Compound type placeholder rename: \"",
                    valueElementStr,
                    "\" -> \"",
                    node.name,
                    "\""
                );
            } else {
                SourceLocation loc = getNodeLocation(node);
                std::string errorMsg =
                    "compound type variable '" + node.name +
                    "' cannot be initialized with a constant or "
                    "non-compound value";
                SEMANTIC_ERROR(
                    errorMsg,
                    loc,
                    "Initialize from another compound variable of the same "
                    "shape, or assign fields individually after declaration"
                );
                LOG_ERROR(errorMsg);
                throw std::runtime_error(errorMsg);
            }
        }

        LOG_DEBUG("Compound type variable declaration completed: " + node.name);

#ifdef ENABLE_DEBUGGER
        emitVarDebugInfo(node.name);
#endif

        return;
    }

    auto declaredArrayType = apc::util::parseFixedArrayType(node.type);
    bool isArrayType = declaredArrayType.has_value();

    if (isArrayType) {
        // uint64[N]: 注册为整体数组元素.
        if (declaredArrayType->elementType == "uint64") {
            size_t arraySize = declaredArrayType->size;
            size_t elementByteSize = 8;

            registerWholeArrayElement(node.name, arraySize, elementByteSize);

            LOG_DEBUG(
                "Registered uint64[] variable '" + node.name + "' with size " +
                std::to_string(arraySize) + " as whole array element"
            );
        }
    } else {
        if (m_scopePtr->symbolExists(node.name)) {
            SourceLocation loc = getNodeLocation(node);
            std::string warningMsg = "variable '" + node.name +
                                     "' already defined, redefinition ignored";
            COMPILER_WARNING(warningMsg, loc);
            LOG_WARNING("Variable already defined: " + node.name);
        } else {
            m_scopePtr->defineSymbol(node.name, node.type);
            LOG_DEBUG(
                "Variable defined: " + node.name + " with type: " + node.type
            );
        }

        // 结构体: 注册其中的 uint64 数组字段.
        if (m_structDefinitions.find(node.type) != m_structDefinitions.end()) {
            LOG_DEBUG(
                "Variable '" + node.name + "' is of struct type '" + node.type +
                "', checking for uint64 array fields"
            );

            // 扁平字段列表: 嵌套结构体中的 uint64[N] 字段也能注册.
            auto flatFields = getStructFieldsExpanded(
                node.type, node.name, m_structDefinitions
            );
            for (const auto& p : flatFields) {
                const std::string& fieldPath = p.first;
                const std::string& fieldType = p.second;

                // uint64[N] 保持整体: 注册为整体数组元素.
                if (auto fieldArray = apc::util::parseFixedArrayType(fieldType);
                    fieldArray && fieldArray->elementType == "uint64") {
                        size_t arraySize = fieldArray->size;
                        registerWholeArrayElement(fieldPath, arraySize, 8);
                        LOG_DEBUG(
                            "Registered uint64 array field '" + fieldPath +
                            "' with size " + std::to_string(arraySize) +
                            " as whole array element (flattened)"
                        );
                }
            }
        }

        // 结构体无初值: 预先定义所有字段, 便于后续 data.value = 5 这类赋值.
        if (!node.initValue &&
            m_structDefinitions.find(node.type) != m_structDefinitions.end()) {
            LOG_DEBUG(
                "Struct variable '" + node.name +
                "' declared without initial value, "
                "pre-defining all fields"
            );

            auto flatFields = getStructFieldsExpanded(
                node.type, node.name, m_structDefinitions
            );
            for (const auto& fieldInfo : flatFields) {
                std::string fieldPath = fieldInfo.first;
                const std::string& fieldType = fieldInfo.second;

                if (!m_scopePtr->symbolExists(fieldPath)) {
                    m_scopePtr->defineSymbol(fieldPath);
                    LOG_DEBUG("Pre-defined struct field: " + fieldPath);
                }

                // 扁平化的 uint64[N] 字段: 补充整体数组元素注册.
                if (auto fieldArray = apc::util::parseFixedArrayType(fieldType);
                    fieldArray && fieldArray->elementType == "uint64") {
                        size_t arraySize = fieldArray->size;
                        registerWholeArrayElement(fieldPath, arraySize, 8);
                        LOG_DEBUG(
                            "Registered uint64 array field '" + fieldPath +
                            "' with size " + std::to_string(arraySize) +
                            " as whole array element (predefine)"
                        );
                }
            }
        }
    }

    if (node.initValue) {
        LOG_DEBUG("Processing initialization value for variable: " + node.name);

        // 结构体大括号初始化: data: Data = {a, b, ...}.
        if (auto braceExpr = dynamic_cast<BraceExprNode*>(node.initValue.get()
            )) {
            auto structDefIt = m_structDefinitions.find(node.type);
            if (structDefIt != m_structDefinitions.end()) {
                // 扁平字段列表: 支持数组与嵌套结构体逐元素递归初始化.
                auto flatFields = getStructFieldsExpanded(
                    node.type, node.name, m_structDefinitions
                );
                if (flatFields.empty()) {
                    SourceLocation loc = getNodeLocation(node);
                    std::ostringstream oss;
                    oss << "failed to flatten struct '" << node.type
                        << "' for brace initialization of '" << node.name
                        << "'";
                    SEMANTIC_ERROR(
                        oss.str(),
                        loc,
                        "Check struct definition and nested field types"
                    );
                    LOG_ERROR(oss.str());
                    throw std::runtime_error(oss.str());
                }

                if (braceExpr->elements.size() != flatFields.size()) {
                    SourceLocation loc = getNodeLocation(node);
                    std::ostringstream oss;
                    oss << "brace initializer count mismatch for struct '"
                        << node.type << "': expected " << flatFields.size()
                        << " value(s) (flattened fields) but got "
                        << braceExpr->elements.size();
                    SEMANTIC_ERROR(
                        oss.str(),
                        loc,
                        "Provide exactly one initializer per struct field in "
                        "flattened declaration order"
                    );
                    LOG_ERROR(oss.str());
                    throw std::runtime_error(oss.str());
                }

                LOG_DEBUG(
                    "Detected struct brace initialization for variable: " +
                    node.name + " of type: " + node.type
                );

                // 仅 defineSymbol 注册扁平字段, 不做幽灵 push:
                // 赋值前 getPos 返回 nullopt, 让 applyLeafFieldAssignment
                // 触发零成本重命名 (复合字段按整体单槽语义).
                for (const auto& fieldInfo : flatFields) {
                    std::string fieldPath = fieldInfo.first;

                    if (!m_scopePtr->symbolExists(fieldPath)) {
                        m_scopePtr->defineSymbol(fieldPath);
                        LOG_DEBUG(
                            "Defined flattened struct field: " + fieldPath
                        );
                    }
                }

                visitExpr(*node.initValue);

                std::vector<tbc::StackElement> rhsValues;
                rhsValues.reserve(flatFields.size());
                for (size_t k = 0; k < flatFields.size(); ++k) {
                    auto rhsOpt = m_scopePtr->pop();
                    if (!rhsOpt.has_value()) {
                        SourceLocation loc = getNodeLocation(node);
                        std::ostringstream oss;
                        oss << "not enough elements on stack while "
                               "initializing struct '"
                            << node.type << "' (expected " << flatFields.size()
                            << " values)";
                        SEMANTIC_ERROR(
                            oss.str(),
                            loc,
                            "Ensure brace values count matches struct fields"
                        );
                        LOG_ERROR(oss.str());
                        throw std::runtime_error(oss.str());
                    }
                    rhsValues.push_back(rhsOpt.value());
                }

                // 按声明顺序逐一赋值; rhsValues[0] 是原栈顶 (最后一个
                // brace 元素), 需反向索引.
                for (size_t i = 0; i < flatFields.size(); ++i) {
                    const std::string fieldPath = flatFields[i].first;
                    const std::string expectedType = flatFields[i].second;
                    const auto& rhsVal = rhsValues[flatFields.size() - 1 - i];
                    applyLeafFieldAssignment(
                        fieldPath, expectedType, rhsVal, node
                    );
                    LOG_DEBUG("Initialized field: " + fieldPath);
                }

                LOG_DEBUG(
                    "Struct brace initialization completed for: " + node.name
                );
                m_scopePtr->markSymbolInitialized(node.name);
                LOG_DEBUG(
                    "Visiting variable declaration node end. Variable: " +
                    node.name
                );

#ifdef ENABLE_DEBUGGER
                emitVarDebugInfo(node.name);
#endif

                return;
            }
        }

        // 非结构体大括号初始化: 走通用流程.
        visitExpr(*node.initValue);

        auto valueElementOpt = m_scopePtr->pop();
        if (!valueElementOpt.has_value()) {
            std::ostringstream errorStream;
            errorStream << "No processing results for initialization expression"
                        << " at line " << node.pos.first << ", column "
                        << node.pos.second;
            SourceLocation loc = getNodeLocation(node);
            SEMANTIC_ERROR(
                errorStream.str(),
                loc,
                "Check the initialization expression syntax"
            );
            LOG_ERROR(errorStream.str());
            throw std::runtime_error(errorStream.str());
        }

        auto valueElement = valueElementOpt.value();
        auto valueElementStr = valueElement.getName();

        if (m_structDefinitions.find(valueElement.getType()) !=
            m_structDefinitions.end()) {
            if (node.type != valueElement.getType()) {
                SourceLocation loc = getNodeLocation(node);
                std::ostringstream oss;
                oss << "cannot initialize struct variable '" << node.name
                    << "' of type '" << node.type
                    << "' with returned struct type '"
                    << valueElement.getType() << "'";
                SEMANTIC_ERROR(
                    oss.str(), loc, "Use the same struct type on both sides"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }

            if (valueElementStr == node.name) {
                m_scopePtr->markSymbolInitialized(node.name);
                LOG_DEBUG(
                    "Struct variable initialization preserved returned "
                    "struct with the same identity: " +
                    node.name
                );
#ifdef ENABLE_DEBUGGER
                emitVarDebugInfo(node.name);
#endif
                return;
            }

            if (!transferCompositeIdentity(valueElementStr, node.name)) {
                SourceLocation loc = getNodeLocation(node);
                std::ostringstream oss;
                oss << "failed to initialize struct variable '" << node.name
                    << "' from returned struct '" << valueElementStr << "'";
                SEMANTIC_ERROR(
                    oss.str(),
                    loc,
                    "Ensure every returned struct field is available"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }

            m_scopePtr->markSymbolInitialized(node.name);
            LOG_DEBUG(
                "Struct variable initialization completed from returned "
                "struct: " +
                node.name
            );
#ifdef ENABLE_DEBUGGER
            emitVarDebugInfo(node.name);
#endif
            return;
        }

        bool isScriptElement = isScript(valueElementStr);

        validateDeclaredType(
            "type mismatch in variable declaration",
            node.type,
            valueElement,
            node,
            isArrayType
        );

        // 零成本重命名: 不移动栈数据.
        if (!CompilerPlaceholder::isPlaceholder(valueElementStr) &&
            !isScriptElement) {
            bool renameSuccess = m_scopePtr->rename(valueElementStr, node.name);
            if (renameSuccess) {
                LOG_DEBUG(
                    "Zero-cost rename: \"",
                    valueElementStr,
                    "\" -> \"",
                    node.name,
                    "\""
                );
#ifdef ENABLE_DEBUGGER
                emitVarDebugInfo(node.name);
#endif
                return;
            } else {
                // 后备: 传统 OP_ROLL 移动.
                auto elementPosOpt = m_scopePtr->getPos(valueElementStr);
                if (elementPosOpt.has_value() &&
                    STACK_TOP_POS != elementPosOpt.value()) {
                    emitRoll(elementPosOpt.value());
                    LOG_INFO(
                        "The variable \"", valueElementStr, "\" becomes invalid"
                    );
                }
            }
        } else if (valueElementStr.find("[") != std::string::npos &&
                   valueElementStr.find("]") != std::string::npos) {
            // 数组元素访问: rename 而非复制.
            bool renameSuccess = m_scopePtr->rename(valueElementStr, node.name);
            if (renameSuccess) {
                LOG_DEBUG(
                    "Zero-cost array element rename: \"",
                    valueElementStr,
                    "\" -> \"",
                    node.name,
                    "\""
                );
#ifdef ENABLE_DEBUGGER
                emitVarDebugInfo(node.name);
#endif
                return;
            } else {
                LOG_DEBUG(
                    "Array element value already at stack top: " +
                    valueElementStr
                );
            }
        }

        if (isScriptElement) {
            valueElement.setName(node.name);
            // script 元素: 设为固定变量映射.
            m_scopePtr->setFixed(valueElement);
        } else {
            m_scopePtr->push(
                node.name, valueElement.getType(), valueElement.getData()
            );
        }

        if (!CompilerPlaceholder::isPlaceholder(valueElementStr) &&
            !isScriptElement) {
            auto elementPosOpt = m_scopePtr->getPos(valueElementStr);
            if (elementPosOpt.has_value()) {
                m_scopePtr->roll(elementPosOpt.value());
                m_scopePtr->pop();
            }
        }

        LOG_DEBUG("Variable " + node.name + " initialized with value");
    } else {
        LOG_DEBUG("Variable " + node.name + " declared without initial value");
    }

#ifdef ENABLE_DEBUGGER
    emitVarDebugInfo(node.name);
#endif

    LOG_DEBUG("Visiting variable declaration node end. Variable: " + node.name);
}

void ASTToBytecodeVisitor::visitLiteral(LiteralNode& node)
{
    LOG_DEBUG("Visiting literal node start. value: " + node.value);

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    BytecodeType inferredType = inferLiteralType(node);

    if (LiteralNode::Type::Boolean != inferredType &&
        !tbc::TypeValidator::validateType(inferredType, node.value)) {
        reportTypeError("Literal validation", inferredType, node.value);
        return;
    }

    // bytecode 无类型, 所有数据统一转 hex.
    switch (node.type) {
        case LiteralNode::Type::Number: {
            int64_t numberValue = 0;
            try {
                numberValue = std::stoll(node.value);
            } catch (const std::exception&) {
                SourceLocation loc = getNodeLocation(node);
                std::ostringstream oss;
                oss << "integer literal out of range: '" << node.value << "'";
                TYPE_ERROR(
                    oss.str(),
                    loc,
                    "Use an integer literal that fits in the compiler's "
                    "supported numeric range"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }

            std::string numberHex = numberToScriptHex(numberValue);
            LOG_DEBUG("Pushing number to stack, hex: " + numberHex);
            m_scopePtr->push(numberHex, "num", numberHex);
            break;
        }
        case LiteralNode::Type::String: {
            std::string stringHex = stringToScriptHex(node.value);

            if (!tbc::TypeValidator::validateSize(
                    BytecodeType::String, stringHex.length() / 2
                )) {
                reportTypeError(
                    "String length check", BytecodeType::String, node.value
                );
                return;
            }

            LOG_DEBUG("Pushing string to stack, hex: " + stringHex);
            m_scopePtr->push(stringHex, "string", stringHex);
            break;
        }
        case LiteralNode::Type::Boolean: {
            std::string elementFlag = node.value == "true" ? "0x00" : "0x51";
            LOG_DEBUG("Pushing boolean to stack: " + elementFlag);
            m_scopePtr->push(elementFlag, "bool", elementFlag);
            break;
        }
        case LiteralNode::Type::Addr: {
            // 地址: P2PKH 解码为 20 字节公钥哈希.
            std::string pubkeyHashHex = tbc::decodeP2PKHAddress(node.value);
            if (pubkeyHashHex.empty()) {
                reportTypeError(
                    "Invalid P2PKH address format",
                    BytecodeType::Addr,
                    node.value
                );
                return;
            }

            // 公钥哈希长度: 40 个十六进制 = 20 字节.
            if (pubkeyHashHex.length() != 40) {
                reportTypeError(
                    "Invalid public key hash length",
                    BytecodeType::Addr,
                    node.value
                );
                return;
            }

            // 20 字节公钥哈希 -> 脚本格式.
            std::string lengthOpcode = tbc::bytEncodeLengthOpcode(20);
            std::string addressHex = "0x" + lengthOpcode + pubkeyHashHex;

            LOG_DEBUG(
                "Pushing address to stack, decoded pubkey hash: " +
                pubkeyHashHex
            );
            m_scopePtr->push(addressHex, "address", pubkeyHashHex);
            break;
        }
        case LiteralNode::Type::PubKey:
        case LiteralNode::Type::Sha256:
        case LiteralNode::Type::Ripemd160:
        case LiteralNode::Type::PrivKey:
        case LiteralNode::Type::Sig:
        case LiteralNode::Type::Hex: {
            if (!tbc::TypeValidator::isValidHex(node.value)) {
                reportTypeError(
                    "Hexadecimal format check", BytecodeType::Hex, node.value
                );
                return;
            }

            int dataLength = static_cast<int>(
                tbc::TypeValidator::getHexDataSize(node.value)
            );

            // 通用长度验证; 具体限制在变量声明处检查.
            if (!tbc::TypeValidator::validateSize(
                    BytecodeType::Hex, dataLength
                )) {
                reportTypeError(
                    "Hexadecimal data length check",
                    BytecodeType::Hex,
                    node.value
                );
                return;
            }

            auto hexOpValue = hexToScriptHex(node.value);
            if (hexOpValue.empty()) {
                std::ostringstream errorStream;
                errorStream
                    << "Failed to process hexadecimal constant: " << node.value
                    << " at line " << node.pos.first << ", column "
                    << node.pos.second;
                SourceLocation loc("", node.pos.first, node.pos.second);
                SEMANTIC_ERROR(
                    errorStream.str(), loc, "Check hexadecimal format and value"
                );
                LOG_ERROR(errorStream.str());
                throw std::runtime_error(errorStream.str());
            } else {
                LOG_DEBUG("Pushing hex data to stack, value: " + hexOpValue);
            }

            m_scopePtr->push(hexOpValue, "hex", hexOpValue);
            break;
        }
        default:
            SourceLocation loc = getNodeLocation(node);
            std::string warningMsg = "unknown literal type encountered";
            COMPILER_WARNING(warningMsg, loc);
            LOG_WARNING(warningMsg);
            break;
    }
    LOG_DEBUG("Visiting literal node end.");
}

void ASTToBytecodeVisitor::visitIdentifier(IdentifierNode& node)
{
    DEFER([node]() {
        LOG_DEBUG("Visiting identifier node end. name: " + node.name);
    });
    LOG_DEBUG("Visiting identifier node start. name: " + node.name);

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    if ("self" == node.name) {
        m_scopePtr->push("<" + node.name + ">", "", "<" + node.name + ">");
        return;
    }

    // 形参绑定到 <self.X> (builtin_member) 实参时, 每次访问都把名
    // push 到符号栈 (不 emit, 由后续 lamd 在 isScript 分支 emit, 与主函数中
    // self.X 字段访问路径一致). 这样 `x * x` 多次引用形参的表达式不会
    // 因 zero-cost rename 丢失第二次 push 而栈下溢. 配合 processArgsToTop
    // 对 <self.X> 实参的预 push 跳过. CompilerPlaceholder 实参不走这条
    // 分支, 走下面普通栈元素查找路径 (受 move 语义).
    {
        StackElement probe(node.name);
        m_scopePtr->getPos(probe);
        const std::string& boundName = probe.getName();
        if (boundName != node.name) {
            bool isBuiltinMemberOpcode =
                boundName.size() >= 2 && boundName.front() == '<' &&
                boundName.back() == '>';
            if (isBuiltinMemberOpcode) {
                LOG_DEBUG(
                    "Bound param '" + node.name +
                    "' -> builtin_member, push name: " + boundName
                );
                m_scopePtr->push(boundName, "builtin_member", boundName);
                return;
            }
        }
    }

    auto stackPos = m_scopePtr->getPos(node.name);
    if (stackPos.has_value()) {
        LOG_DEBUG(
            "Found variable '" + node.name +
            "' on stack at position: " + std::to_string(stackPos.value())
        );
        m_scopePtr->push(node.name, "", node.name);
        return;
    }

    // 栈上没找到, 尝试 fixed 区.
    if (auto fixedVar = m_scopePtr->getFixed(node.name)) {
        LOG_DEBUG(
            "Found fixed variable: " + fixedVar.value().getName() +
            " with value: " + fixedVar.value().getData()
        );

        // data 包含实际值时直接推入, 否则推变量名.
        const std::string& data = fixedVar.value().getData();
        if (!data.empty() && data != node.name) {
            LOG_DEBUG("Using fixed variable data: " + data);
            m_scopePtr->push(data, fixedVar.value().getType(), data);
        } else {
            LOG_DEBUG("Using fixed variable name: " + node.name);
            m_scopePtr->push(node.name, fixedVar.value().getType(), node.name);
        }
        return;
    }

    // 栈和 fixed 都没有: 当作未定义变量或参数, 推入变量名.
    LOG_DEBUG(
        "Program at: ",
        node.pos.first,
        ",",
        node.pos.second,
        " accessing identifier '",
        node.name,
        "' (not found in stack or fixed area)"
    );
    m_scopePtr->push(node.name, "", node.name);
}

void ASTToBytecodeVisitor::visitOperator(OpNode& node)
{
    DEFER([]() { LOG_DEBUG("Visiting op node end."); });
    LOG_DEBUG("Visiting op node start. op: " + node.op);

    // Fold complete numeric expressions over fixed bindings (notably static
    // loop targets). Runtime slots remain authoritative and force the normal
    // stack-aware lowering path.
    const auto staticValue = evaluateCompileTimeInteger(node, true);
    if (staticValue.isError()) {
        SourceLocation loc = getNodeLocation(node);
        SEMANTIC_ERROR(
            staticValue.diagnostic,
            loc,
            "Keep the compile-time integer expression within int64 bounds"
        );
        throw std::runtime_error(staticValue.diagnostic);
    }
    if (staticValue.isKnown()) {
        LiteralNode folded(
            LiteralNode::Type::Number,
            std::to_string(staticValue.value),
            node.pos.first,
            node.pos.second
        );
        visitLiteral(folded);
        return;
    }

    auto lamd = [this](
                    std::optional<StackElement> elementOpt,
                    std::vector<std::string>& operMap,
                    const ASTNode& node,
                    std::string str
                ) {
        if (!elementOpt.has_value()) {
            std::ostringstream errorStream;
            errorStream << "No processing results for " << str
                        << " expression at line " << node.pos.first
                        << ", column " << node.pos.second;
            SourceLocation loc("", node.pos.first, node.pos.second);
            SEMANTIC_ERROR(
                errorStream.str(),
                loc,
                "Check the expression syntax and operands"
            );
            LOG_ERROR(errorStream.str());
            throw std::runtime_error(errorStream.str());
        }
        auto element = elementOpt.value();
        auto elementStr = element.getName();
        operMap.push_back(elementStr);

        if (isScript(elementStr) ||
            CompilerPlaceholder::isPlaceholder(elementStr)) {
            if (isScript(elementStr)) {
                m_generator.emit(elementStr);
            }
            CompilerPlaceholder ph;
            m_scopePtr->push(ph.toString(), "", "");
            return;
        }

        auto stackPos = m_scopePtr->getPos(elementStr);
        if (!stackPos.has_value()) {
            LOG_DEBUG(
                "Element not found in stack, trying to get from fixed area: " +
                elementStr
            );
            std::string elementStrRef = elementStr; // getFixed 需非 const 引用.
            auto fixedElementOpt = m_scopePtr->getFixed(elementStrRef);
            if (fixedElementOpt.has_value()) {
                LOG_DEBUG("Found element in fixed area: " + elementStr);
                m_scopePtr->push(
                    elementStr,
                    fixedElementOpt.value().getType(),
                    fixedElementOpt.value().getData()
                );
                return;
            }

            SourceLocation loc("", node.pos.first, node.pos.second);
            std::ostringstream errorStream;
            errorStream << "unable to find element '" << elementStr
                        << "' position on the stack";
            SEMANTIC_ERROR(
                errorStream.str(),
                loc,
                "Check if the variable is properly declared and in scope"
            );
            LOG_ERROR(errorStream.str());
            throw std::runtime_error(errorStream.str());
        }
        auto vmPos = stackPos.value();
        m_scopePtr->roll(stackPos.value());

        if (STACK_TOP_POS != vmPos) {
            emitRoll(vmPos);
        }
    };

    std::vector<std::string> operVec;

    if (node.lhs == nullptr) {
        LOG_DEBUG("Processing unary operator: " + node.op);

        // -literal 已被 ConstantFolder 折成 Literal, 此处只会遇到 -expr.
        if (node.op == "-") {
            visitExpr(*node.rhs);
            auto elementOpt = m_scopePtr->pop();
            lamd(elementOpt, operVec, *node.rhs, "rhs");

            m_generator.emit(tbc::BytOpcode::OP_NEGATE);
            return;
        }

        visitExpr(*node.rhs);
        auto elementOpt = m_scopePtr->pop();
        lamd(elementOpt, operVec, *node.rhs, "rhs");
    } else {
        LOG_DEBUG("Processing binary operator: " + node.op);

        // 纯字面量已折叠, 此处只处理含变量的情况.
        auto isCommutativeOp = [](const std::string& op) -> bool {
            return op == "+" || op == "*" || op == "==" || op == "!=" ||
                   op == "&&" || op == "||";
        };

        // 优化: 两操作数均为不同标识符且都在主栈时, 预查栈位置避免多余 ROLL.
        // L=1,R=0: 已在 [rhs@0,lhs@1], 零 ROLL.
        // R=0,L>1 且可交换: rhs 已在栈顶, 只 roll lhs 一次.
        bool operandsHandled = false;
        auto* lhsIdent = dynamic_cast<IdentifierNode*>(node.lhs.get());
        auto* rhsIdent = dynamic_cast<IdentifierNode*>(node.rhs.get());

        // 同名标识符特例: a * a, x + x 等. 普通栈变量 / CompilerPlaceholder
        // 受 move 语义约束 (zero-cost rename 模型, 用过即消), 第二次 visit
        // 同一 a 会找到同源副本而非独立第二份, 默认路径要么符号栈下溢 (单参
        // Internal error) 要么静默借用调用方入参凑数 (multi-arg / Push /
        // Clone miscompile). 这里在底层 emit 之前给出明确语义错误. 仅
        // <self.X> builtin_member 不占栈位, 允许重复读, 由专门分支处理.
        if (lhsIdent && rhsIdent && lhsIdent->name == rhsIdent->name) {
            StackElement probe(lhsIdent->name);
            m_scopePtr->getPos(probe);
            const std::string& boundName = probe.getName();
            bool isBuiltinMemberOpcode =
                boundName.size() >= 2 && boundName.front() == '<' &&
                boundName.back() == '>';

            if (!isBuiltinMemberOpcode &&
                m_scopePtr->getPos(lhsIdent->name).has_value()) {
                SourceLocation loc("", node.pos.first, node.pos.second);
                std::ostringstream errorStream;
                errorStream << "variable '" << lhsIdent->name
                            << "' is consumed more than once in the same "
                               "expression (move semantics violation)";
                SEMANTIC_ERROR(
                    errorStream.str(),
                    loc,
                    "AtomicProof stack variables follow move semantics: "
                    "each name binds to one stack slot and is consumed on "
                    "use. To reuse a value, call .Clone() to obtain an "
                    "independent copy first. Contract members (<self.X>) "
                    "are the only exception."
                );
                LOG_ERROR(errorStream.str());
                throw std::runtime_error(errorStream.str());
            }
        }

        if (lhsIdent && rhsIdent && lhsIdent->name != rhsIdent->name) {
            auto lhsPosOpt = m_scopePtr->getPos(lhsIdent->name);
            auto rhsPosOpt = m_scopePtr->getPos(rhsIdent->name);

            if (lhsPosOpt.has_value() && rhsPosOpt.has_value()) {
                const int64_t L = lhsPosOpt.value();
                const int64_t R = rhsPosOpt.value();

                if (L == 1 && R == 0) {
                    operVec.push_back(lhsIdent->name);
                    operVec.push_back(rhsIdent->name);
                    operandsHandled = true;
                } else if (L == 0 && R == 1) {
                    // 相反位置: 可交换零 ROLL, 否则一次 swap (emitRoll(1) -> OP_SWAP).
                    if (isCommutativeOp(node.op)) {
                        operVec.push_back(lhsIdent->name);
                        operVec.push_back(rhsIdent->name);
                        operandsHandled = true;
                    } else {
                        operVec.push_back(lhsIdent->name);
                        operVec.push_back(rhsIdent->name);
                        emitRoll(1);
                        m_scopePtr->roll(1); // rhs: 1->0, lhs 右移到 1.
                        operandsHandled = true;
                    }
                } else if (R == 0 && L > 1 && isCommutativeOp(node.op)) {
                    // 可交换 + rhs 已在栈顶: 只 roll lhs 一次.
                    operVec.push_back(rhsIdent->name);
                    operVec.push_back(lhsIdent->name);
                    // rhs 已在 pos 0, 仅 roll lhs 一次 (L==2 自动 OP_ROT).
                    emitRoll(L);
                    m_scopePtr->roll(L); // lhs: L->0, rhs 右移到 1.
                    operandsHandled = true;
                } else if (L == 0 && R > 1 && isCommutativeOp(node.op)) {
                    // 可交换 + lhs 已在栈顶: 只 roll rhs 一次.
                    operVec.push_back(lhsIdent->name);
                    operVec.push_back(rhsIdent->name);
                    emitRoll(R);
                    m_scopePtr->roll(R); // rhs: R->0, lhs 右移到 1.
                    operandsHandled = true;
                }
            }
        }

        if (!operandsHandled) {
            // 优化: x+1 -> OP_1ADD, x-1 -> OP_1SUB (省 1 字节).
            auto isLiteralOne = [](ExprNode* e) -> bool {
                auto* lit = dynamic_cast<LiteralNode*>(e);
                return lit && lit->type == LiteralNode::Type::Number &&
                       lit->value == "1";
            };

            auto isLiteralValue = [](ExprNode* e, int64_t val) -> bool {
                auto* lit = dynamic_cast<LiteralNode*>(e);
                if (!lit || lit->type != LiteralNode::Type::Number) {
                    return false;
                }
                try {
                    return std::stoll(lit->value) == val;
                } catch (const std::exception&) {
                    return false;
                }
            };

            if (node.op == "+" || node.op == "-") {
                bool rhsIsOne = node.rhs && isLiteralOne(node.rhs.get());
                // + 可交换, lhs=1 也优化; - 不可交换, 仅 rhs=1.
                bool lhsIsOne = node.op == "+" && node.lhs &&
                                isLiteralOne(node.lhs.get());

                if (rhsIsOne || lhsIsOne) {
                    ExprNode* mainExpr = rhsIsOne ? node.lhs.get()
                                                  : node.rhs.get();
                    auto op1Code = (node.op == "+") ? tbc::BytOpcode::OP_1ADD
                                                    : tbc::BytOpcode::OP_1SUB;

                    visitExpr(*mainExpr);
                    auto elementOpt = m_scopePtr->pop();
                    lamd(elementOpt, operVec, *mainExpr, "lhs");

                    m_generator.emit(op1Code);

                    m_scopePtr->pop();
                    CompilerPlaceholder ph1add;
                    m_scopePtr->push(ph1add.toString(), "int", "");
                    return;
                }

                // 恒等优化: x+0=x, x-0=x (省 2 字节).
                bool rhsIsZero = node.rhs && isLiteralValue(node.rhs.get(), 0);
                bool lhsIsZero = node.op == "+" && node.lhs &&
                                 isLiteralValue(node.lhs.get(), 0);

                if (rhsIsZero || lhsIsZero) {
                    ExprNode* mainExpr = rhsIsZero ? node.lhs.get()
                                                   : node.rhs.get();
                    LOG_DEBUG("Identity optimization: x " + node.op + " 0 = x");
                    visitExpr(*mainExpr);
                    auto elementOpt = m_scopePtr->pop();
                    lamd(elementOpt, operVec, *mainExpr, "lhs");
                    m_scopePtr->pop();
                    CompilerPlaceholder phId;
                    m_scopePtr->push(phId.toString(), "int", "");
                    return;
                }

                // 0-x -> OP_NEGATE (省 2 字节).
                bool lhsIsZeroSub = node.op == "-" && node.lhs &&
                                    isLiteralValue(node.lhs.get(), 0);

                if (lhsIsZeroSub) {
                    visitExpr(*node.rhs);
                    auto elementOpt = m_scopePtr->pop();
                    lamd(elementOpt, operVec, *node.rhs, "rhs");

                    m_generator.emit(tbc::BytOpcode::OP_NEGATE);

                    m_scopePtr->pop();
                    CompilerPlaceholder phNeg;
                    m_scopePtr->push(phNeg.toString(), "int", "");
                    return;
                }
            }

            // 恒等优化: x*1=x, x/1=x (省 2 字节).
            if (node.op == "*" || node.op == "/") {
                bool rhsIsOne = node.rhs && isLiteralValue(node.rhs.get(), 1);
                bool lhsIsOne = node.op == "*" && node.lhs &&
                                isLiteralValue(node.lhs.get(), 1);

                if (rhsIsOne || lhsIsOne) {
                    ExprNode* mainExpr = rhsIsOne ? node.lhs.get()
                                                  : node.rhs.get();
                    LOG_DEBUG("Identity optimization: x " + node.op + " 1 = x");
                    visitExpr(*mainExpr);
                    auto elementOpt = m_scopePtr->pop();
                    lamd(elementOpt, operVec, *mainExpr, "lhs");
                    m_scopePtr->pop();
                    CompilerPlaceholder phId;
                    m_scopePtr->push(phId.toString(), "int", "");
                    return;
                }
            }

            visitExpr(*node.lhs);
            auto elementOpt = m_scopePtr->pop();
            lamd(elementOpt, operVec, *node.lhs, "lhs");

            visitExpr(*node.rhs);
            elementOpt = m_scopePtr->pop();
            lamd(elementOpt, operVec, *node.rhs, "rhs");
        }
    }

    std::ostringstream oss;
    oss << "Participating computing elements:";
    auto itOpt = operVec.begin();
    for (; itOpt != operVec.end(); ++itOpt) {
        oss << " " << *itOpt;
    }
    LOG_DEBUG(oss.str());

    m_scopePtr->pop();
    if (node.lhs != nullptr) {
        m_scopePtr->pop();
    }

    auto it = g_operatorMap.find(node.op);
    if (it != g_operatorMap.end()) {
        m_generator.emit(it->second());

        std::string resultType = "int";
        if (node.op == "==" || node.op == "!=" || node.op == "<" ||
            node.op == ">" || node.op == "<=" || node.op == ">=") {
            resultType = "bool";
        }

        CompilerPlaceholder ph;
        m_scopePtr->push(ph.toString(), resultType, it->second());
    } else {
        SourceLocation loc("", node.pos.first, node.pos.second);
        std::ostringstream warningStream;
        warningStream << "unsupported operator '" << node.op << "'";
        COMPILER_WARNING(warningStream.str(), loc);
        LOG_WARNING(warningStream.str());
    }
}

void ASTToBytecodeVisitor::visitCall(CallNode& node)
{
    LOG_DEBUG("Visiting call node start. fun name: " + node.funcName);

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    try {
        processGenericFunctionCall(
            node.funcName, node.args, node, std::nullopt
        );
    } catch (const std::exception& e) {
        SourceLocation loc("", node.pos.first, node.pos.second);
        std::ostringstream errorStream;
        errorStream << "function call error: " << e.what();
        SEMANTIC_ERROR(
            errorStream.str(),
            loc,
            "Check the function name, arguments, and ensure the "
            "function is properly declared"
        );
        LOG_ERROR(errorStream.str());
        throw;
    }

    LOG_DEBUG("Visiting call node end. fun name: " + node.funcName);
}

void ASTToBytecodeVisitor::visitMethodCall(MethodCallNode& node)
{
    LOG_DEBUG("Visiting method call node start. Method: " + node.methodName);

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    auto objectResult = processMethodCallObject(node);
    if (!objectResult.has_value()) {
        return; // 错误已在辅助函数中记录.
    }

    try {
        processGenericFunctionCall(
            node.methodName, node.args, node, objectResult
        );
    } catch (const std::exception& e) {
        SourceLocation loc("", node.pos.first, node.pos.second);
        std::ostringstream errorStream;
        errorStream << "method call error: " << e.what();
        SEMANTIC_ERROR(
            errorStream.str(),
            loc,
            "Check the method name, arguments, and ensure the "
            "method is properly declared on the object"
        );
        LOG_ERROR(errorStream.str());
        throw std::runtime_error(errorStream.str());
    }

    LOG_DEBUG("Visiting method call node end. Method: " + node.methodName);
}

void ASTToBytecodeVisitor::visitFieldAccess(FieldAccessNode& node)
{
    LOG_DEBUG("Visiting field access node start. field name: " + node.field);
    DEFER([field = node.field]() {
        LOG_DEBUG("Visiting field access node end. field name: " + field);
    });

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    visitExpr(*node.base);

    auto baseElementOpt = m_scopePtr->pop();
    if (!baseElementOpt.has_value()) {
        SourceLocation loc("", node.pos.first, node.pos.second);
        std::ostringstream oss;
        oss << "failed to evaluate base expression for field access '"
            << node.field << "'";
        LOG_ERROR(
            "Semantic error at line ",
            node.pos.first,
            ", column ",
            node.pos.second,
            " - ",
            oss.str()
        );
        SEMANTIC_ERROR(
            oss.str(), loc, "Check the base expression before the dot operator"
        );
        return;
    }

    const std::string& baseElementStr = baseElementOpt.value().getName();

    if (m_scopePtr->isCompoundTypeSymbol(baseElementStr)) {
        LOG_DEBUG(
            "Accessing compound type field: " + baseElementStr + "." +
            node.field
        );

        auto compoundInfoOpt = m_scopePtr->getCompoundTypeInfo(baseElementStr);
        if (!compoundInfoOpt.has_value()) {
            SourceLocation loc("", node.pos.first, node.pos.second);
            std::ostringstream oss;
            oss << "Failed to get compound type info for '" << baseElementStr
                << "'";
            LOG_ERROR(oss.str());
            SEMANTIC_ERROR(oss.str(), loc, "Internal compiler error");
            return;
        }

        const CompoundTypeInfo& compoundInfo = compoundInfoOpt.value();

        if (!m_scopePtr->isCompoundTypeSplitted(baseElementStr)) {
            LOG_DEBUG(
                "Compound type not yet splitted, performing split: " +
                baseElementStr
            );

            // 首次访问触发拆分: roll 整体数据到栈顶, 逐个 SPLIT 字段.
            auto basePos = m_scopePtr->getPos(baseElementStr);
            if (basePos.has_value() && basePos.value() != STACK_TOP_POS) {
                emitRoll(basePos.value());
                LOG_DEBUG("Rolled compound type to top: " + baseElementStr);
            }

            for (size_t i = 0; i < compoundInfo.fields.size() - 1; ++i) {
                const auto& field = compoundInfo.fields[i];
                m_generator.emit(numberToScriptHex(field.byteSize));
                m_generator.emit(tbc::BytOpcode::OP_SPLIT);
                LOG_DEBUG(
                    "Split field: " + field.name + " (" +
                    std::to_string(field.byteSize) + " bytes)"
                );
            }

            // 注册字段到符号表; split 后栈序: 底 [field0]..[fieldN] 顶.
            m_scopePtr->pop();

            // 正序 push (field0 -> fieldN), 与 Script 栈顺序一致.
            for (size_t i = 0; i < compoundInfo.fields.size(); ++i) {
                const auto& field = compoundInfo.fields[i];
                std::string fullFieldName = baseElementStr + "." + field.name;
                m_scopePtr->push(fullFieldName, field.type, fullFieldName);
                LOG_DEBUG("Registered field: " + fullFieldName);
            }

            m_scopePtr->markCompoundTypeSplitted(baseElementStr);
            LOG_DEBUG("Marked compound type as splitted: " + baseElementStr);
        }

        std::string fullFieldName = baseElementStr + "." + node.field;
        auto fieldPos = m_scopePtr->getPos(fullFieldName);
        if (!fieldPos.has_value()) {
            SourceLocation loc("", node.pos.first, node.pos.second);
            std::ostringstream oss;
            oss << "Field '" << node.field << "' not found in compound type '"
                << baseElementStr << "'";
            LOG_ERROR(oss.str());
            SEMANTIC_ERROR(oss.str(), loc, "Check the field name");
            return;
        }

        std::string fieldType = "int";
        for (const auto& field : compoundInfo.fields) {
            if (field.name == node.field) {
                fieldType = field.type;
                break;
            }
        }

        m_scopePtr->push(fullFieldName, fieldType, fullFieldName);
        LOG_DEBUG("Accessed compound type field: " + fullFieldName);
        return;
    }

    if (auto builtStructObjPtr =
            tbc::BuiltinUtilsStruct::createBuiltStructOjb(baseElementStr)) {
        std::string opcodeHex = builtStructObjPtr->getOpcodeHex(node.field);

        // 内置对象成员: 存固定区, 同时入栈保持兼容.
        tbc::StackElement builtinElement(
            opcodeHex, "builtin_member", opcodeHex
        );
        m_scopePtr->setFixed(builtinElement);
        LOG_DEBUG(
            "Stored builtin member '" + opcodeHex +
            "' to fixed area with opcode: " + opcodeHex
        );

        m_scopePtr->push(opcodeHex, "builtin_member", opcodeHex);
        return;
    }
    if (!CompilerPlaceholder::isPlaceholder(baseElementStr) &&
        !isScript(baseElementStr)) {
        // 类型名不可访问字段; 必须用实例.
        if (m_structDefinitions.find(baseElementStr) !=
            m_structDefinitions.end()) {
            SourceLocation loc("", node.pos.first, node.pos.second);
            std::ostringstream oss;
            oss << "cannot access member '" << node.field << "' on type '"
                << baseElementStr << "'";
            LOG_ERROR(
                "Semantic error at line ",
                node.pos.first,
                ", column ",
                node.pos.second,
                " - ",
                oss.str()
            );
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "You need an instance of the struct, not the type name itself"
            );
            return;
        }

        auto fieldStr = baseElementStr + "." + node.field;
        auto fieldPosOpt = m_scopePtr->getPos(fieldStr);
        if (!fieldPosOpt.has_value()) {
            LOG_DEBUG(
                "Field not found in scope, trying to get from fixed area: " +
                fieldStr
            );

            std::string fieldStrRef = fieldStr; // getFixed 需非 const 引用.
            auto fixedElementOpt = m_scopePtr->getFixed(fieldStrRef);
            if (fixedElementOpt.has_value()) {
                LOG_DEBUG("Found field in fixed area: " + fieldStr);
                const std::string& data = fixedElementOpt.value().getData();
                if (!data.empty() && data != fieldStr) {
                    LOG_DEBUG("Using fixed variable data: " + data);
                    m_scopePtr
                        ->push(data, fixedElementOpt.value().getType(), data);
                } else {
                    LOG_DEBUG("Using fixed variable name: " + fieldStr);
                    m_scopePtr->push(
                        fieldStr, fixedElementOpt.value().getType(), fieldStr
                    );
                }
                return;
            }

            // 动态结构体数组字段: baseElementStr 含 "]" 表示来自索引虚拟元素.
            size_t bracket = baseElementStr.find(']');
            size_t lb = baseElementStr.find('[');
            if (bracket != std::string::npos && lb != std::string::npos &&
                lb < bracket) {
                std::string arrayBase = baseElementStr.substr(0, lb);
                // 拼接扁平字段链: "]" 后的相对路径 + 当前字段名.
                std::string after = "";
                if (bracket + 1 < baseElementStr.size() &&
                    baseElementStr[bracket + 1] == '.') {
                    after = baseElementStr.substr(bracket + 1);
                }
                std::string flatField = after.empty()
                                            ? "." + node.field
                                            : (after + "." + node.field);

                SourceLocation loc("", node.pos.first, node.pos.second);
                StructArrayInfo info;
                if (ensureStructArrayInfo(arrayBase, info, loc)) {
                    auto itOff = info.fieldOffsets.find(flatField);
                    if (itOff != info.fieldOffsets.end()) {
                        LOG_DEBUG("Found field offset: " + flatField);
                        // 索引阶段已 tmp=basePos-vout*stride; 此处 tmp-fieldOffset.
                        if (itOff->second != 0) {
                            m_generator.emit(numberToScriptHex(itOff->second));
                            m_generator.emit(tbc::BytOpcode::OP_SUB);
                        }

                        m_scopePtr->pop();

                        std::string fieldType = "int";
                        auto flatFields = getStructFieldsExpanded(
                            info.elementType, "", m_structDefinitions
                        );
                        for (const auto& p : flatFields) {
                            if (p.first == flatField) {
                                fieldType = p.second;
                                break;
                            }
                        }
                        m_scopePtr->push(fieldStr, fieldType, fieldStr);
                        return;
                    }
                }
            }

            // 既不在栈也不在固定区: 可能是链式调用或 uint64 数组字段.
            for (auto baseStructIt : m_structDefinitions) {
                for (const auto& fieldDef : baseStructIt.second) {
                    if (fieldDef.first == node.field) {
                        const StructFieldType& fieldTypeInfo = fieldDef.second;
                        const std::string fieldType =
                            fieldTypeInfo.getTypeString();
                        std::string baseType = fieldTypeInfo.baseType;
                        if (m_structDefinitions.find(baseType) !=
                            m_structDefinitions.end()) {
                            m_scopePtr->push(fieldStr, fieldType, fieldStr);
                            return;
                        } else if (baseType == "uint64") {
                            m_scopePtr->push(fieldStr, fieldType, fieldStr);
                            return;
                        }
                    }
                }
            }

            // 字段已定义但未入栈 (结构体声明时预定义).
            if (m_scopePtr->symbolExists(fieldStr)) {
                LOG_DEBUG(
                    "Field exists in symbol table but not on stack, "
                    "looking up field type: " +
                    fieldStr
                );

                // 从结构体定义找字段类型; fieldStr 形如 "base.fieldName".
                for (const auto& structDef : m_structDefinitions) {
                    const auto& fields = structDef.second;
                    for (const auto& field : fields) {
                        if (field.first == node.field) {
                            // 匹配字段, 推入占位符.
                            const StructFieldType& fieldTypeInfo = field.second;
                            const std::string fieldType =
                                fieldTypeInfo.getTypeString();
                            m_scopePtr->push(fieldStr, fieldType, fieldStr);
                            LOG_DEBUG(
                                "Pushed placeholder for uninitialized field: " +
                                fieldStr + " with type: " + fieldType
                            );
                            return;
                        }
                    }
                }
            }

            SourceLocation loc("", node.pos.first, node.pos.second);
            std::ostringstream oss;
            oss << "field '" << node.field << "' not found in struct '"
                << baseElementStr << "'";
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Check if the field name is correct and the struct "
                "is properly defined"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }
        auto element = m_scopePtr->stacktop(fieldPosOpt.value());
        m_scopePtr->push(fieldStr, element.getType(), fieldStr);
    } else {
        SourceLocation loc("", node.pos.first, node.pos.second);
        std::ostringstream oss;
        oss << "cannot access field '" << node.field
            << "' on expression of type '" << baseElementStr << "'";
        LOG_ERROR(
            "Semantic error at line ",
            node.pos.first,
            ", column ",
            node.pos.second,
            " - ",
            oss.str()
        );
        SEMANTIC_ERROR(
            oss.str(), loc, "Only struct types support field access"
        );
        return;
    }
}

void ASTToBytecodeVisitor::visitIndexAccess(IndexAccessNode& node)
{
    LOG_DEBUG("Visiting index access node start.");

    visitExpr(*node.base);
    auto baseElement = m_scopePtr->pop();
    if (!baseElement.has_value()) {
        SourceLocation loc("", node.base->pos.first, node.base->pos.second);
        std::ostringstream errorStream;
        errorStream << "failed to evaluate base expression for index access";
        SEMANTIC_ERROR(
            errorStream.str(),
            loc,
            "Check the base expression syntax and ensure it "
            "evaluates to a valid value"
        );
        LOG_ERROR(errorStream.str());
        throw std::runtime_error(errorStream.str());
    }

    visitExpr(*node.index);
    auto indexElement = m_scopePtr->pop();
    if (!indexElement.has_value()) {
        SourceLocation loc("", node.index->pos.first, node.index->pos.second);
        std::ostringstream errorStream;
        errorStream << "failed to evaluate index expression for index access";
        SEMANTIC_ERROR(
            errorStream.str(),
            loc,
            "Check the index expression syntax and ensure it "
            "evaluates to a valid value"
        );
        LOG_ERROR(errorStream.str());
        throw std::runtime_error(errorStream.str());
    }

    const std::string& baseElementStr = baseElement.value().getName();
    const std::string& indexElementStr = indexElement.value().getName();

    LOG_DEBUG(
        "Processing index access - base: " + baseElementStr +
        ", index: " + indexElementStr
    );

    if (isWholeArrayElement(baseElementStr)) {
        LOG_DEBUG(
            "Base element is a whole array, splitting: " + baseElementStr
        );
        splitWholeArrayElement(baseElementStr);
    }

    LOG_DEBUG("Index element name: " + indexElement.value().getName());

    if (isScript(baseElementStr)) {
        // 特例: 合约成员变量 self.X[i] 当 i 为编译期常量时等价于 self.X<N>,
        // 复用 builtin member 的 fixed-area, 避免运行时偏移计算.
        if (baseElementStr.starts_with("<self.") &&
            baseElementStr.ends_with(">") && isScript(indexElementStr)) {
            int64_t idxValue = 0;
            bool idxOk = true;
            try {
                idxValue = scriptHexToNumber(indexElementStr);
            } catch (const std::exception&) {
                idxOk = false;
            }
            if (idxOk && idxValue >= 0) {
                // 取合约成员变量 <self.X> 的字段名 X, 拼 <self.X{N}>.
                std::string fieldName = baseElementStr.substr(
                    6, baseElementStr.size() - 7
                );
                std::string newLabel =
                    "<self." + fieldName + std::to_string(idxValue) + ">";

                tbc::StackElement builtinElement(
                    newLabel, "builtin_member", newLabel
                );
                m_scopePtr->setFixed(builtinElement);
                LOG_DEBUG(
                    "Expanded self field index access '" + baseElementStr +
                    "[" + indexElementStr + "]' to fixed area: " + newLabel
                );
                m_scopePtr->push(newLabel, "builtin_member", newLabel);
                return;
            }
        }
        SourceLocation loc("", node.pos.first, node.pos.second);
        std::ostringstream errorStream;
        errorStream
            << "index access not supported on generated script element: "
            << baseElementStr;
        SEMANTIC_ERROR(
            errorStream.str(),
            loc,
            "Index access is only supported on array-like "
            "variables, not on script expressions"
        );
        LOG_ERROR(errorStream.str());
        throw std::runtime_error(errorStream.str());
    } else if (CompilerPlaceholder::isPlaceholder(baseElementStr)) {
        SourceLocation loc("", node.pos.first, node.pos.second);
        std::ostringstream errorStream;
        errorStream << "index access not supported on placeholder element";
        SEMANTIC_ERROR(
            errorStream.str(),
            loc,
            "Index access requires a concrete variable or array, "
            "not a placeholder"
        );
        LOG_ERROR(errorStream.str());
        throw std::runtime_error(errorStream.str());
    } else {
        if (m_scopePtr->isArraySymbol(baseElementStr)) {
            // 零成本数组访问: 用索引拼标签.
            std::string elementLabel = baseElementStr + "[" + indexElementStr +
                                       "]";

            LOG_DEBUG("Generated element label: " + elementLabel);

            auto arrayInfoOpt = m_scopePtr->getArrayInfo(baseElementStr);
            std::string elementType = arrayInfoOpt.has_value()
                                          ? arrayInfoOpt.value().elementType
                                          : "int";

            bool isStructArray = m_structDefinitions.find(elementType) !=
                                 m_structDefinitions.end();

            if (isStructArray) {
                if (!CompilerPlaceholder::isPlaceholder(indexElementStr) &&
                    !isScript(indexElementStr)) {
                    auto indexPosOpt = m_scopePtr->getPos(indexElementStr);
                    if (indexPosOpt.has_value() &&
                        STACK_TOP_POS != indexPosOpt.value()) {
                        LOG_DEBUG(
                            "Moving index to stack top from position: " +
                            std::to_string(indexPosOpt.value())
                        );
                        emitRoll(indexPosOpt.value());
                        m_scopePtr->roll(indexPosOpt.value());
                    }
                    // 索引阶段: tmp = basePos - vout*stride.
                    SourceLocation loc("", node.pos.first, node.pos.second);
                    StructArrayInfo info;
                    if (ensureStructArrayInfo(baseElementStr, info, loc)) {
                        m_generator.emit(numberToScriptHex(info.stride));
                        m_generator.emit(tbc::BytOpcode::OP_MUL);
                        m_generator.emit(tbc::BytOpcode::OP_NEGATE);
                        m_generator.emit(numberToScriptHex(info.basePos));
                        m_generator.emit(tbc::BytOpcode::OP_ADD);
                    }
                }

                // 虚拟栈元素: 代表结构体数组元素访问结果.
                LOG_DEBUG(
                    "Creating virtual reference for struct array element: " +
                    elementLabel
                );
                m_scopePtr->push(elementLabel, elementType, elementLabel);
                return;
            } else {
                auto elementPosOpt = m_scopePtr->getPos(elementLabel);
                if (elementPosOpt.has_value()) {
                    LOG_DEBUG(
                        "Array element found with label: " + elementLabel
                    );

                    // 零成本: 仅返回标签, 不生成字节码、不占栈空间.
                    m_scopePtr->push(elementLabel, elementType, elementLabel);
                    return;
                }

                // 栈上找不到时回退 fixed 区 (常量字面量数组元素).
                // data 若为真正的脚本常量 (如 "0x01"), 以 data 作为虚拟栈
                // 元素 name 压入, 这样 visitOperator 的 isScript 检测才能
                // 命中并 emit push; 否则仍以 elementLabel 作为 name.
                std::string elementLabelRef = elementLabel;
                auto fixedElementOpt = m_scopePtr->getFixed(elementLabelRef);
                if (fixedElementOpt.has_value()) {
                    LOG_DEBUG(
                        "Array element found in fixed area: " + elementLabel
                    );
                    const std::string& data = fixedElementOpt.value().getData();
                    if (!data.empty() && data != elementLabel) {
                        m_scopePtr->push(
                            data, fixedElementOpt.value().getType(), data
                        );
                    } else {
                        m_scopePtr->push(
                            elementLabel,
                            fixedElementOpt.value().getType(),
                            elementLabel
                        );
                    }
                    return;
                }

                SourceLocation loc("", node.pos.first, node.pos.second);
                std::ostringstream oss;
                oss << "array element '" << elementLabel
                    << "' not found in stack";
                SEMANTIC_ERROR(
                    oss.str(), loc, "Check array bounds and initialization"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }
        } else {
            SourceLocation loc("", node.pos.first, node.pos.second);
            std::ostringstream oss;
            oss << "non-array variable '" << baseElementStr
                << "' not found for index access";
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Check if the variable is properly declared and in scope"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }
    }

    LOG_DEBUG("Visiting index access node end.");
}

void ASTToBytecodeVisitor::visitArrayDecl(ArrayDeclNode& node)
{
    LOG_DEBUG("Visiting array declaration node start. name: " + node.name);
    DEFER([&]() {
        LOG_DEBUG("Visiting array declaration node end. name: " + node.name);
    });

    size_t arraySize = 0;

    if (node.hasInitializer()) {
        arraySize = node.initArray->getSize();
    } else if (node.sizeExpr) {
        visitExpr(*node.sizeExpr);
        auto sizeElementOpt = m_scopePtr->pop();
        if (!sizeElementOpt.has_value()) {
            SourceLocation loc = getNodeLocation(node);
            std::ostringstream oss;
            oss << "failed to evaluate array size expression for array '"
                << node.name << "'";
            SEMANTIC_ERROR(oss.str(), loc, "Check the array size expression");
            return;
        }

        try {
            arraySize = static_cast<size_t>(
                scriptHexToNumber(sizeElementOpt.value().getData())
            );
        } catch (const std::exception&) {
            SourceLocation loc = getNodeLocation(node);
            std::ostringstream oss;
            oss << "array size must be a compile-time constant for array '"
                << node.name << "'";
            SEMANTIC_ERROR(
                oss.str(), loc, "Use a numeric literal for array size"
            );
            return;
        }
    } else {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "array '" << node.name
            << "' must have either size or initializer";
        SEMANTIC_ERROR(
            oss.str(), loc, "Specify array size or provide initializer"
        );
        return;
    }

    // 仅登记数组元数据, 不压栈.
    if (!m_scopePtr
             ->defineArray(node.name, node.elementType, arraySize, true)) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "array '" << node.name << "' is already defined";
        SEMANTIC_ERROR(oss.str(), loc, "Use a different array name");
        return;
    }

    // 结构体数组: 预登记每个元素的扁平字段, 与结构体参数展开路径一致,
    // 使 tryHandleArrayDefAssignment / tryHandleStructBraceAssignment 在
    // "字段必须已声明" 上行为一致.
    if (m_structDefinitions.find(node.elementType) !=
        m_structDefinitions.end()) {
        for (size_t i = 0; i < arraySize; ++i) {
            std::string elemLabel =
                node.name + "[" +
                numberToScriptHex(static_cast<int64_t>(i)) + "]";
            auto flatFields = getStructFieldsExpanded(
                node.elementType, elemLabel, m_structDefinitions
            );
            for (const auto& f : flatFields) {
                std::string fieldPathCopy = f.first;
                    m_scopePtr->defineSymbol(f.first);
            }
        }
        LOG_DEBUG(
            "Pre-registered flattened struct fields for array: " + node.name
        );
    } else {
        // 标量数组: defineArray 仅登记名, 这里补登记每个元素符号,
        // 使 symbolExists("arr[0x..]") 返回 true, 与结构体数组路径一致.
        for (size_t i = 0; i < arraySize; ++i) {
            std::string elemLabel =
                node.name + "[" +
                numberToScriptHex(static_cast<int64_t>(i)) + "]";
            m_scopePtr->defineSymbol(elemLabel, node.elementType);
        }
        LOG_DEBUG(
            "Pre-registered scalar array elements as symbols: " + node.name
        );
    }

    if (node.hasInitializer()) {
        visitArrayDef(*node.initArray);

        bool isStructArray = m_structDefinitions.find(node.elementType) !=
                             m_structDefinitions.end();

        if (isStructArray) {
            auto structDefIt = m_structDefinitions.find(node.elementType);
            const auto& structFields = structDefIt->second;
            size_t fieldsPerStruct = structFields.size();
            size_t totalFieldsExpected = arraySize * fieldsPerStruct;

            LOG_DEBUG(
                "Processing struct array assignment: " + node.name +
                ", arraySize=" + std::to_string(arraySize) +
                ", fieldsPerStruct=" + std::to_string(fieldsPerStruct) +
                ", totalFields=" + std::to_string(totalFieldsExpected)
            );

            // 预检每个 ArrayDefNode 元素是否为常量 (结构体元素是 BraceExprNode).
            std::vector<bool> isConstantFlags;
            isConstantFlags.reserve(totalFieldsExpected);

            // 逆序遍历: 栈 LIFO.
            for (auto it = node.initArray->elements.rbegin();
                 it != node.initArray->elements.rend();
                 ++it) {
                auto* braceExpr = dynamic_cast<BraceExprNode*>((*it).get());
                if (braceExpr) {
                    for (auto braceIt = braceExpr->elements.rbegin();
                         braceIt != braceExpr->elements.rend();
                         ++braceIt) {
                        bool isConst = dynamic_cast<LiteralNode*>(braceIt->get()
                                       ) != nullptr;
                        isConstantFlags.push_back(isConst);
                    }
                } else {
                    for (size_t i = 0; i < fieldsPerStruct; ++i) {
                        bool isConst = dynamic_cast<LiteralNode*>((*it).get()
                                       ) != nullptr;
                        isConstantFlags.push_back(isConst);
                    }
                }
            }

            std::vector<tbc::StackElement> allValues;
            allValues.reserve(totalFieldsExpected);

            for (size_t i = 0; i < totalFieldsExpected; ++i) {
                auto valueOpt = m_scopePtr->pop();
                if (!valueOpt.has_value()) {
                    LOG_WARNING(
                        "Not enough initializer values for struct array " +
                        node.name + ", expected " +
                        std::to_string(totalFieldsExpected) + ", got " +
                        std::to_string(i)
                    );
                    break;
                }
                allValues.push_back(valueOpt.value());
            }

            // 按声明顺序创建字段符号并推栈.
            for (size_t arrayIdx = 0; arrayIdx < arraySize; ++arrayIdx) {
                for (size_t fieldIdx = 0; fieldIdx < fieldsPerStruct;
                     ++fieldIdx) {
                    // 初始化值索引: 栈 LIFO + 数组初始化逆序.
                    size_t valueIdx = (arraySize - 1 - arrayIdx) *
                                          fieldsPerStruct +
                                      (fieldsPerStruct - 1 - fieldIdx);

                    if (valueIdx >= allValues.size()) {
                        LOG_WARNING(
                            "Value index out of bounds: " +
                            std::to_string(valueIdx)
                        );
                        continue;
                    }

                    const auto& value = allValues[valueIdx];
                    const auto& fieldInfo = structFields[fieldIdx];
                    const std::string& fieldName = fieldInfo.first;
                    const StructFieldType& fieldTypeInfo = fieldInfo.second;
                    const std::string fieldType = fieldTypeInfo.getTypeString();

                    std::string elementLabel =
                        node.name + "[" +
                        numberToScriptHex(static_cast<int64_t>(arrayIdx)) + "]";
                    std::string fieldPath = elementLabel + "." + fieldName;

                    LOG_DEBUG(
                        "Creating field: " + fieldPath + " with value: " +
                        value.getName() + " (type: " + fieldType + ")"
                    );

                    // 常量字段存 fixed 区, 非常量走栈 (与普通变量赋值一致).
                    bool isConst = false;
                    if (valueIdx < isConstantFlags.size()) {
                        isConst = isConstantFlags[valueIdx];
                    }

                    if (isConst) {
                        tbc::StackElement fixedElement(
                            fieldPath, fieldType, value.getData()
                        );
                        m_scopePtr->setFixed(fixedElement);
                        LOG_DEBUG(
                            "Stored constant field '" + fieldPath +
                            "' to fixed area with data: " + value.getData()
                        );
                    } else {
                        m_scopePtr->push(fieldPath, fieldType, value.getData());
                        LOG_DEBUG("Pushed field value: " + fieldPath);
                    }
                }
            }
        } else {
            for (size_t i = 0; i < arraySize; ++i) {
                auto valueOpt = m_scopePtr->pop();
                if (!valueOpt.has_value()) {
                    LOG_WARNING(
                        "Not enough initializer values for array " + node.name
                    );
                    break;
                }

                // visitArrayDef 逆序遍历后，源码中的首元素位于栈顶，
                // 因此连续 pop 可直接按 i 绑定到对应数组元素。
                std::string elementLabel = m_scopePtr->getArrayElementLabel(
                    node.name, i
                );

                const auto& value = valueOpt.value();
                if (isScript(value.getName())) {
                    tbc::StackElement fixedElement(
                        elementLabel, value.getType(), value.getData()
                    );
                    m_scopePtr->setFixed(fixedElement);
                    LOG_DEBUG(
                        "Stored constant element '" + elementLabel +
                        "' to fixed area"
                    );
                } else {
                    m_scopePtr
                        ->push(elementLabel, node.elementType, value.getData());
                    LOG_DEBUG("Pushed array element value: " + elementLabel);
                }

                LOG_DEBUG("Initializing array element: " + elementLabel);
            }
        }
    }

    LOG_DEBUG("Array declaration completed for: " + node.name);
}

void ASTToBytecodeVisitor::visitArrayDef(ArrayDefNode& node)
{
    LOG_DEBUG("Visiting array definition node start.");
    DEFER([]() { LOG_DEBUG("Visiting array definition node end."); });

    // 逆序处理: 栈 LIFO.
    for (auto it = node.elements.rbegin(); it != node.elements.rend(); ++it) {
        visitExpr(**it);
    }

    LOG_DEBUG(
        "Array definition completed with " +
        std::to_string(node.elements.size()) + " elements"
    );
}

void ASTToBytecodeVisitor::visitBraceExpr(BraceExprNode& node)
{
    LOG_DEBUG("Visiting brace expression node start");
    DEFER([&]() { LOG_DEBUG("Visiting brace expression node end"); });

    for (auto& element : node.elements) {
        visitExpr(*element);
    }

    // 大括号语义依上下文: 结构体字面量由上层做类型推导, 多返回值解构时
    // 元素已在栈上.

    LOG_DEBUG(
        "Processed " + std::to_string(node.elements.size()) +
        " elements in brace expression"
    );
}

void ASTToBytecodeVisitor::visitDestructureAssign(DestructureAssignNode& node)
{
    LOG_DEBUG("Visiting destructure assignment node start");
    DEFER([&]() { LOG_DEBUG("Visiting destructure assignment node end"); });

    visitExpr(*node.value);

    if (!m_scopePtr->empty() &&
        m_structDefinitions.find(m_scopePtr->top().getType()) !=
            m_structDefinitions.end()) {
        SourceLocation loc = getNodeLocation(node);
        const std::string errorMsg =
            "returned struct cannot be unpacked as scalar values";
        SEMANTIC_ERROR(
            errorMsg,
            loc,
            "Assign the complete returned struct to one struct variable"
        );
        LOG_ERROR(errorMsg);
        throw std::runtime_error(errorMsg);
    }

    // 假设函数按顺序压入多返回值; 栈顶=最后一个返回值, 栈底=第一个.
    std::vector<tbc::StackElement> values;

    if (m_scopePtr->size() < node.targets.size()) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "not enough values to unpack (expected "
            << node.targets.size() << ", got " << m_scopePtr->size() << ")";
        SEMANTIC_ERROR(oss.str(), loc, "Check the function return values");
        throw std::runtime_error(oss.str());
    }

    for (size_t i = 0; i < node.targets.size(); ++i) {
        values.push_back(m_scopePtr->at(static_cast<uint64_t>(i)));
    }

    // 反转 values 与 targets 顺序对齐.
    std::reverse(values.begin(), values.end());

    for (size_t i = 0; i < node.targets.size(); ++i) {
        const std::string& targetName = node.targets[i];
        const tbc::StackElement& value = values[i];

        // private 标量形参是调用者栈槽的逻辑别名。解构赋值写入同名
        // 新值后，旧别名已经失效，后续读取必须解析到新压入的目标。
        SymbolTable& symtab = m_scopePtr->getCurrentSymtab();
        if (symtab.resolveBindSymbol(targetName) != targetName) {
            symtab.removeBindSymbol(targetName);
        }

        // 隐式声明: 左值不在符号表则按右值类型自动声明.
        std::string lookupName = targetName;
        if (!targetName.empty() && !m_scopePtr->symbolExists(lookupName)) {
            m_scopePtr->defineSymbol(targetName, value.getType());
            m_scopePtr->markSymbolInitialized(targetName);
            LOG_DEBUG(
                "Implicit declaration via destructure assignment: " + targetName
            );
        }

        const int position = static_cast<int>(node.targets.size() - 1 - i);
        if (!m_scopePtr->renameAtPosition(position, targetName)) {
            m_scopePtr->push(targetName, value.getType(), value.getData());
        }

        LOG_DEBUG(
            "Assigned value to variable: " + targetName +
            " with type: " + value.getType()
        );
    }

    LOG_DEBUG(
        "Destructure assignment completed for " +
        std::to_string(node.targets.size()) + " variables"
    );
}

void ASTToBytecodeVisitor::privateFunctionResolution(
    FunctionNode& node,
    const std::vector<tbc::StackElement>& existingArgs
)
{
    LOG_DEBUG("Private function inline resolution for: " + node.name);

    auto currentStackSize = m_scopePtr->size();
    LOG_DEBUG(
        "Current stack size before private function call: " +
        std::to_string(currentStackSize)
    );

    if (existingArgs.size() != node.parameters.size()) {
        std::ostringstream errorStream;
        errorStream << "Parameter count mismatch for private function '"
                    << node.name << "': expected " << node.parameters.size()
                    << ", got " << existingArgs.size();
        SourceLocation loc("", node.pos.first, node.pos.second);
        SEMANTIC_ERROR(
            errorStream.str(), loc, "Check the function call arguments"
        );
        LOG_ERROR(errorStream.str());
        throw std::runtime_error(errorStream.str());
    }

    // Interpreter control-flow lowering stops visiting statements after the
    // first lowercase return. Validate the full source shape before inlining
    // so an unreachable additional return cannot hide an illegal struct plus
    // scalar multi-return behind an internal assignment error.
    std::vector<ReturnNode*> inlineReturns;
    if (node.block) {
        findAllReturnNodes(node.block.get(), inlineReturns);
    }
    size_t lowercaseReturnCount = 0;
    std::unordered_set<std::string> knownStructValues;
    for (const auto& parameter : node.parameters) {
        if (m_structDefinitions.find(parameter.type) !=
            m_structDefinitions.end()) {
            knownStructValues.insert(parameter.name);
        }
    }
    std::function<void(const StmtNode*)> collectStructLocals =
        [&](const StmtNode* stmt) {
            if (!stmt) {
                return;
            }
            if (auto* declaration =
                    dynamic_cast<const VarDeclNode*>(stmt)) {
                if (m_structDefinitions.find(declaration->type) !=
                    m_structDefinitions.end()) {
                    knownStructValues.insert(declaration->name);
                }
                return;
            }
            if (auto* block = dynamic_cast<const BlockNode*>(stmt)) {
                for (const auto& child : block->statements) {
                    collectStructLocals(child.get());
                }
                return;
            }
            if (auto* branch = dynamic_cast<const IfNode*>(stmt)) {
                collectStructLocals(branch->thenBranch.get());
                collectStructLocals(branch->elseBranch.get());
                return;
            }
            if (auto* loop = dynamic_cast<const ForNode*>(stmt)) {
                collectStructLocals(loop->body.get());
            }
        };
    collectStructLocals(node.block.get());

    ReturnNode* knownStructReturn = nullptr;
    for (ReturnNode* returnNode : inlineReturns) {
        if (!returnNode || !returnNode->isValueReturn) {
            continue;
        }
        ++lowercaseReturnCount;
        if (auto* identifier =
                dynamic_cast<IdentifierNode*>(returnNode->expr.get())) {
            if (knownStructValues.count(identifier->name) != 0) {
                knownStructReturn = returnNode;
            }
        }
    }
    if (knownStructReturn != nullptr && lowercaseReturnCount > 1) {
        std::ostringstream oss;
        oss << "a struct-returning function cannot contain additional "
               "lowercase return values";
        SourceLocation loc(
            "", knownStructReturn->pos.first, knownStructReturn->pos.second
        );
        SEMANTIC_ERROR(
            oss.str(), loc, "Return the complete struct as the only value"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    // Private calls are emitted inline into one shared symbol table. Keep all
    // call-local metadata scoped explicitly so nested calls cannot leak their
    // parameter bindings or return/Keep markers into their caller.
    SymbolTable& callSymtab = m_scopePtr->getCurrentSymtab();
    const auto savedBindings = callSymtab.m_bindSymbol;
    const auto savedKeepSymbols = callSymtab.m_keepSymbol;
    const auto savedCurrentScope = callSymtab.m_currentScope;
    const auto savedNewSymbols = callSymtab.m_newSymbol;
    const auto savedDeclaredSymbols = callSymtab.m_declaredSymbols;
    const auto savedWholeArrayElements = m_wholeArrayElements;
    const auto savedFixedStack =
        callSymtab.m_fixedStackPtr->getStackContent();
    const size_t savedFixedCombinedSize =
        callSymtab.m_fixedStackPtr->getCombinedStackSize();
    const size_t savedBindSymbolStart =
        callSymtab.activeBindSymbolStart();
    const size_t savedScopeEntryStart =
        callSymtab.activeScopeEntryStart();
    callSymtab.beginBindSymbolFrame();
    callSymtab.beginScopeEntryFrame();
    m_activePrivateFunctions.push_back(&node);
    m_structReturnFrames.emplace_back();
    DEFER_BLOCK(
        SymbolTable& currentSymtab = m_scopePtr->getCurrentSymtab();
        currentSymtab.m_bindSymbol = savedBindings;
        currentSymtab.restoreBindSymbolFrame(savedBindSymbolStart);
        currentSymtab.restoreScopeEntryFrame(savedScopeEntryStart);
        currentSymtab.m_keepSymbol = savedKeepSymbols;
        m_structReturnFrames.pop_back();
        m_activePrivateFunctions.pop_back();
    );

    // The inline callee forms a nested lexical layer over the caller. Array
    // and scalar declarations use this list to decide whether a name may
    // shadow an outer declaration; the caller's exact list is restored after
    // the call.
    callSymtab.m_declaredSymbols.clear();

    // 把实参映射到形参名.
    LOG_DEBUG("Binding parameters");

    for (int i = node.parameters.size() - 1; i >= 0; --i) {
        const auto& paramInfo = node.parameters[i];
        const std::string& paramName = paramInfo.name;
        const std::string& paramType = paramInfo.type;
        auto argRootName = callSymtab.resolveBindSymbol(
            existingArgs[i].getName()
        );

        // 不在此处对 argRootName 改名:
        //  - CompilerPlaceholder 实参: 走普通栈元素路径, 保留 /Compiler.../ 原名
        //    供 BindSymbol 解析后在真栈上匹配; 与普通局部变量 / main 入参一致.
        //  - <self.X> 实参: 不会进 BindSymbol 解析, 由 visitIdentifier 在
        //    形参访问时按需 push 名 + emit.
        // 早期版本曾对 CompilerPlaceholder 做 renameTopToBottom + 改 argRootName,
        // 但 Scope::getCurrentSymtab() const 是值返回, rename 实际作用在 SymbolTable
        // 副本上, 真栈毫无变化, 反而让 BindSymbol 指向真栈上不存在的 funcName_paramName,
        // 导致 visitIdentifier 形参访问失败.

        const auto arrayType = apc::util::parseFixedArrayType(paramType);
        bool isStructType = !paramType.empty() &&
                            (m_structDefinitions.find(paramType) !=
                             m_structDefinitions.end());

        if (arrayType.has_value()) {
            auto sourceArray = m_scopePtr->getArrayInfo(argRootName);
            if (!sourceArray.has_value() ||
                sourceArray->elementType != arrayType->elementType ||
                sourceArray->size != arrayType->size) {
                std::ostringstream oss;
                oss << "Unable to bind fixed-array argument '" << argRootName
                    << "' to parameter '" << paramName << "' of type '"
                    << paramType << "'";
                SourceLocation loc("", node.pos.first, node.pos.second);
                SEMANTIC_ERROR(
                    oss.str(), loc,
                    "Pass a fixed array with the exact declared shape"
                );
                throw std::runtime_error(oss.str());
            }

            if (!m_scopePtr->defineArray(
                    paramName,
                    arrayType->elementType,
                    arrayType->size,
                    true
                )) {
                throw std::runtime_error(
                    "failed to declare private fixed-array parameter '" +
                    paramName + "'"
                );
            }

            if (paramName != argRootName) {
                std::pair<std::string, std::string> rootBinding(
                    paramName, argRootName
                );
                m_scopePtr->addBindSymbol(rootBinding);
            }

            const bool isStructArray =
                m_structDefinitions.find(arrayType->elementType) !=
                m_structDefinitions.end();

            for (size_t elementIndex = 0;
                 elementIndex < arrayType->size;
                 ++elementIndex) {
                const std::string paramElement =
                    m_scopePtr->getArrayElementLabel(
                        paramName, elementIndex
                    );
                const std::string argumentElement =
                    sourceArray->getElementLabel(elementIndex);
                if (paramElement != argumentElement) {
                    std::pair<std::string, std::string> elementBinding(
                        paramElement, argumentElement
                    );
                    m_scopePtr->addBindSymbol(elementBinding);
                }

                if (!isStructArray) {
                    m_scopePtr->defineSymbol(
                        paramElement, arrayType->elementType
                    );
                    continue;
                }

                const auto paramFields = getStructFieldsExpanded(
                    arrayType->elementType,
                    paramElement,
                    m_structDefinitions
                );
                const auto argumentFields = getStructFieldsExpanded(
                    arrayType->elementType,
                    argumentElement,
                    m_structDefinitions
                );
                if (paramFields.size() != argumentFields.size()) {
                    std::ostringstream oss;
                    oss << "Unable to bind struct-array parameter '"
                        << paramName << "' element " << elementIndex
                        << ": parameter has " << paramFields.size()
                        << " runtime fields, argument has "
                        << argumentFields.size();
                    SourceLocation loc("", node.pos.first, node.pos.second);
                    SEMANTIC_ERROR(
                        oss.str(), loc,
                        "Pass a struct array with the exact declared shape"
                    );
                    throw std::runtime_error(oss.str());
                }

                for (size_t fieldIndex = 0;
                     fieldIndex < paramFields.size();
                     ++fieldIndex) {
                    const auto& [paramFieldPath, paramFieldType] =
                        paramFields[fieldIndex];
                    const auto& [argumentFieldPath, argumentFieldType] =
                        argumentFields[fieldIndex];
                    if (paramFieldType != argumentFieldType) {
                        std::ostringstream oss;
                        oss << "Unable to bind struct-array parameter '"
                            << paramName << "' field '" << paramFieldPath
                            << "': expected type '" << paramFieldType
                            << "', argument field has type '"
                            << argumentFieldType << "'";
                        SourceLocation loc(
                            "", node.pos.first, node.pos.second
                        );
                        SEMANTIC_ERROR(
                            oss.str(), loc,
                            "Pass a struct array with the exact declared shape"
                        );
                        throw std::runtime_error(oss.str());
                    }

                    m_scopePtr->defineSymbol(
                        paramFieldPath, paramFieldType
                    );
                    if (paramFieldPath != argumentFieldPath) {
                        std::pair<std::string, std::string> fieldBinding(
                            paramFieldPath, argumentFieldPath
                        );
                        m_scopePtr->addBindSymbol(fieldBinding);
                    }
                }
            }
            m_scopePtr->markSymbolInitialized(paramName);
        } else if (isStructType) {
            LOG_DEBUG(
                "Binding struct parameter '" + paramName + "' of type '" +
                paramType + "'"
            );

            // 形参/实参各自展开扁平字段路径, 然后 1:1 绑定.
            auto paramFields = getStructFieldsExpanded(
                paramType, paramName, m_structDefinitions
            );
            auto argFields = getStructFieldsExpanded(
                paramType, argRootName, m_structDefinitions
            );

            if (paramFields.size() != argFields.size()) {
                LOG_WARNING(
                    "Struct parameter field count mismatch when binding '" +
                    paramName +
                    "': paramFields=" + std::to_string(paramFields.size()) +
                    ", argFields=" + std::to_string(argFields.size())
                );
            }

            size_t fieldCount = std::min(paramFields.size(), argFields.size());
            for (size_t fi = 0; fi < fieldCount; ++fi) {
                const std::string& paramFieldPath = paramFields[fi].first;
                const std::string& argFieldPath = argFields[fi].first;

                LOG_DEBUG(
                    "Binding struct field '" + paramFieldPath + "' -> '" +
                    argFieldPath + "'"
                );

                std::pair<std::string, std::string> bindPair(
                    paramFieldPath, argFieldPath
                );
                m_scopePtr->addBindSymbol(bindPair);
            }
        } else {
            // 基本类型: 单名字绑定.
            LOG_DEBUG(
                "Binding parameter '" + paramName + "' to argument '" +
                argRootName + "'"
            );

            std::pair<std::string, std::string> tempPair(
                paramName, argRootName
            );
            m_scopePtr->addBindSymbol(tempPair);
        }
    }

#ifdef ENABLE_DEBUGGER
    size_t privateFuncStartPC = m_generator.getCurrentPC();
    if (m_debugInfoGen) {
        apc_debug::SourceLocation loc = extractDebugLocation(node);
        bool isPublic = false;
        m_debugInfoGen
            ->onEnterFunction(node.name, isPublic, loc, privateFuncStartPC);

        for (const auto& param : node.parameters) {
            size_t paramIndex = &param - node.parameters.data();
            int stackOffset = static_cast<int>(
                node.parameters.size() - 1 - paramIndex
            );
            apc_debug::SourceLocation paramLoc = extractDebugLocation(node);
            m_debugInfoGen->onVariableDecl(
                param.name,
                param.type,
                paramLoc,
                true,
                stackOffset,
                true
            );
        }
    }
#endif

    std::vector<std::string> privateBaseStorageNames;
    const SymbolTable& privateEntryState = m_scopePtr->getCurrentSymtab();
    auto appendPrivateBaseName = [&](const StackElement& element) {
        if (std::find(
                privateBaseStorageNames.begin(),
                privateBaseStorageNames.end(),
                element.getName()
            ) == privateBaseStorageNames.end()) {
            privateBaseStorageNames.push_back(element.getName());
        }
    };
    if (privateEntryState.m_stackPtr) {
        for (const auto& element :
             privateEntryState.m_stackPtr->getStackContent()) {
            appendPrivateBaseName(element);
        }
    }
    if (privateEntryState.m_altStackPtr) {
        for (const auto& element :
             privateEntryState.m_altStackPtr->getStackContent()) {
            appendPrivateBaseName(element);
        }
    }
    m_privateFunctionBaseStorageNames.push_back(
        std::move(privateBaseStorageNames)
    );
    DEFER_BLOCK(m_privateFunctionBaseStorageNames.pop_back(););

    if (node.block) {
        LOG_DEBUG("Executing private function body inline");
        m_lastFlowResult = FlowResult::FallsThrough;
        const auto* previousLifetimePlan = m_currentLifetimePlan;
        m_currentLifetimePlan = lifetimePlanFor(node);
        DEFER_BLOCK(m_currentLifetimePlan = previousLifetimePlan;);
        node.block->accept(*this);

        cleanupFunctionParameters(node);
    }

    // Inline execution shares the caller's runtime stacks, but lexical
    // metadata must remain call-local. Publish only returned values, then
    // restore the caller snapshot exactly. A locally-created struct is first
    // detached under a unique compiler placeholder root so a same-named
    // caller object cannot be overwritten by the caller's assignment.
    SymbolTable& currentSymtab = m_scopePtr->getCurrentSymtab();
    auto& returnFrame = m_structReturnFrames.back();
    std::vector<std::pair<std::string, SymbolInfo>> publishedScopeEntries;
    std::vector<std::string> publishedDeclaredSymbols;
    std::vector<std::string> publishedNewSymbols;
    std::map<std::string, std::pair<size_t, size_t>> publishedWholeArrays;
    bool returnedCallerComposite = false;

    auto belongsToComposite = [](const std::string& name,
                                 const std::string& root) {
        return name == root ||
               (name.size() > root.size() &&
                name.compare(0, root.size(), root) == 0 &&
                name[root.size()] == '.');
    };
    auto renamedCompositeName = [](const std::string& name,
                                   const std::string& oldRoot,
                                   const std::string& newRoot) {
        return newRoot + name.substr(oldRoot.size());
    };

    if (!returnFrame.descriptor.has_value()) {
        bool publishedReturnValue = false;
        for (const std::string& returnedName : returnFrame.returnedValues) {
            auto position = currentSymtab.getPos(returnedName);
            if (!position.has_value() &&
                moveAltElementToMain(returnedName)) {
                position = currentSymtab.getPos(returnedName);
            }
            if (!position.has_value()) {
                // Mutually-exclusive lowercase returns may use different
                // source names for the same runtime slot. If lowering keeps
                // the then-branch metadata at the join, the else name is not
                // expected to remain addressable (and vice versa).
                continue;
            }

            tbc::CompilerPlaceholder placeholder;
            m_scopePtr->renameAtPosition(
                static_cast<int>(position.value()), placeholder.toString()
            );
            publishedReturnValue = true;
        }

        if (!returnFrame.returnedValues.empty() &&
            !publishedReturnValue) {
            const std::string& returnedName =
                returnFrame.returnedValues.front();
            const std::string message =
                "private function return value '" + returnedName +
                "' is no longer available";
            SourceLocation loc = getNodeLocation(node);
            SEMANTIC_ERROR(
                message,
                loc,
                "Preserve the returned value on the main or alt stack"
            );
            throw std::runtime_error(message);
        }
    } else {
        const std::string returnedRoot = returnFrame.descriptor->getName();
        const std::string returnedType = returnFrame.descriptor->getType();
        const bool callerHasComposite = std::any_of(
            savedCurrentScope.begin(),
            savedCurrentScope.end(),
            [&](const auto& entry) {
                return belongsToComposite(entry.first, returnedRoot);
            }
        );
        const bool calleeDeclaredComposite = std::any_of(
            currentSymtab.m_declaredSymbols.begin(),
            currentSymtab.m_declaredSymbols.end(),
            [&](const std::string& name) {
                return belongsToComposite(name, returnedRoot);
            }
        );
        returnedCallerComposite =
            returnFrame.returnedBoundComposite ||
            (callerHasComposite && !calleeDeclaredComposite);

        if (!returnedCallerComposite) {
            const std::string publishedRoot =
                tbc::CompilerPlaceholder().toString();

            for (const std::string& fieldName : returnFrame.returnedFields) {
                if (!belongsToComposite(fieldName, returnedRoot)) {
                    continue;
                }
                auto position = currentSymtab.getPos(fieldName);
                if (!position.has_value()) {
                    const std::string message =
                        "returned struct field '" + fieldName +
                        "' is no longer available";
                    SourceLocation loc = getNodeLocation(node);
                    SEMANTIC_ERROR(
                        message,
                        loc,
                        "Preserve every returned field on the main stack"
                    );
                    throw std::runtime_error(message);
                }
                m_scopePtr->renameAtPosition(
                    static_cast<int>(position.value()),
                    renamedCompositeName(
                        fieldName, returnedRoot, publishedRoot
                    )
                );
            }

            const size_t localMetadataStart = std::min(
                savedCurrentScope.size(), currentSymtab.m_currentScope.size()
            );
            for (size_t i = localMetadataStart;
                 i < currentSymtab.m_currentScope.size();
                 ++i) {
                auto entry = currentSymtab.m_currentScope[i];
                if (!belongsToComposite(entry.first, returnedRoot)) {
                    continue;
                }
                const std::string newName = renamedCompositeName(
                    entry.first, returnedRoot, publishedRoot
                );
                entry.first = newName;
                entry.second.m_stackElement.setName(newName);
                if (entry.second.isArray()) {
                    ArrayInfo& arrayInfo = entry.second.getArrayInfo();
                    arrayInfo.name = newName;
                    const size_t arraySize = arrayInfo.size;
                    arrayInfo.elements.clear();
                    arrayInfo.elements.reserve(arraySize);
                    for (size_t elementIndex = 0;
                         elementIndex < arraySize;
                         ++elementIndex) {
                        arrayInfo.elements.emplace_back(
                            newName,
                            arrayInfo.elementType,
                            elementIndex,
                            elementIndex
                        );
                    }
                } else if (entry.second.isCompoundType()) {
                    entry.second.getCompoundInfo().name = newName;
                }
                publishedScopeEntries.push_back(std::move(entry));
            }

            if (std::none_of(
                    publishedScopeEntries.begin(),
                    publishedScopeEntries.end(),
                    [&](const auto& entry) {
                        return entry.first == publishedRoot;
                    }
                )) {
                SymbolInfo rootInfo(publishedRoot, returnedType);
                rootInfo.setInitialized();
                publishedScopeEntries.emplace_back(
                    publishedRoot, std::move(rootInfo)
                );
            }

            auto captureNames = [&](const std::vector<std::string>& names,
                                    std::vector<std::string>& output) {
                for (const std::string& name : names) {
                    if (belongsToComposite(name, returnedRoot)) {
                        output.push_back(renamedCompositeName(
                            name, returnedRoot, publishedRoot
                        ));
                    }
                }
            };
            captureNames(
                currentSymtab.m_declaredSymbols,
                publishedDeclaredSymbols
            );
            captureNames(currentSymtab.m_newSymbol, publishedNewSymbols);
            if (std::find(
                    publishedDeclaredSymbols.begin(),
                    publishedDeclaredSymbols.end(),
                    publishedRoot
                ) == publishedDeclaredSymbols.end()) {
                publishedDeclaredSymbols.push_back(publishedRoot);
            }

            for (const auto& [name, info] : m_wholeArrayElements) {
                if (belongsToComposite(name, returnedRoot)) {
                    publishedWholeArrays.emplace(
                        renamedCompositeName(
                            name, returnedRoot, publishedRoot
                        ),
                        info
                    );
                }
            }

            returnFrame.descriptor = tbc::StackElement(
                publishedRoot, returnedType, publishedRoot
            );
        }
    }

    currentSymtab.m_currentScope = savedCurrentScope;
    currentSymtab.m_newSymbol = savedNewSymbols;
    currentSymtab.m_declaredSymbols = savedDeclaredSymbols;
    currentSymtab.m_fixedStackPtr->replaceStackContent(savedFixedStack);
    currentSymtab.m_fixedStackPtr->setCombinedStackSize(
        savedFixedCombinedSize
    );
    m_wholeArrayElements = savedWholeArrayElements;

    if (returnedCallerComposite) {
        // preserveStructReturn materialized every field on main. Discard stale
        // fixed copies restored from the caller snapshot.
        for (const std::string& fieldName : returnFrame.returnedFields) {
            std::string mutableName = fieldName;
            currentSymtab.removeFixed(mutableName);
        }
    } else if (returnFrame.descriptor.has_value()) {
        currentSymtab.m_currentScope.insert(
            currentSymtab.m_currentScope.end(),
            publishedScopeEntries.begin(),
            publishedScopeEntries.end()
        );
        currentSymtab.m_declaredSymbols.insert(
            currentSymtab.m_declaredSymbols.end(),
            publishedDeclaredSymbols.begin(),
            publishedDeclaredSymbols.end()
        );
        currentSymtab.m_newSymbol.insert(
            currentSymtab.m_newSymbol.end(),
            publishedNewSymbols.begin(),
            publishedNewSymbols.end()
        );
        m_wholeArrayElements.insert(
            publishedWholeArrays.begin(), publishedWholeArrays.end()
        );
    }

#ifdef ENABLE_DEBUGGER
    size_t privateFuncEndPC = m_generator.getCurrentPC();
    LOG_DEBUG(
        "Private function exit endPC: " + std::to_string(privateFuncEndPC)
    );
    if (m_debugInfoGen) {
        m_debugInfoGen->onExitFunction(privateFuncEndPC);
    }
#endif

    m_scopePtr->popScopeStack();

    // The root descriptor is compiler-only. Publishing it after all callee
    // cleanup keeps emitted runtime stack positions exact while still giving
    // the caller one typed value to consume immediately.
    if (m_structReturnFrames.back().descriptor.has_value()) {
        m_scopePtr->push(m_structReturnFrames.back().descriptor.value());
    }

    // 私有函数的 lowercase return 已在内联体内部完成控制流截断；
    // 对调用者而言，函数调用本身仍是一个普通表达式。
    m_lastFlowResult = FlowResult::FallsThrough;
    LOG_DEBUG("Private function inline resolution completed for: " + node.name);
}

void ASTToBytecodeVisitor::cleanupFunctionParameters(const FunctionNode& node)
{
    LOG_DEBUG("Cleaning up function parameters for: " + node.name);

    SymbolTable& symbolTable = m_scopePtr->getCurrentSymtab();
    auto isKeptReturnSymbol = [&](const std::string& name) {
        return std::find(
                   symbolTable.m_keepSymbol.begin(),
                   symbolTable.m_keepSymbol.end(),
                   name
               ) != symbolTable.m_keepSymbol.end();
    };

    for (int i = node.parameters.size() - 1; i >= 0; --i) {
        const std::string& paramName = node.parameters[i].name;
        const std::string& paramType = node.parameters[i].type;

        if (isKeptReturnSymbol(paramName)) {
            LOG_DEBUG(
                "Skipping stack cleanup for kept return parameter: " +
                paramName
            );
            continue;
        }

        LOG_DEBUG("Cleaning up parameter: " + paramName);

        const bool isArrayType =
            apc::util::parseFixedArrayType(paramType).has_value();
        bool isStructType = m_structDefinitions.find(paramType) !=
                            m_structDefinitions.end();

        if (isArrayType) {
            cleanupArrayParameter(paramName, paramType);
        } else if (isStructType) {
            cleanupStructParameter(paramName, paramType);
        } else {
            cleanupBasicParameter(paramName);
        }
    }

    // Bindings are restored from the exact pre-call snapshot by
    // privateFunctionResolution(). Removing only the root parameter name is
    // insufficient for flattened struct bindings (param.field -> arg.field)
    // and corrupts nested calls with reused parameter names.
}

void ASTToBytecodeVisitor::cleanupArrayParameter(
    const std::string& paramName,
    const std::string& paramType
)
{
    auto declaredType = apc::util::parseFixedArrayType(paramType);
    auto arrayInfo = m_scopePtr->getArrayInfo(paramName);
    if (!declaredType.has_value() || !arrayInfo.has_value()) {
        throw std::runtime_error(
            "invalid private fixed-array parameter metadata for '" +
            paramName + "'"
        );
    }

    if (m_structDefinitions.find(declaredType->elementType) !=
        m_structDefinitions.end()) {
        // Struct-array elements are represented solely by flattened leaves.
        for (size_t i = arrayInfo->size; i > 0; --i) {
            cleanupStructParameter(
                arrayInfo->getElementLabel(i - 1),
                declaredType->elementType
            );
        }
        return;
    }

    SymbolTable& currentSymtab = m_scopePtr->getCurrentSymtab();
    for (size_t i = 0; i < arrayInfo->size; ++i) {
        const std::string logicalElement = arrayInfo->getElementLabel(i);
        const std::string resolvedElement =
            currentSymtab.resolveBindSymbol(logicalElement);
        const bool isReturned = std::find(
                                    currentSymtab.m_keepSymbol.begin(),
                                    currentSymtab.m_keepSymbol.end(),
                                    resolvedElement
                                ) != currentSymtab.m_keepSymbol.end();
        if (isReturned) {
            continue;
        }

        auto position = currentSymtab.getPos(logicalElement);
        if (!position.has_value() &&
            currentSymtab.getPos(logicalElement, true).has_value()) {
            if (!moveAltElementToMain(logicalElement)) {
                throw std::runtime_error(
                    "failed to recover private array parameter element '" +
                    resolvedElement + "' from altstack"
                );
            }
            position = currentSymtab.getPos(logicalElement);
        }
        if (!position.has_value()) {
            continue;
        }

        emitRoll(position.value());
        currentSymtab.roll(position.value());
        m_generator.emit(tbc::BytOpcode::OP_DROP);
        currentSymtab.pop();
    }
}

void ASTToBytecodeVisitor::cleanupStructParameter(
    const std::string& paramName,
    const std::string& paramType
)
{
    LOG_DEBUG(
        "Cleaning up struct parameter: " + paramName + " of type: " + paramType
    );

    std::vector<std::pair<std::string, std::string>> fieldPathsAndTypes =
        getStructFieldsExpanded(paramType, paramName, m_structDefinitions);

    SymbolTable& currentSymtab = m_scopePtr->getCurrentSymtab();

    auto isReturnedField = [&](const std::string& fieldPath) {
        if (m_structReturnFrames.empty()) {
            return false;
        }
        const std::string resolvedField =
            currentSymtab.resolveBindSymbol(fieldPath);
        return m_structReturnFrames.back().returnedFields.count(resolvedField) !=
               0;
    };

    auto isKeptScalarField = [&](const std::string& fieldPath) {
        const std::string resolvedField =
            currentSymtab.resolveBindSymbol(fieldPath);
        return std::find(
                   currentSymtab.m_keepSymbol.begin(),
                   currentSymtab.m_keepSymbol.end(),
                   resolvedField
               ) != currentSymtab.m_keepSymbol.end() ||
               std::find(
                   currentSymtab.m_keepSymbol.begin(),
                   currentSymtab.m_keepSymbol.end(),
                   fieldPath
               ) != currentSymtab.m_keepSymbol.end();
    };

    // 逆序清理: 字段按声明顺序入栈, 弹出顺序与之相反.
    for (auto it = fieldPathsAndTypes.rbegin(); it != fieldPathsAndTypes.rend();
         ++it) {
        const std::string& fieldPath = it->first;
        const std::string& fieldType = it->second;

        if (isReturnedField(fieldPath) || isKeptScalarField(fieldPath)) {
            LOG_DEBUG("Preserving returned struct field: " + fieldPath);
            continue;
        }

        LOG_DEBUG(
            "Cleaning up struct field: " + fieldPath + " of type: " + fieldType
        );

        auto fieldPosOpt = m_scopePtr->getPos(fieldPath);
        if (fieldPosOpt.has_value()) {
            auto sfPos = fieldPosOpt.value();
            if (sfPos == 1) {
                // OP_NIP 直接删次栈顶字段, 省 2 字节.
                m_scopePtr->dropAt(1);
                m_generator.emit(tbc::BytOpcode::OP_NIP);
            } else {
                if (STACK_TOP_POS != sfPos) {
                    LOG_DEBUG("Moving field to stack top: " + fieldPath);
                    emitRoll(sfPos);
                    m_scopePtr->roll(sfPos);
                }
                LOG_DEBUG("Dropping struct field: " + fieldPath);
                m_generator.emit(tbc::BytOpcode::OP_DROP);
                m_scopePtr->pop();
            }
        } else {
            auto altFieldPosOpt = m_scopePtr->getPos(fieldPath, true);
            if (altFieldPosOpt.has_value()) {
                LOG_DEBUG(
                    "Preserving struct parameter field on altstack: " +
                    fieldPath
                );
            } else {
                // 字段已被消耗或优化, 不在栈中.
                LOG_DEBUG(
                    "Struct field not found on stack (may have been "
                    "consumed): " +
                    fieldPath
                );
            }
        }
    }

    LOG_DEBUG("Struct parameter cleanup completed: " + paramName);
}

void ASTToBytecodeVisitor::cleanupBasicParameter(const std::string& paramName)
{
    LOG_DEBUG("Cleaning up basic parameter: " + paramName);

    SymbolTable& currentSymtab = m_scopePtr->getCurrentSymtab();
    const std::string resolvedParam =
        currentSymtab.resolveBindSymbol(paramName);
    if (std::find(
            currentSymtab.m_keepSymbol.begin(),
            currentSymtab.m_keepSymbol.end(),
            resolvedParam
        ) != currentSymtab.m_keepSymbol.end()) {
        LOG_DEBUG("Preserving returned basic parameter: " + resolvedParam);
        return;
    }

    auto paramPosOpt = m_scopePtr->getPos(paramName);
    if (paramPosOpt.has_value()) {
        auto bpPos = paramPosOpt.value();
        if (bpPos == 1) {
            // OP_NIP 直接删次栈顶, 省 2 字节.
            m_scopePtr->dropAt(1);
            m_generator.emit(tbc::BytOpcode::OP_NIP);
        } else {
            if (STACK_TOP_POS != bpPos) {
                LOG_DEBUG("Moving parameter to stack top: " + paramName);
                emitRoll(bpPos);
                m_scopePtr->roll(bpPos);
            }
            LOG_DEBUG("Dropping basic parameter: " + paramName);
            m_generator.emit(tbc::BytOpcode::OP_DROP);
            m_scopePtr->pop();
        }
    } else {
        auto altParamPosOpt = m_scopePtr->getPos(paramName, true);
        if (altParamPosOpt.has_value()) {
            LOG_DEBUG(
                "Preserving basic parameter on altstack: " + paramName
            );
        } else {
            // 参数已被消耗或优化, 不在栈中.
            LOG_DEBUG(
                "Basic parameter not found on stack (may have been "
                "consumed): " +
                paramName
            );
        }
    }

    LOG_DEBUG("Basic parameter cleanup completed: " + paramName);
}

void ASTToBytecodeVisitor::emitRoll(int64_t pos)
{
    if (pos == 0) {
        return;
    }
    // [1]OP_ROLL -> OP_SWAP (省 1 字节).
    if (pos == 1) {
        m_generator.emit(tbc::BytOpcode::OP_SWAP);
        return;
    }
    // [2]OP_ROLL -> OP_ROT.
    if (pos == 2) {
        m_generator.emit(tbc::BytOpcode::OP_ROT);
        return;
    }
    m_generator.emit(numberToScriptHex(pos));
    m_generator.emit(tbc::BytOpcode::OP_ROLL);
}

void ASTToBytecodeVisitor::emitPick(int64_t pos)
{
    if (pos == 0) {
        m_generator.emit(tbc::BytOpcode::OP_DUP);
        return;
    }
    if (pos == 1) {
        m_generator.emit(tbc::BytOpcode::OP_OVER);
        return;
    }
    m_generator.emit(numberToScriptHex(pos));
    m_generator.emit(tbc::BytOpcode::OP_PICK);
}

std::optional<int64_t> ASTToBytecodeVisitor::resolveCompileTimeIndex(
    const ExprNode& expr
) const
{
    return resolveCompileTimeInteger(expr, false);
}

std::optional<int64_t> ASTToBytecodeVisitor::resolveCompileTimeCondition(
    const ExprNode& expr
) const
{
    return resolveCompileTimeInteger(expr, true);
}

std::optional<int64_t> ASTToBytecodeVisitor::resolveCompileTimeInteger(
    const ExprNode& expr,
    bool requireNumericFixedValue
) const
{
    const auto result = evaluateCompileTimeInteger(
        expr, requireNumericFixedValue
    );
    return result.isKnown() ? std::optional<int64_t>(result.value)
                            : std::nullopt;
}

compiler::StaticIntegerResult
ASTToBytecodeVisitor::evaluateCompileTimeInteger(
    const ExprNode& expr,
    bool requireNumericFixedValue
) const
{
    return compiler::StaticIntegerEvaluator::evaluate(
        expr,
        [this, requireNumericFixedValue](const IdentifierNode& identifier) {
            std::string name = identifier.name;

            // A materialized runtime slot always supersedes a fixed copy.
            if (m_scopePtr->getPos(name).has_value() ||
                m_scopePtr->getPos(name, true).has_value()) {
                return compiler::StaticIntegerResult::unknown();
            }

            auto fixed = m_scopePtr->getFixed(name);
            if (!fixed.has_value()) {
                return compiler::StaticIntegerResult::unknown();
            }
            const std::string& encoded = fixed->getData().empty()
                                             ? fixed->getName()
                                             : fixed->getData();
            if (requireNumericFixedValue) {
                const std::string& type = fixed->getType();
                const bool isNumericType =
                    type == "int" || type == "num" || type == "number" ||
                    type == "uint64";
                if (!isNumericType || !isStrictScriptNumberHex(encoded)) {
                    return compiler::StaticIntegerResult::unknown();
                }
            }
            try {
                return compiler::StaticIntegerResult::known(
                    scriptHexToNumber(encoded)
                );
            } catch (...) {
                return compiler::StaticIntegerResult::unknown();
            }
        }
    );
}

BytecodeType ASTToBytecodeVisitor::inferLiteralType(const LiteralNode& node)
{
    switch (node.type) {
        case LiteralNode::Type::Boolean:
            return BytecodeType::Boolean;
        case LiteralNode::Type::Number:
            return BytecodeType::Number;
        case LiteralNode::Type::String:
            return BytecodeType::String;
        case LiteralNode::Type::Addr:
            return BytecodeType::Addr;
        case LiteralNode::Type::Hex: {
            // 按 hex 数据特征精化类型.
            size_t dataSize = tbc::TypeValidator::getHexDataSize(node.value);

            if (tbc::TypeValidator::isValidPubKey(node.value)) {
                return BytecodeType::PubKey;
            }

            if (dataSize == 32 &&
                tbc::TypeValidator::isValidHash(node.value, 32)) {
                return BytecodeType::Sha256;
            }
            if (dataSize == 20 &&
                tbc::TypeValidator::isValidHash(node.value, 20)) {
                return BytecodeType::Ripemd160;
            }

            if (dataSize == 32 &&
                tbc::TypeValidator::isValidPrivKey(node.value)) {
                return BytecodeType::PrivKey;
            }

            if (tbc::TypeValidator::isValidDERSignature(node.value)) {
                return BytecodeType::Sig;
            }

            return BytecodeType::Hex;
        }
        default:
            return BytecodeType::String;
    }
}

void ASTToBytecodeVisitor::reportTypeError(
    const std::string& context,
    BytecodeType expectedType,
    const std::string& actualValue
)
{
    std::string errorMsg =
        context + " - " +
        tbc::TypeValidator::getValidationError(expectedType, actualValue);
    LOG_ERROR(errorMsg);

    auto constraints = tbc::TypeValidator::getConstraints(expectedType);
    LOG_ERROR("Type requirement: " + constraints.description);

    if (constraints.fixedSize > 0) {
        LOG_ERROR(
            "Expected fixed size: " + std::to_string(constraints.fixedSize) +
            " bytes"
        );
    } else {
        LOG_ERROR(
            "Expected size range: " + std::to_string(constraints.minSize) +
            " - " + std::to_string(constraints.maxSize) + " bytes"
        );
    }
}

bool ASTToBytecodeVisitor::isTypeCompatible(
    const std::string& declaredType,
    const tbc::StackElement& valueElement,
    bool isArrayType
) const
{
    if (declaredType.empty() || declaredType == "auto" || isArrayType) {
        return true;
    }
    if (!declaredType.empty() && declaredType.front() == '{') {
        return true;
    }

    const std::string actualType = valueElement.getType();
    const std::string valueName = valueElement.getName();
    const std::string valueData = valueElement.getData();

    // self.X and other deployment-time placeholders do not carry enough type
    // information at compile time. Keep accepting them for annotated locals.
    if (actualType == "builtin_member" ||
        (isScript(valueName) && valueName.front() == '<')) {
        return true;
    }

    auto lower = [](std::string text) {
        for (char& ch : text) {
            ch = static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))
            );
        }
        return text;
    };

    const std::string declared = lower(declaredType);
    const std::string actual = lower(actualType);

    if (declared == actual) {
        return true;
    }

    if (declared == "int" || declared == "number" || declared == "uint64") {
        return actual == "int" || actual == "num" || actual == "number" ||
               actual == "uint64";
    }

    if (declared == "bool" || declared == "boolean") {
        if (actual == "bool" || actual == "boolean") {
            return true;
        }
        if (actual == "int" || actual == "num" || actual == "number") {
            return valueName == "0x00" || valueName == "0x51" ||
                   valueData == "0x00" || valueData == "0x51";
        }
        return false;
    }

    if (declared == "hex" || declared.rfind("hex", 0) == 0) {
        return actual == "hex" || actual == "pubkey" ||
               actual == "signature" || actual == "sig" ||
               actual == "ripemd160" || actual == "pubkeyhash" ||
               actual == "sha1" || actual == "sha256" ||
               actual == "privkey";
    }

    if (declared == "string") {
        return actual == "string";
    }

    if (declared == "address") {
        return actual == "address";
    }

    return false;
}

void ASTToBytecodeVisitor::validateDeclaredType(
    const std::string& context,
    const std::string& declaredType,
    const tbc::StackElement& valueElement,
    const ASTNode& node,
    bool isArrayType
) const
{
    if (isTypeCompatible(declaredType, valueElement, isArrayType)) {
        return;
    }

    SourceLocation loc = getNodeLocation(node);
    std::ostringstream oss;
    oss << context << ": expected '" << declaredType << "' but got '"
        << valueElement.getType() << "'";
    TYPE_ERROR(
        oss.str(),
        loc,
        "Ensure the expression value matches the declared type"
    );
    LOG_ERROR(oss.str());
    throw std::runtime_error(oss.str());
}

std::optional<tbc::StackElement> ASTToBytecodeVisitor::processMethodCallObject(
    const MethodCallNode& node
)
{
    LOG_DEBUG(
        "Processing object expression for method call: " + node.methodName
    );
    visitExpr(*node.object);
    auto stackTop = m_scopePtr->pop();

    if (!stackTop.has_value()) {
        std::ostringstream oss;
        oss << "Stack is empty when trying to pop object for method call '"
            << node.methodName << "' at line " << node.pos.first << ", column "
            << node.pos.second;
        SourceLocation loc("", node.pos.first, node.pos.second);
        SEMANTIC_ERROR(
            oss.str(), loc, "Check the object expression before the method call"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    // 结构体数组字段的子字段直接返回.
    const std::string& objName = stackTop.value().getName();
    if (isStructArrayFieldSubfield(objName)) {
        LOG_DEBUG("Object is a struct array field subfield: " + objName);
        return stackTop;
    }

    auto elementPosOpt = m_scopePtr->getPos(stackTop.value());

    if (!elementPosOpt.has_value()) {
        std::string objName = stackTop.value().getName();
        auto fixedElement = m_scopePtr->getFixed(objName);

        if (fixedElement.has_value()) {
            LOG_DEBUG(
                "Object '" + objName +
                "' found in fixed area for method call: " + node.methodName
            );

            // fixed 区对象需把数据 emit 到栈, 供后续 slice 等使用.
            if (fixedElement->getType() == "builtin_member") {
                // 内置对象成员: 直接 emit 操作码 (生成器按字节切分).
                std::string opcodeHex = fixedElement->getData();
                m_generator.emit(opcodeHex);
            } else {
                std::string dataHex = fixedElement->getData();
                m_generator.emit(dataHex);
            }

            m_scopePtr->push(*fixedElement);

            LOG_DEBUG(
                "Fixed object processed successfully, name: " +
                fixedElement->getName()
            );
            return fixedElement;
        }

        std::ostringstream oss;
        oss << "Cannot find stack position for object '"
            << stackTop.value().getName() << "' in method call '"
            << node.methodName << "' at line " << node.pos.first << ", column "
            << node.pos.second;
        SourceLocation loc("", node.pos.first, node.pos.second);
        SEMANTIC_ERROR(
            oss.str(), loc, "Check if the object variable is properly defined"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    LOG_DEBUG("Object processed successfully, name: " + stackTop->getName());
    return stackTop;
}

void ASTToBytecodeVisitor::processGenericFunctionCall(
    const std::string& functionName,
    const std::vector<std::unique_ptr<ExprNode>>& args,
    const ExprNode& node,
    std::optional<tbc::StackElement> objectElement
)
{
    bool isMethodCall = objectElement.has_value();
    std::string callType = isMethodCall ? "method call" : "function call";

    LOG_DEBUG(
        "Processing " + callType + ": " + functionName + " with " +
        std::to_string(args.size()) + " arguments"
    );

    const bool isDelete = functionName == "Delete";
    ScopeRollbackGuard deleteStateGuard(m_scopePtr, isDelete);
    std::vector<std::string> deleteTargetNames;
    struct PreservedDeleteArray
    {
        ArrayInfo info;
        bool externalView{false};
    };
    std::vector<PreservedDeleteArray> preservedDeleteArrays;

    // Delete consumes l-values rather than their values. Resolve every target
    // before changing fixed storage, then remove all matching fixed leaves in
    // one transaction. This covers scalars, struct fields and array elements
    // without making visitIdentifier()/visitFieldAccess() materialize them.
    if (isDelete) {
        LOG_DEBUG(
            "Special handling for Delete function with " +
            std::to_string(args.size()) + " arguments"
        );

        std::function<std::optional<std::string>(const ExprNode&)>
            resolveDeleteTarget;
        resolveDeleteTarget = [&](const ExprNode& expression)
            -> std::optional<std::string> {
            if (const auto* identifier =
                    dynamic_cast<const IdentifierNode*>(&expression)) {
                return identifier->name;
            }
            if (const auto* field =
                    dynamic_cast<const FieldAccessNode*>(&expression)) {
                auto base = resolveDeleteTarget(*field->base);
                if (!base.has_value()) {
                    return std::nullopt;
                }
                return base.value() + "." + field->field;
            }
            if (const auto* index =
                    dynamic_cast<const IndexAccessNode*>(&expression)) {
                auto base = resolveDeleteTarget(*index->base);
                auto resolvedIndex = resolveCompileTimeIndex(*index->index);
                if (!base.has_value() || !resolvedIndex.has_value() ||
                    resolvedIndex.value() < 0) {
                    return std::nullopt;
                }
                return m_scopePtr->getArrayElementLabel(
                    base.value(), static_cast<size_t>(resolvedIndex.value())
                );
            }
            return std::nullopt;
        };

        auto isSameOrDescendant = [](
                                      const std::string& candidate,
                                      const std::string& root
                                  ) {
            return candidate == root || candidate.find(root + ".") == 0 ||
                   candidate.find(root + "[") == 0;
        };

        auto& symbolTable = m_scopePtr->getCurrentSymtab();
        deleteTargetNames.reserve(args.size());
        for (const auto& arg : args) {
            auto target = resolveDeleteTarget(*arg);
            if (!target.has_value()) {
                SourceLocation loc = getNodeLocation(*arg);
                const std::string message =
                    "Delete() target must be a variable, field, or "
                    "compile-time array element";
                SEMANTIC_ERROR(
                    message,
                    loc,
                    "Pass an assignable storage location to Delete()"
                );
                throw std::invalid_argument(message);
            }
            for (const auto& claimed : deleteTargetNames) {
                if (isSameOrDescendant(target.value(), claimed) ||
                    isSameOrDescendant(claimed, target.value())) {
                    throw std::invalid_argument(
                        "Delete() contains duplicate or overlapping target '" +
                        target.value() + "'"
                    );
                }
            }
            if (auto arrayInfo = m_scopePtr->getArrayInfo(target.value())) {
                preservedDeleteArrays.push_back(
                    {*arrayInfo,
                     symbolTable.isExternalArrayView(target.value())}
                );
            }
            deleteTargetNames.push_back(target.value());
        }

        std::vector<StackElement> fixedEntries;
        if (symbolTable.m_fixedStackPtr) {
            fixedEntries = symbolTable.m_fixedStackPtr->getStackContent();
        }
        std::vector<std::string> fixedTargets;
        std::unordered_set<std::string> claimedFixedTargets;
        for (const auto& target : deleteTargetNames) {
            std::vector<std::string> physicalRoots;
            const size_t bindingStart = symbolTable.activeBindSymbolStart();
            for (size_t bindingIndex = bindingStart;
                 bindingIndex < symbolTable.m_bindSymbol.size();
                 ++bindingIndex) {
                const auto& binding = symbolTable.m_bindSymbol[bindingIndex];
                if (isSameOrDescendant(binding.first, target)) {
                    physicalRoots.push_back(
                        symbolTable.resolveBindSymbol(binding.first)
                    );
                }
            }
            const bool targetsActiveBinding = !physicalRoots.empty();
            if (!targetsActiveBinding) {
                physicalRoots.push_back(target);
                physicalRoots.push_back(
                    symbolTable.resolveBindSymbol(target)
                );
                const size_t scopeEntryStart =
                    symbolTable.activeScopeEntryStart();
                for (size_t scopeIndex = scopeEntryStart;
                     scopeIndex < symbolTable.m_currentScope.size();
                     ++scopeIndex) {
                    const std::string logicalName =
                        symbolTable.m_currentScope[scopeIndex]
                            .second.getSymbolName();
                    if (isSameOrDescendant(logicalName, target)) {
                        physicalRoots.push_back(
                            symbolTable.resolveBindSymbol(logicalName)
                        );
                    }
                }
            }

            std::unordered_set<std::string> fixedForArgument;
            for (const auto& fixed : fixedEntries) {
                const std::string& fixedName = fixed.getName();
                for (const auto& root : physicalRoots) {
                    if (isSameOrDescendant(fixedName, root)) {
                        fixedForArgument.insert(fixedName);
                        break;
                    }
                }
            }
            for (const auto& fixedName : fixedForArgument) {
                if (!claimedFixedTargets.insert(fixedName).second) {
                    throw std::invalid_argument(
                        "Delete() contains aliased fixed target '" +
                        fixedName + "'"
                    );
                }
                fixedTargets.push_back(fixedName);
            }
        }

        for (auto fixedName : fixedTargets) {
            LOG_INFO("Deleting fixed data: " + fixedName);
            m_scopePtr->removeFixed(fixedName);
        }

        LOG_DEBUG("Fixed target preflight completed");
    }

    // Size: 不消耗参数.
    if ("Size" == functionName) {
        LOG_DEBUG("Special handling for Size function - non-consuming operation"
        );

        if (args.size() != 1) {
            SourceLocation loc("", 0, 0);
            SEMANTIC_ERROR(
                "Size function expects exactly 1 argument, got " +
                    std::to_string(args.size()),
                loc,
                "Check the Size function call"
            );
        }

        visitExpr(*args[0]);
        auto argResultOpt = m_scopePtr->top();
        std::string elementStr = argResultOpt.getName();
        if (isScript(argResultOpt.getName())) {
            m_generator.emit(elementStr);
        } else if (!CompilerPlaceholder::isPlaceholder(elementStr)) {
            m_scopePtr->pop();
        }

        // Size 返回 int, 入占位符; OP_SIZE 不弹参数.
        CompilerPlaceholder ph;
        m_scopePtr->push(ph.toString(), "", ph.toString());

        m_generator.emit(tbc::BytOpcode::OP_SIZE);

        LOG_DEBUG("Size function processed - argument preserved on stack");
        return;
    }

    // 整体数组元素的 Clone(): 注册克隆出的占位符.
    if ("Clone" == functionName) {
        LOG_DEBUG("Processing clone method call");

        const std::string& objectName = objectElement.value().getName();
        auto elementPosOpt = m_scopePtr->getPos(objectName);

        if (isStructArrayFieldSubfield(objectName) &&
            !elementPosOpt.has_value()) {
            LOG_DEBUG(
                "Clone target is struct array field subfield: " + objectName
            );

            m_generator.emit(tbc::BytOpcode::OP_PICK);

            CompilerPlaceholder ph;
            m_scopePtr->push(ph.toString(), "", ph.toString());
            return;
        }

        if (isMethodCall && objectElement.has_value()) {
            const std::string& objectName = objectElement.value().getName();
            if (isWholeArrayElement(objectName)) {
                auto arrayInfo = getWholeArrayInfo(objectName);
                if (arrayInfo.has_value()) {
                    auto [arraySize, elementByteSize] = arrayInfo.value();
                    // clone 后栈顶占位符需注册为整体数组元素.
                    auto topElement = m_scopePtr->top();
                    std::string topPlaceholderName = topElement.getName();
                    registerWholeArrayElement(
                        topPlaceholderName, arraySize, elementByteSize
                    );
                    LOG_DEBUG(
                        "Registered temporary clone placeholder: " +
                        topPlaceholderName
                    );
                }
            }
        }
    }

    // OpFunction: 仅普通函数调用走此路径.
    if (auto opFuncPtr =
            tbc::OpFunctionFactory::createFunction(functionName, args.size())) {
        LOG_DEBUG("Found OpFunction: " + functionName);

        int expectedArgCount = opFuncPtr->getExpectedArgCount();
        auto processedArgs =
            processArguments(args, expectedArgCount, functionName);

#ifdef ENABLE_DEBUGGER
        // Argument visits update the generator location.  Attribute the stack
        // layout and the consuming opcode to the call expression itself.
        setCurrentLocationForGenerator(node);
#endif

        // Preserve source evaluation order, but let commutative binary
        // opcodes choose the cheaper physical stack layout afterwards.
        if (processedArgs.size() == 2 &&
            !opFuncPtr->isArgOrderSensitive()) {
            auto swappedArgs = processedArgs;
            std::swap(swappedArgs[0], swappedArgs[1]);
            const auto originalCost =
                estimateArgumentLayoutCost(*m_scopePtr, processedArgs);
            const auto swappedCost =
                estimateArgumentLayoutCost(*m_scopePtr, swappedArgs);
            if (originalCost.has_value() && swappedCost.has_value() &&
                swappedCost.value() < originalCost.value()) {
                LOG_DEBUG(
                    "Commutative argument layout selected for ",
                    functionName,
                    ": ",
                    originalCost.value(),
                    " -> ",
                    swappedCost.value(),
                    " bytes"
                );
                processedArgs = std::move(swappedArgs);
            }
        }
        adjustStackToMatch(processedArgs);
        for (size_t i = 0; i < opFuncPtr->getReturnCount(); ++i) {
            CompilerPlaceholder ph;
            m_scopePtr->push(ph.toString(), "",
                             ph.toString()); // TODO
        }
        m_generator.emit(opFuncPtr->getOpcodeEnum());
        return;
    }

    if (auto builtFunPtr = tbc::BuiltinFunctionFactory::createFunction(
            functionName, args.size()
        )) {
        LOG_DEBUG("Found BuiltinFunction: " + functionName);

        if (functionName == "Slice" &&
            (!isMethodCall || !objectElement.has_value())) {
            SourceLocation loc = getNodeLocation(node);
            SEMANTIC_ERROR(
                "Slice must be called as a method on a byte string",
                loc,
                "Use value.Slice(start, length)"
            );
            throw std::runtime_error(
                "Slice must be called as a method on a byte string"
            );
        }

        if (!isMethodCall &&
            tryProcessCompositeArrayBuiltin(functionName, args, node)) {
            deleteStateGuard.commit();
            return;
        }

        // SetAlt needs a real named runtime slot. Fixed values normally stay
        // compiler-only and visitIdentifier creates only a temporary encoded
        // argument, which the generic argument collector immediately removes.
        // Materialize the fixed identifier under its logical name first so
        // both the emitted opcode and the symbolic main/alt stacks agree.
        if (functionName == "SetAlt" && args.size() == 1) {
            if (const auto* identifier =
                    dynamic_cast<const IdentifierNode*>(args[0].get())) {
                std::string name = identifier->name;
                if (!m_scopePtr->getPos(name).has_value()) {
                    if (auto fixed = m_scopePtr->getFixed(name)) {
                        const std::string encoded = fixed->getData().empty()
                                                        ? fixed->getName()
                                                        : fixed->getData();
                        if (!isScript(encoded)) {
                            SourceLocation loc = getNodeLocation(node);
                            const std::string message =
                                "cannot materialize fixed value '" + name +
                                "' for SetAlt";
                            SEMANTIC_ERROR(
                                message,
                                loc,
                                "Move a concrete scalar value to altstack"
                            );
                            throw std::runtime_error(message);
                        }
                        m_generator.emit(encoded);
                        m_scopePtr->removeFixed(name);
                        m_scopePtr->push(name, fixed->getType(), encoded);
                    }
                }
            }
        }

        int expectedArgCount = builtFunPtr->getExpectedArgCount();
        std::vector<tbc::StackElement> processedArgs;
        if (isDelete) {
            processedArgs.reserve(deleteTargetNames.size());
            for (const auto& target : deleteTargetNames) {
                processedArgs.emplace_back(target);
            }
        } else {
            processedArgs =
                processArguments(args, expectedArgCount, functionName);
        }

        if (functionName == "Slice") {
            processSliceBuiltin(objectElement.value(), processedArgs, node);
            return;
        }

        if ("Keep" == functionName) {
            keep(processedArgs);
            return;
        }
        auto opcodeHex = builtFunPtr->getOpcodeHex(
            isMethodCall ? objectElement.value() : StackElement(),
            processedArgs,
            m_scopePtr
        );
        if (functionName == "Push") {
            opcodeHex = appendSelfPlaceholderLengths(opcodeHex, node);
        }
        auto& currentSymtab = m_scopePtr->getCurrentSymtab();
        if ("SetMain" == functionName) {
            // 仅将指定变量从副栈移回主栈, 其余元素保持原序.
            if (!processedArgs.empty()) {
                const std::string& varName = processedArgs[0].getName();
                auto varPosOpt =
                    currentSymtab.getPos(processedArgs[0], true);
                if (varPosOpt.has_value()) {
                    auto position = varPosOpt.value();
                    StackElement target =
                        currentSymtab.m_altStackPtr->at(position);
                    currentSymtab.m_altStackPtr->erase(position);
                    currentSymtab.m_stackPtr->push(target);
                } else {
                    LOG_ERROR(
                        "setMain: cannot find variable '" + varName +
                        "' in altstack symbol table during stack operation"
                    );
                }
            }
        } else if ("SetAlt" == functionName) {
            // 先 roll 到栈顶, 再移到副栈.
            if (!processedArgs.empty()) {
                const std::string& varName = processedArgs[0].getName();
                auto varPosOpt = currentSymtab.getPos(processedArgs[0]);

                if (varPosOpt.has_value()) {
                    auto position = varPosOpt.value();
                    if (position > 0) {
                        m_scopePtr->roll(position);
                    }
                    currentSymtab.m_altStackPtr->moveTopToStack(
                        *currentSymtab.m_stackPtr.get(),
                        true // 共享副栈下强制允许.
                    );
                } else {
                    LOG_ERROR(
                        "setAlt: cannot find variable '" + varName +
                        "' in symbol table during stack operation"
                    );
                }
            }
        }
        if (isConsumeFun(functionName) && isMethodCall) {
            auto objPosOpt = m_scopePtr->getPos(objectElement.value());
            if (objPosOpt.has_value()) {
                m_scopePtr->roll(objPosOpt.value());
                m_scopePtr->pop();
            }
        }

        bool skipReturnPlaceholder = (functionName == "Range");

        if (!skipReturnPlaceholder) {
            for (size_t i = 0; i < builtFunPtr->getReturnCount(); ++i) {
                CompilerPlaceholder ph;
                m_scopePtr->push(ph.toString(), "",
                                 ph.toString()); // TODO
            }
        }

        // Delete consumes the current values, but an array declaration remains
        // a valid l-value container. Re-register only the root metadata after
        // the builtin has removed its physical elements, so a later
        // `Delete(values); values[0] = source` can materialize that slot again.
        for (const auto& preserved : preservedDeleteArrays) {
            if (m_scopePtr->getArrayInfo(preserved.info.name).has_value()) {
                continue;
            }
            bool restored = false;
            if (preserved.externalView) {
                restored = currentSymtab.importExternalArrayView(
                    preserved.info.name,
                    preserved.info.elementType,
                    preserved.info.size,
                    preserved.info.isFixedSize
                );
            } else {
                restored = m_scopePtr->defineArray(
                    preserved.info.name,
                    preserved.info.elementType,
                    preserved.info.size,
                    preserved.info.isFixedSize
                );
            }
            if (!restored) {
                throw std::runtime_error(
                    "failed to restore array declaration after Delete(): " +
                    preserved.info.name
                );
            }
        }

        m_generator.emit(opcodeHex);
        deleteStateGuard.commit();
        return;
    }

    if (auto privateFuncIt = m_privateFunctions.find(functionName);
        privateFuncIt != m_privateFunctions.end()) {
        LOG_DEBUG("Found private function: " + functionName);

        auto processedArgs = processArguments(
            args, privateFuncIt->second->parameters.size(), functionName
        );

        const auto& funcParams = privateFuncIt->second->parameters;
        processArgsToTop(processedArgs, funcParams);

        privateFunctionResolution(*privateFuncIt->second, processedArgs);

        LOG_DEBUG(
            "Successfully processed private function call: " + functionName
        );
        return;
    }

    SourceLocation loc("", node.pos.first, node.pos.second);
    std::ostringstream errorStream;
    errorStream << "undefined " << callType << ": " << functionName << " with "
                << args.size() << " arguments";

    SEMANTIC_ERROR(
        errorStream.str(),
        loc,
        "Check if the function name is spelled correctly and is "
        "properly declared"
    );
    LOG_ERROR(errorStream.str());
    throw std::runtime_error(errorStream.str());
}

void ASTToBytecodeVisitor::processSliceBuiltin(
    const StackElement& objectElement,
    const std::vector<StackElement>& processedArgs,
    const ExprNode& node
)
{
    if (processedArgs.size() != 2) {
        SourceLocation loc = getNodeLocation(node);
        SEMANTIC_ERROR(
            "Slice expects exactly two arguments",
            loc,
            "Use value.Slice(start, length)"
        );
        throw std::runtime_error("Slice expects exactly two arguments");
    }

    enum class SliceOperandKind { Immediate, InlineScript, StackValue };

    struct SliceOperand
    {
        StackElement element;
        SliceOperandKind kind;
        std::optional<int64_t> immediateValue;
    };

    auto isAnglePlaceholder = [](const std::string& name) {
        return name.size() >= 2 && name.front() == '<' && name.back() == '>';
    };

    auto classify = [&](const StackElement& element) -> SliceOperand {
        const std::string& name = element.getName();

        // Builtin members may use a 0x-prefixed opcode string as their name,
        // but they are runtime scripts rather than numeric immediates.
        if (element.getType() == "builtin_member" ||
            isAnglePlaceholder(name)) {
            return {element, SliceOperandKind::InlineScript, std::nullopt};
        }

        if (name.size() >= 2 &&
            (name.starts_with("0x") || name.starts_with("0X"))) {
            if (!isStrictScriptNumberHex(name)) {
                SourceLocation loc = getNodeLocation(node);
                SEMANTIC_ERROR(
                    "Slice bound is not a valid Script number",
                    loc,
                    "Use an integer offset or length"
                );
                throw std::runtime_error(
                    "Slice bound is not a valid Script number"
                );
            }
            return {
                element,
                SliceOperandKind::Immediate,
                scriptHexToNumber(name)
            };
        }

        return {element, SliceOperandKind::StackValue, std::nullopt};
    };

    SliceOperand start = classify(processedArgs[0]);
    SliceOperand length = classify(processedArgs[1]);

    const bool startIsBeginning =
        start.immediateValue.has_value() &&
        (start.immediateValue.value() == -1 ||
         start.immediateValue.value() == 0);
    const bool lengthIsToEnd =
        length.immediateValue.has_value() &&
        length.immediateValue.value() == -1;

    if (startIsBeginning && lengthIsToEnd) {
        SourceLocation loc = getNodeLocation(node);
        SEMANTIC_ERROR(
            "Slice does not support two boundary sentinel arguments",
            loc,
            "Use an explicit length or a non-boundary start offset"
        );
        throw std::runtime_error(
            "Slice does not support two boundary sentinel arguments"
        );
    }

    // Validate every existing runtime slot before emitting or mutating either
    // stack. This keeps a failed Slice call atomic from the compiler's view.
    std::unordered_set<int64_t> runtimePositions;
    auto requireUniqueStackValue = [&](const StackElement& element,
                                       const std::string& role) {
        auto pos = m_scopePtr->getPos(element.getName());
        if (!pos.has_value()) {
            SourceLocation loc = getNodeLocation(node);
            std::ostringstream oss;
            oss << "Slice cannot find " << role << " '" << element.getName()
                << "' on the main stack";
            SEMANTIC_ERROR(
                oss.str(), loc, "Ensure the value has not already been consumed"
            );
            throw std::runtime_error(oss.str());
        }

        if (!runtimePositions.insert(pos.value()).second) {
            SourceLocation loc = getNodeLocation(node);
            std::ostringstream oss;
            oss << "Slice " << role << " '" << element.getName()
                << "' reuses the same runtime stack slot as another input";
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Clone the value before using it for multiple Slice inputs"
            );
            throw std::runtime_error(oss.str());
        }
    };

    requireUniqueStackValue(objectElement, "object");
    if (start.kind == SliceOperandKind::StackValue) {
        requireUniqueStackValue(start.element, "start argument");
    }
    if (length.kind == SliceOperandKind::StackValue) {
        requireUniqueStackValue(length.element, "length argument");
    }

    // Materialize one operand at the VM top and mirror that exact slot in the
    // symbolic stack. Immediates/inline scripts were popped by processArguments
    // and therefore need a fresh, unique compiler-only name after emission.
    auto materialize = [&](const SliceOperand& operand) {
        if (operand.kind == SliceOperandKind::StackValue) {
            auto pos = m_scopePtr->getPos(operand.element.getName());
            if (!pos.has_value()) {
                throw std::runtime_error(
                    "Slice operand disappeared during stack preparation: " +
                    operand.element.getName()
                );
            }
            if (pos.value() != STACK_TOP_POS) {
                emitRoll(pos.value());
                m_scopePtr->roll(pos.value());
            }
            return;
        }

        std::string script = operand.element.getName();
        if (operand.element.getType() == "builtin_member" &&
            !operand.element.getData().empty()) {
            script = operand.element.getData();
        }
        m_generator.emit(script);

        CompilerPlaceholder emitted;
        m_scopePtr->push(
            emitted.toString(), operand.element.getType(), emitted.toString()
        );
    };

    auto materializeObject = [&]() {
        auto pos = m_scopePtr->getPos(objectElement.getName());
        if (!pos.has_value()) {
            throw std::runtime_error(
                "Slice object disappeared during stack preparation: " +
                objectElement.getName()
            );
        }
        if (pos.value() != STACK_TOP_POS) {
            emitRoll(pos.value());
            m_scopePtr->roll(pos.value());
        }
    };

    // Avoid emitting a sequence of rolls which only permutes an operand block
    // away from, and then back into, the order OP_SPLIT already requires.
    // The vector is expressed from the bottom of the block to its VM top.
    auto stackBlockIsReady =
        [&](const std::vector<const StackElement*>& operands) {
            for (size_t i = 0; i < operands.size(); ++i) {
                auto pos = m_scopePtr->getPos(operands[i]->getName());
                const long expectedPos =
                    static_cast<long>(operands.size() - i - 1);
                if (!pos.has_value() || pos.value() != expectedPos) {
                    return false;
                }
            }
            return true;
        };

    auto prepareObjectAnd = [&](const SliceOperand& operand) {
        if (operand.kind == SliceOperandKind::StackValue &&
            stackBlockIsReady({&objectElement, &operand.element})) {
            return;
        }
        materializeObject();
        materialize(operand);
    };

    // Both supported OP_SPLIT tails have the same net stack effect: two input
    // slots become one result slot. keepRight selects suffix vs prefix.
    auto splitToResult = [&](bool keepRight) {
        if (m_scopePtr->size() < 2) {
            SourceLocation loc = getNodeLocation(node);
            SEMANTIC_ERROR(
                "Slice symbolic stack underflow",
                loc,
                "Internal compiler error while applying OP_SPLIT"
            );
            throw std::runtime_error("Slice symbolic stack underflow");
        }

        m_generator.emit(tbc::BytOpcode::OP_SPLIT);
        m_generator.emit(
            keepRight ? tbc::BytOpcode::OP_NIP : tbc::BytOpcode::OP_DROP
        );

        auto top = m_scopePtr->pop();
        auto below = m_scopePtr->pop();
        if (!top.has_value() || !below.has_value()) {
            SourceLocation loc = getNodeLocation(node);
            SEMANTIC_ERROR(
                "Slice symbolic stack underflow",
                loc,
                "Internal compiler error while applying OP_SPLIT"
            );
            throw std::runtime_error("Slice symbolic stack underflow");
        }

        CompilerPlaceholder result;
        m_scopePtr->push(result.toString(), "bytes", result.toString());
    };

    if (startIsBeginning) {
        // [..., object, length] -> [..., prefix]
        prepareObjectAnd(length);
        splitToResult(false);
        return;
    }

    if (lengthIsToEnd) {
        // [..., object, start] -> [..., suffix]
        prepareObjectAnd(start);
        splitToResult(true);
        return;
    }

    if (length.kind == SliceOperandKind::StackValue) {
        // Preserve an already-materialized length below the first split:
        // [..., length, object, start] -> [..., length, suffix].
        if (!stackBlockIsReady(
                {&length.element, &objectElement, &start.element}
            )) {
            materialize(length);
            materializeObject();
            materialize(start);
        }
        splitToResult(true);

        auto lengthPos = m_scopePtr->getPos(length.element.getName());
        if (!lengthPos.has_value() || lengthPos.value() != 1) {
            SourceLocation loc = getNodeLocation(node);
            SEMANTIC_ERROR(
                "Slice length was not preserved below the suffix",
                loc,
                "Internal compiler error while preparing the second OP_SPLIT"
            );
            throw std::runtime_error(
                "Slice length was not preserved below the suffix"
            );
        }
        emitRoll(lengthPos.value());
        m_scopePtr->roll(lengthPos.value());
    } else {
        // Immediate/inline length does not occupy a slot yet, so emit it only
        // after the first split has produced the suffix.
        prepareObjectAnd(start);
        splitToResult(true);
        materialize(length);
    }

    // [..., suffix, length] -> [..., requested window]
    splitToResult(false);
}

std::vector<tbc::StackElement> ASTToBytecodeVisitor::processArguments(
    const std::vector<std::unique_ptr<ExprNode>>& args,
    int expectedArgCount,
    const std::string& functionName
)
{
    DEFER([]() { LOG_DEBUG("processArguments function call over"); });
    LOG_DEBUG(
        "Processing " + std::to_string(expectedArgCount) +
        " expected arguments for function: " + functionName
    );

    if (static_cast<int>(args.size()) != expectedArgCount) {
        SourceLocation loc("", 0, 0); // TODO: 从调用点传位置信息.
        std::ostringstream warningStream;
        warningStream << "argument count mismatch for function '"
                      << functionName << "': expected " << expectedArgCount
                      << ", got " << args.size();
        COMPILER_WARNING(warningStream.str(), loc);
        LOG_WARNING(warningStream.str());
    }

    std::vector<tbc::StackElement> processedArgs;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& argElement = args[i];
        LOG_DEBUG(
            "Processing argument " + std::to_string(i + 1) + "/" +
            std::to_string(expectedArgCount) +
            " (array index: " + std::to_string(i) + ")"
        );

        visitExpr(*argElement);
        if (m_scopePtr->empty()) {
            std::ostringstream errorStream;
            errorStream << "argument " << (i + 1) << " for function '"
                        << functionName
                        << "' did not produce a compiler stack value";
            SourceLocation loc(
                "", argElement->pos.first, argElement->pos.second
            );
            SEMANTIC_ERROR(
                errorStream.str(),
                loc,
                "Check that the argument expression produces a value"
            );
            LOG_ERROR(errorStream.str());
            throw std::runtime_error(errorStream.str());
        }
        auto topElementName = m_scopePtr->top().getName();
        if (CompilerPlaceholder::isPlaceholder(topElementName)) {
            processedArgs.push_back(m_scopePtr->top());
        } else {
            auto argResultOpt = m_scopePtr->pop();
            if (!argResultOpt.has_value()) {
                std::ostringstream errorStream;
                errorStream << "Failed to process parameter " << (i + 1) << "/"
                            << args.size() << " for function '" << functionName
                            << "' at line " << argElement->pos.first
                            << ", column " << argElement->pos.second;
                SourceLocation loc(
                    "", argElement->pos.first, argElement->pos.second
                );
                SEMANTIC_ERROR(
                    errorStream.str(),
                    loc,
                    "Check the function argument expression"
                );
                LOG_CRITICAL(errorStream.str());
                throw std::runtime_error(errorStream.str());
            }
            processedArgs.push_back(argResultOpt.value());
        }
    }
    return processedArgs;
}

void ASTToBytecodeVisitor::adjustStackToMatch(
    const std::vector<StackElement>& elementsVec
)
{
    if (elementsVec.empty()) {
        return;
    }

    int n = static_cast<int>(elementsVec.size());

    // The planner is deliberately limited to already-materialized, distinct
    // main-stack slots.  Script arguments, aliases and oversized windows retain
    // the legacy path below, including its existing diagnostics.
    const auto layout = analyzeArgumentLayout(*m_scopePtr, elementsVec);
    if (layout.has_value() && layout->strictMovePlan.has_value()) {
        emitAndApplyMoveOnlyPlan(
            m_generator, *m_scopePtr, *layout->strictMovePlan
        );
        for (int consumed = 0; consumed < n; ++consumed) {
            m_scopePtr->pop();
        }
        LOG_DEBUG(
            "Shortest argument-layout plan selected: ",
            layout->legacyBytes,
            " -> ",
            layout->strictMovePlan->serializedBytes,
            " bytes"
        );
        return;
    }

    // 期望位置: 参数 i 应在 pos=n-1-i.
    bool allInCorrectPosition = true;

    for (int i = 0; i < n; i++) {
        auto element = elementsVec[i];
        auto elementStr = element.getName();

        if (isScript(elementStr)) {
            allInCorrectPosition = false;
            break;
        }

        auto elementPosOpt = m_scopePtr->getPos(element);
        if (!elementPosOpt.has_value()) {
            allInCorrectPosition = false;
            break;
        }

        int expectedPos = n - 1 - i;
        if (elementPosOpt.value() != expectedPos) {
            allInCorrectPosition = false;
            break;
        }
    }

    if (allInCorrectPosition) {
        LOG_DEBUG("All arguments are already in correct positions, removing "
                  "elements from stack");
        for (int i = 0; i < n; i++) {
            auto element = elementsVec[i];
            auto elementPosOpt = m_scopePtr->getPos(element);
            m_scopePtr->roll(elementPosOpt.value());
            m_scopePtr->pop();
        }
        return;
    }

    int i = 0;
    while (i < n) {
        auto element = elementsVec[i];
        auto elementStr = element.getName();
        LOG_DEBUG("Processing argument ", i, " with content:", elementStr);

        if (isScript(elementStr)) {
            m_generator.emit(elementStr);
            i++;
            continue;
        }

        auto elementPosOpt = m_scopePtr->getPos(element);
        if (!elementPosOpt.has_value()) {
            std::ostringstream errorStream;
            errorStream
                << "Unable to find element '" << elementStr
                << "' on stack during function call argument processing";
            SourceLocation loc("", 0, 0);
            SEMANTIC_ERROR(
                errorStream.str(),
                loc,
                "Check if the variable is properly defined"
            );
            LOG_ERROR(errorStream.str());
            throw std::runtime_error(errorStream.str());
        }
        auto vmElementPos = elementPosOpt.value() + i;
        if (i != n - 1) {
            auto nextElement = elementsVec[i + 1];
            auto nextElementStr = nextElement.getName();
            if (!isScript(nextElementStr)) {
                auto nextElementPosOpt = m_scopePtr->getPos(nextElement);
                if (nextElementPosOpt.has_value()) {
                    auto nextvmElementPos = nextElementPosOpt.value() + i + 1;
                    if (vmElementPos == 3 && nextvmElementPos == 2) {
                        m_generator.emit(tbc::BytOpcode::OP_2SWAP);
                        m_scopePtr->roll(elementPosOpt.value());
                        m_scopePtr->pop();
                        m_scopePtr->roll(nextElementPosOpt.value());
                        m_scopePtr->pop();
                        i += 2;
                        continue;
                    } else if (vmElementPos == 5 && nextvmElementPos == 4) {
                        m_generator.emit(tbc::BytOpcode::OP_2ROT);
                        m_scopePtr->roll(elementPosOpt.value());
                        m_scopePtr->pop();
                        m_scopePtr->roll(nextElementPosOpt.value());
                        m_scopePtr->pop();
                        i += 2;
                        continue;
                    }
                }
            }
        }
        m_scopePtr->roll(elementPosOpt.value());
        m_scopePtr->pop();

        if (STACK_TOP_POS != vmElementPos) {
            if (1 == vmElementPos) {
                m_generator.emit(tbc::BytOpcode::OP_SWAP);
            } else if (2 == vmElementPos) {
                m_generator.emit(tbc::BytOpcode::OP_ROT);
            } else {
                std::string pushDataOpcode = numberToScriptHex(vmElementPos);
                m_generator.emit(pushDataOpcode);
                m_generator.emit(tbc::BytOpcode::OP_ROLL);
            }
        }
        i++;
    }
}

void ASTToBytecodeVisitor::processArgsToTop(
    std::vector<tbc::StackElement>& elementsVec,
    const std::vector<ParameterInfo>& paramInfos
)
{
    if (elementsVec.empty()) {
        return;
    }

    if (elementsVec.size() != paramInfos.size()) {
        std::ostringstream oss;
        oss << "Private function argument metadata mismatch: received "
            << elementsVec.size() << " values for " << paramInfos.size()
            << " parameters";
        SourceLocation loc("", 0, 0);
        SEMANTIC_ERROR(
            oss.str(), loc, "Check the private function call argument count"
        );
        throw std::runtime_error(oss.str());
    }

    LOG_DEBUG(
        "processArgsToTop: processing " + std::to_string(elementsVec.size()) +
        " arguments"
    );

    // 提前检测: 所有原子元素 (含结构体字段) 已在期望位置则零移动直接返回.
    // 期望: 栈顶起 arg[n-1] 字段 (声明逆序) -> arg[n-2] -> ... -> arg[0].
    {
        bool canCheckAll = true;

        // 原子元素名 -> 期望 position; 逆序累加.
        std::vector<std::pair<std::string, int>> expectedPositions;
        int pos = 0;

        for (int i = static_cast<int>(elementsVec.size()) - 1;
             i >= 0 && canCheckAll;
             --i) {
            auto elem = elementsVec[i];
            const std::string& paramType = (i <
                                            static_cast<int>(paramInfos.size()))
                                               ? paramInfos[i].type
                                               : "";

            // Fixed arrays are represented by element slots (and struct
            // arrays by flattened leaves), never by the root descriptor.
            if (apc::util::parseFixedArrayType(paramType).has_value()) {
                canCheckAll = false;
                break;
            }

            // 占位符无法按名字查位置, 跳过优化.
            if (CompilerPlaceholder::isPlaceholder(elem.getName())) {
                canCheckAll = false;
                break;
            }

            bool isStruct = !paramType.empty() &&
                            m_structDefinitions.find(paramType) !=
                                m_structDefinitions.end();

            if (isStruct) {
                auto fields = getStructFieldsExpanded(
                    paramType, elem.getName(), m_structDefinitions
                );
                // 各字段按声明顺序 roll 后, 末字段停在最小 position.
                for (int fi = static_cast<int>(fields.size()) - 1; fi >= 0;
                     --fi) {
                    expectedPositions.emplace_back(fields[fi].first, pos++);
                }
            } else {
                expectedPositions.emplace_back(elem.getName(), pos++);
            }
        }

        if (canCheckAll) {
            bool allInCorrectPos = true;
            for (const auto& [elemName, expectedPos] : expectedPositions) {
                auto posOpt = m_scopePtr->getPos(elemName);
                if (!posOpt.has_value() || posOpt.value() != expectedPos) {
                    allInCorrectPos = false;
                    break;
                }
            }

            if (allInCorrectPos) {
                LOG_DEBUG("processArgsToTop: all arguments already in correct "
                          "positions, no moves needed");
                return;
            }
        }
    }
    for (size_t i = 0; i < elementsVec.size(); ++i) {
        const auto& element = elementsVec[i];
        const std::string& elementStr = element.getName();
        const auto& paramInfo = (i < paramInfos.size()) ? paramInfos[i]
                                                        : ParameterInfo("", "");
        const std::string& paramType = paramInfo.type;
        const auto arrayType = apc::util::parseFixedArrayType(paramType);

        LOG_DEBUG(
            "processArgsToTop: processing argument " + std::to_string(i) +
            " with name: " + elementStr + ", type: " + paramType
        );

        bool isStructType = !paramType.empty() &&
                            m_structDefinitions.find(paramType) !=
                                m_structDefinitions.end();

        if (arrayType.has_value()) {
            SymbolTable& symbolTable = m_scopePtr->getCurrentSymtab();
            std::string argumentRoot =
                symbolTable.resolveBindSymbol(element.getName());

            if (isWholeArrayElement(argumentRoot)) {
                splitWholeArrayElement(argumentRoot);
            }

            auto arrayInfo = m_scopePtr->getArrayInfo(argumentRoot);
            if (!arrayInfo.has_value()) {
                std::ostringstream oss;
                oss << "Unable to materialize fixed-array argument '"
                    << argumentRoot << "' for parameter '" << paramInfo.name
                    << "'";
                SourceLocation loc("", 0, 0);
                SEMANTIC_ERROR(
                    oss.str(), loc,
                    "Pass a declared fixed array with matching element type"
                );
                throw std::runtime_error(oss.str());
            }
            if (arrayInfo->size != arrayType->size ||
                arrayInfo->elementType != arrayType->elementType) {
                std::ostringstream oss;
                oss << "Fixed-array argument '" << argumentRoot
                    << "' has type '" << arrayInfo->elementType << "["
                    << arrayInfo->size << "]', expected '" << paramType
                    << "'";
                SourceLocation loc("", 0, 0);
                SEMANTIC_ERROR(
                    oss.str(), loc,
                    "Pass an array with the parameter's exact shape"
                );
                throw std::runtime_error(oss.str());
            }

            elementsVec[i] = StackElement(
                argumentRoot, paramType, argumentRoot
            );
            std::vector<StackElement> arrayAtoms;
            const bool isStructArray =
                m_structDefinitions.find(arrayInfo->elementType) !=
                m_structDefinitions.end();
            for (const auto& arrayElement : arrayInfo->elements) {
                if (!isStructArray) {
                    arrayAtoms.emplace_back(
                        arrayElement.qualifiedName,
                        arrayInfo->elementType,
                        arrayElement.qualifiedName
                    );
                    continue;
                }

                const auto fields = getStructFieldsExpanded(
                    arrayInfo->elementType,
                    arrayElement.qualifiedName,
                    m_structDefinitions
                );
                if (fields.empty()) {
                    std::ostringstream oss;
                    oss << "Struct-array argument element '"
                        << arrayElement.qualifiedName << "' of type '"
                        << arrayInfo->elementType
                        << "' has no runtime fields";
                    SourceLocation loc("", 0, 0);
                    SEMANTIC_ERROR(
                        oss.str(), loc, "Check the struct definition"
                    );
                    throw std::runtime_error(oss.str());
                }
                for (const auto& [fieldPath, fieldType] : fields) {
                    arrayAtoms.emplace_back(
                        fieldPath, fieldType, fieldPath
                    );
                }
            }

            // Preflight the complete array before mutating stack order.
            for (const auto& atom : arrayAtoms) {
                const std::string& atomName = atom.getName();
                if (m_scopePtr->getPos(atomName).has_value() ||
                    m_scopePtr->getPos(atomName, true).has_value()) {
                    continue;
                }
                std::string fixedName = atomName;
                if (!m_scopePtr->getFixed(fixedName).has_value()) {
                    std::ostringstream oss;
                    oss << "Unable to find argument slot '" << atomName
                        << "' on the main or alternate stack";
                    SourceLocation loc("", 0, 0);
                    SEMANTIC_ERROR(
                        oss.str(), loc,
                        "Ensure the value has not already been consumed"
                    );
                    throw std::runtime_error(oss.str());
                }
            }

            for (const auto& atom : arrayAtoms) {
                const std::string& atomName = atom.getName();
                auto mainPos = m_scopePtr->getPos(atomName);
                if (!mainPos.has_value()) {
                    if (!moveAltElementToMain(atomName)) {
                        std::string fixedName = atomName;
                        auto fixed = m_scopePtr->getFixed(fixedName);
                        if (!fixed.has_value()) {
                            throw std::runtime_error(
                                "Failed to materialize private array "
                                "argument: " + atomName
                            );
                        }
                        const std::string& encoded =
                            fixed->getData().empty() ? fixed->getName()
                                                     : fixed->getData();
                        m_generator.emit(encoded);
                        m_scopePtr->removeFixed(fixedName);
                        m_scopePtr->push(
                            atomName,
                            fixed->getType().empty() ? atom.getType()
                                                     : fixed->getType(),
                            fixed->getData()
                        );
                    }
                    mainPos = m_scopePtr->getPos(atomName);
                }
                if (!mainPos.has_value()) {
                    throw std::runtime_error(
                        "Private array argument disappeared during stack "
                        "preparation: " + atomName
                    );
                }
                if (mainPos.value() != STACK_TOP_POS) {
                    emitRoll(mainPos.value());
                    m_scopePtr->roll(mainPos.value());
                }
            }
        } else if (isStructType) {
            LOG_DEBUG("Argument is a struct type: " + paramType);
            auto fieldPathsAndTypes = getStructFieldsExpanded(
                paramType, elementStr, m_structDefinitions
            );

            LOG_DEBUG(
                "Struct has " + std::to_string(fieldPathsAndTypes.size()) +
                " expanded fields"
            );

            // 字段倒序 roll: 最后声明的字段在栈顶.
            for (auto it = fieldPathsAndTypes.rbegin();
                 it != fieldPathsAndTypes.rend();
                 ++it) {
                const std::string& fieldPath = it->first;
                const std::string& fieldType = it->second;

                LOG_DEBUG(
                    "Moving struct field: " + fieldPath +
                    " of type: " + fieldType
                );

                auto fieldPosOpt = m_scopePtr->getPos(fieldPath);
                if (!fieldPosOpt.has_value()) {
                    LOG_WARNING(
                        "Struct field not found on stack: " + fieldPath
                    );
                    continue;
                }

                int position = fieldPosOpt.value();
                LOG_DEBUG(
                    "Found field at position: " + std::to_string(position)
                );

                if (position == STACK_TOP_POS) {
                    LOG_DEBUG("Field already at top");
                } else {
                    emitRoll(position);
                    m_scopePtr->roll(position);
                }

                LOG_DEBUG("Moved struct field to stack top: " + fieldPath);
            }
        } else {
            LOG_DEBUG("Argument is a basic type");

            // 合约成员变量 <self.X> (builtin_member) 作为实参时, 不预 emit、
            // 也不占栈位. 形参体内每次访问由 visitIdentifier 单独 push 名,
            // 再由 OpNode lamd 的 isScript 分支 emit, 支持 x * x 这类多次读.
            // CompilerPlaceholder (Push/.Clone()/表达式中间结果) 是真实栈
            // 元素, 走普通参数路径 (roll 到栈顶, 受 move 语义).
            bool isBuiltinMemberOpcode =
                elementStr.size() >= 2 && elementStr.front() == '<' &&
                elementStr.back() == '>';
            if (isBuiltinMemberOpcode) {
                LOG_DEBUG(
                    "Builtin member arg '" + elementStr +
                    "' deferred to per-access emit"
                );
                continue;
            }

            // Literal/immediate arguments and raw builtin members are returned
            // by processArguments() as inline script atoms after their virtual
            // stack entry has been consumed. Materialize one real call slot
            // and publish its compiler identity to privateFunctionResolution.
            if (element.getType() == "builtin_member" ||
                isScript(elementStr)) {
                m_generator.emit(elementStr);
                CompilerPlaceholder materialized;
                StackElement runtimeElement(
                    materialized.toString(),
                    element.getType(),
                    materialized.toString()
                );
                m_scopePtr->push(runtimeElement);
                elementsVec[i] = runtimeElement;
                continue;
            }

            auto mutableElement = element;
            auto elementPosOpt = m_scopePtr->getPos(mutableElement);
            if (!elementPosOpt.has_value()) {
                std::ostringstream errorStream;
                errorStream << "Unable to find element '" << elementStr
                            << "' on stack during argument processing";
                SourceLocation loc("", 0, 0);
                SEMANTIC_ERROR(
                    errorStream.str(),
                    loc,
                    "Check if the variable is properly defined"
                );
                LOG_ERROR(errorStream.str());
                throw std::runtime_error(errorStream.str());
            }

            int position = elementPosOpt.value();
            LOG_DEBUG(
                "Found argument at position: " + std::to_string(position)
            );

            if (position == STACK_TOP_POS) {
                LOG_DEBUG("Argument already at top, no operation needed");
            } else {
                emitRoll(position);
                m_scopePtr->roll(position);
            }

            LOG_DEBUG("Moved argument to stack top: " + elementStr);
        }
    }

    LOG_DEBUG("processArgsToTop: completed processing all arguments");
}

// 沿 fieldPath (如 "paramName.field1.field2.metadata") 在结构体定义中
// 寻找复合类型字段; 找到返回 StructFieldType, 否则 nullptr.
static const StructFieldType* findCompoundTypeFieldInPath(
    const std::string& fieldPath,
    const std::string& paramType,
    const std::string& paramName,
    const std::map<
        std::string,
        std::vector<std::pair<std::string, StructFieldType>>>& structDefinitions
)
{
    // 去参数名前缀: "block.header.metadata" -> "header.metadata".
    if (fieldPath.find(paramName + ".") != 0) {
        return nullptr;
    }
    std::string relativePath = fieldPath.substr(paramName.length() + 1);

    // "header.metadata" -> ["header", "metadata"].
    std::vector<std::string> fieldNames;
    size_t start = 0;
    while (start < relativePath.length()) {
        size_t dotPos = relativePath.find('.', start);
        if (dotPos == std::string::npos) {
            fieldNames.push_back(relativePath.substr(start));
            break;
        }
        fieldNames.push_back(relativePath.substr(start, dotPos - start));
        start = dotPos + 1;
    }

    if (fieldNames.empty()) {
        return nullptr;
    }

    std::string currentStructType = paramType;
    const StructFieldType* result = nullptr;

    for (size_t i = 0; i < fieldNames.size(); ++i) {
        const std::string& fieldName = fieldNames[i];
        bool isLastField = (i == fieldNames.size() - 1);

        auto structIt = structDefinitions.find(currentStructType);
        if (structIt == structDefinitions.end()) {
            return nullptr;
        }

        bool found = false;
        for (const auto& fieldDef : structIt->second) {
            if (fieldDef.first == fieldName) {
                found = true;

                if (isLastField) {
                    if (fieldDef.second.isCompoundType) {
                        result = &fieldDef.second;
                    }
                    return result;
                } else {
                    // 中间字段必须是结构体类型, 否则路径错误.
                    const std::string& fieldType = fieldDef.second.baseType;
                    auto nextStructIt = structDefinitions.find(fieldType);
                    if (nextStructIt != structDefinitions.end()) {
                        currentStructType = fieldType;
                        break;
                    } else {
                        return nullptr;
                    }
                }
            }
        }

        if (!found) {
            return nullptr;
        }
    }

    return result;
}

void ASTToBytecodeVisitor::registerExpandedParameterField(
    const std::string& fieldPath,
    const std::string& fieldType,
    const std::string& sourceStructType,
    const std::string& sourcePrefix,
    const std::map<
        std::string,
        std::vector<std::pair<std::string, StructFieldType>>>& structDefinitions
)
{
    if (fieldType == "__compound__") {
        LOG_DEBUG(
            "Struct field '",
            fieldPath,
            "' is a compound type, treating as whole"
        );

        const StructFieldType* compoundFieldDef =
            findCompoundTypeFieldInPath(
                fieldPath,
                sourceStructType,
                sourcePrefix,
                structDefinitions
            );

        if (compoundFieldDef != nullptr) {
            const auto& compoundFields = compoundFieldDef->compoundFields;
            CompoundTypeInfo compoundInfo(fieldPath, compoundFields, true);

            if (!m_scopePtr->defineCompoundType(compoundInfo)) {
                LOG_WARNING(
                    "Failed to define compound type for field: ",
                    fieldPath
                );
            }

            LOG_DEBUG(
                "Registered compound type field: ",
                fieldPath,
                " with ",
                compoundFields.size(),
                " fields"
            );
        } else {
            LOG_WARNING(
                "Could not find compound type field definition for: ",
                fieldPath
            );
        }
    } else if (auto arrayType = apc::util::parseFixedArrayType(fieldType)) {
        size_t elementByteSize = 8;
        size_t arraySize = arrayType->size;
        registerWholeArrayElement(fieldPath, arraySize, elementByteSize);
    }

    m_scopePtr->defineSymbol(fieldPath);
    m_scopePtr->push(fieldPath, fieldType, fieldPath);
    m_generator.emitUnlock("<" + fieldPath + ">");
}

void ASTToBytecodeVisitor::expandStructParameter(
    const std::string& paramName,
    const std::string& paramType,
    const std::map<
        std::string,
        std::vector<std::pair<std::string, StructFieldType>>>& structDefinitions
)
{
    LOG_DEBUG(
        "Expanding struct parameter: ", paramName, " of type: ", paramType
    );

    // 登记根名 (结构体类型); 扁平字段随后单独登记.
    m_scopePtr->defineSymbol(paramName, paramType);

    std::vector<std::pair<std::string, std::string>> fieldPathsAndTypes =
        getStructFieldsExpanded(paramType, paramName, structDefinitions);

    // 按声明顺序逐字段 unlock; 除 uint64 数组外, 其他基础类型数组在
    // getStructFieldsExpanded 中已展开为独立元素.
    for (const auto& fieldInfo : fieldPathsAndTypes) {
        const std::string& fieldPath = fieldInfo.first;
        const std::string& fieldType = fieldInfo.second;
        LOG_DEBUG(
            "Defining struct field: ", fieldPath, " of type: ", fieldType
        );

        registerExpandedParameterField(
            fieldPath, fieldType, paramType, paramName, structDefinitions
        );
    }

    LOG_DEBUG(
        "Expanded struct parameter '",
        paramName,
        "' into ",
        fieldPathsAndTypes.size(),
        " fields in declaration order"
    );
}

void ASTToBytecodeVisitor::expandArrayParameter(
    const std::string& paramName,
    const std::string& paramType,
    const std::
        map<std::string, std::vector<std::pair<std::string, StructFieldType>>>&
            structDefinitions,
    const SourceLocation& loc
)
{
    LOG_DEBUG(
        "Expanding array parameter: ", paramName, " of type: ", paramType
    );

    auto arrayType = apc::util::parseFixedArrayType(paramType);
    if (!arrayType) {
        std::ostringstream errorStream;
        errorStream << "Invalid array type format: " << paramType;
        SEMANTIC_ERROR(
            errorStream.str(), loc, "Array type must be in format 'type[size]'"
        );
        LOG_ERROR(errorStream.str());
        throw std::runtime_error(errorStream.str());
    }

    const std::string& baseType = arrayType->elementType;
    size_t arraySize = arrayType->size;

    if (structDefinitions.find(baseType) != structDefinitions.end()) {
        // 结构体数组: 展开每个元素的字段.
        LOG_DEBUG(
            "Parameter '",
            paramName,
            "' is a struct array type '",
            paramType,
            "', base type: '",
            baseType,
            "', size: ",
            std::to_string(arraySize)
        );

        // 仅登记数组元数据; 元素栈槽由下方字段展开时 push 建立.
        m_scopePtr->defineArray(paramName, baseType, arraySize, true);

        for (size_t i = 0; i < arraySize; ++i) {
            std::string indexedPrefix = paramName + "[" + numberToScriptHex(i) +
                                        "]";
            auto nestedFields = getStructFieldsExpanded(
                baseType, indexedPrefix, structDefinitions
            );

            for (const auto& fieldPair : nestedFields) {
                const std::string& fieldPath = fieldPair.first;
                const std::string& fieldType = fieldPair.second;

                registerExpandedParameterField(
                    fieldPath,
                    fieldType,
                    baseType,
                    indexedPrefix,
                    structDefinitions
                );
            }
        }

        LOG_DEBUG(
            "Expanded struct array parameter '",
            paramName,
            "' into ",
            arraySize,
            " struct elements with all fields"
        );
    }
    // uint64 数组: 整体处理.
    else if (baseType == "uint64") {
        size_t elementByteSize = 8;

        registerWholeArrayElement(paramName, arraySize, elementByteSize);

        LOG_DEBUG(
            "Parameter '",
            paramName,
            "' is a uint64[] type with size ",
            std::to_string(arraySize),
            ", registered as whole array element"
        );

        m_scopePtr->defineSymbol(paramName);
        m_scopePtr->push(paramName, paramType, paramName);
        m_generator.emitUnlock("<" + paramName + ">");
    } else {
        // 其他基础类型数组: 展开为独立元素.
        LOG_DEBUG(
            "Parameter '",
            paramName,
            "' is a basic array type '",
            paramType,
            "', expanding to ",
            std::to_string(arraySize),
            " elements"
        );

        // 仅登记元数据; 循环为每个元素 defineSymbol+push.
        m_scopePtr->defineArray(paramName, baseType, arraySize, true);

        for (size_t i = 0; i < arraySize; ++i) {
            std::string indexedField = paramName + "[" + numberToScriptHex(i) +
                                       "]";
            m_scopePtr->defineSymbol(indexedField);
            m_scopePtr->push(indexedField, baseType, indexedField);
            m_generator.emitUnlock("<" + indexedField + ">");
        }

        LOG_DEBUG(
            "Expanded array parameter '",
            paramName,
            "' into ",
            arraySize,
            " elements of type '",
            baseType,
            "'"
        );
    }
}

// 递归展开结构体字段为完整路径列表, 按声明顺序.
// 例: Person{name, age:Sub{a, b:TxID{a, b}, c}, address} ->
//   [p.name, p.age.a, p.age.b.a, p.age.b.b, p.age.c, p.address].
std::vector<std::pair<std::string, std::string>>
ASTToBytecodeVisitor::getStructFieldsExpanded(
    const std::string& structName,
    const std::string& prefix,
    const std::map<
        std::string,
        std::vector<std::pair<std::string, StructFieldType>>>& structDefinitions
)
{
    std::vector<std::pair<std::string, std::string>> result;

    auto it = structDefinitions.find(structName);
    if (it == structDefinitions.end()) {
        LOG_ERROR("Struct '", structName, "' not found in definitions");
        return result;
    }

    const auto& fields = it->second;

    for (const auto& field : fields) {
        const std::string& fieldName = field.first;
        const StructFieldType& fieldTypeInfo = field.second;

        std::string fieldPath = prefix + "." + fieldName;
        const std::string& fieldType = fieldTypeInfo.getTypeString();

        if (fieldTypeInfo.isCompoundType) {
            LOG_DEBUG(
                "Field '", fieldPath, "' is a compound type, treating as whole"
            );
            result.push_back(std::make_pair(fieldPath, "__compound__"));
            continue;
        }

        if (fieldTypeInfo.isArray) {
            const std::string& baseType = fieldTypeInfo.baseType;
            size_t arraySize = fieldTypeInfo.arraySize;

            // 结构体数组: 注册元数据后按索引展开字段.
            if (structDefinitions.find(baseType) != structDefinitions.end()) {
                m_scopePtr->defineArray(fieldPath, baseType, arraySize, true);
                for (size_t i = 0; i < arraySize; ++i) {
                    auto indexedPrefix = fieldPath + "[" +
                                         numberToScriptHex(i) + "]";
                    auto nestedFields = getStructFieldsExpanded(
                        baseType, indexedPrefix, structDefinitions
                    );
                    result.insert(
                        result.end(), nestedFields.begin(), nestedFields.end()
                    );
                }
            } else if (baseType == "uint64") {
                // uint64 数组保持整体 (连续字节块).
                result.push_back(std::make_pair(fieldPath, fieldType));
            } else {
                m_scopePtr->defineArray(fieldPath, baseType, arraySize, true);
                for (size_t i = 0; i < arraySize; ++i) {
                    auto indexedField = fieldPath + "[" + numberToScriptHex(i) +
                                        "]";
                    result.push_back(std::make_pair(indexedField, baseType));
                }
            }
            continue;
        }

        const std::string& baseType = fieldTypeInfo.baseType;
        if (structDefinitions.find(baseType) != structDefinitions.end()) {
            auto nestedFields =
                getStructFieldsExpanded(baseType, fieldPath, structDefinitions);
            result
                .insert(result.end(), nestedFields.begin(), nestedFields.end());
        } else {
            result.push_back(std::make_pair(fieldPath, fieldType));
        }
    }

    return result;
}

SourceLocation ASTToBytecodeVisitor::getNodeLocation(const ASTNode& node) const
{
    if (node.hasSourceLocation()) {
        return node.sourceLocation;
    }
    return SourceLocation("", node.pos.first, node.pos.second);
}

#ifdef ENABLE_DEBUGGER
apc_debug::SourceLocation ASTToBytecodeVisitor::extractDebugLocation(
    const ASTNode& node,
    const std::string& sourceFile
) const
{
    apc_debug::SourceLocation loc;
    if (node.hasSourceLocation()) {
        loc.filename = sourceFile.empty() ? node.sourceLocation.filename
                                          : sourceFile;
        loc.line = node.sourceLocation.line;
        loc.column = node.sourceLocation.column;
        loc.endLine = node.sourceLocation.line;
        loc.endColumn = node.sourceLocation.column;
        return loc;
    }

    loc.filename = sourceFile.empty() ? m_sourceFile : sourceFile;
    loc.line = node.pos.first;
    loc.column = node.pos.second;
    return loc;
}

void ASTToBytecodeVisitor::setCurrentLocationForGenerator(const ASTNode& node)
{
    // 即便无 DebugInfoGenerator 也更新, 让日志/调试器可依赖
    // BytecodeGenerator::m_currentLocation.
    apc_debug::SourceLocation loc = extractDebugLocation(node);
    m_generator.setCurrentLocation(loc);
}
#endif

void ASTToBytecodeVisitor::validateRebinding(
    const std::string& varName,
    const AssignNode& node
)
{
    LOG_DEBUG("Validating rebinding for variable: " + varName);

    // 检查右侧表达式是否包含对同一变量的引用
    // 这是重新绑定语义的核心：右侧必须"消耗"左侧变量的旧值
    bool containsVarReference =
        checkExpressionContainsVariable(*node.value, varName);

    if (!containsVarReference) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "Invalid rebinding: variable '" << varName
            << "' is being rebound without consuming its previous value. "
            << "In rebinding semantics, the right-hand side should "
               "reference "
               "the variable being rebound.";

        COMPILER_WARNING(oss.str(), loc);
        LOG_WARNING(oss.str());
    } else {
        LOG_DEBUG(
            "Valid rebinding: variable '" + varName +
            "' is consumed in right-hand side expression"
        );
    }
}

bool ASTToBytecodeVisitor::tryHandleStructBraceAssignment(
    const AssignNode& node,
    const std::string& leftVarName,
    const BraceExprNode& braceExpr
)
{
    std::string rootName = leftVarName;
    if (!m_scopePtr->symbolExists(rootName)) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "cannot assign brace initializer to undefined variable '"
            << leftVarName << "'";
        SEMANTIC_ERROR(
            oss.str(), loc, "Declare the variable before brace assignment"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    std::optional<std::string> declaredTypeOpt;
    const auto scopeSymbols =
        m_scopePtr->getCurrentSymtab().getCurrentScopeSymbols();
    for (auto it = scopeSymbols.rbegin(); it != scopeSymbols.rend(); ++it) {
        if (it->getSymbolName() == leftVarName) {
            declaredTypeOpt = it->m_stackElement.getType();
            break;
        }
    }

    if (!declaredTypeOpt.has_value() || declaredTypeOpt->empty()) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "variable '" << leftVarName
            << "' has no declared type in symbol table; cannot use brace "
               "initializer";
        SEMANTIC_ERROR(
            oss.str(), loc, "Ensure variable is declared with a type"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    const std::string& declaredType = *declaredTypeOpt;

    // brace 初始化按扁平字段顺序赋值, 仅 Struct 适用; 标量
    // (uint64/int/bool) 与 uint64[] 走其他路径, 在此拦截.
    bool isStructType = m_structDefinitions.find(declaredType) !=
                        m_structDefinitions.end();
    if (!isStructType) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "brace initializer is only valid for struct types; variable '"
            << leftVarName << "' has type '" << declaredType
            << "' which is not a defined Struct";
        SEMANTIC_ERROR(
            oss.str(), loc, "Use a variable declared with a Struct type name"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    // 扁平字段列表: 支持数组与嵌套结构体逐元素递归赋值.
    auto flatFields =
        getStructFieldsExpanded(declaredType, leftVarName, m_structDefinitions);
    if (flatFields.empty()) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "failed to flatten struct '" << declaredType
            << "' for brace assignment of '" << leftVarName << "'";
        SEMANTIC_ERROR(
            oss.str(), loc, "Check struct definition and nested field types"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    if (flatFields.size() != braceExpr.elements.size()) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "brace initializer for struct '" << declaredType << "' expects "
            << flatFields.size() << " value(s) (flattened fields), got "
            << braceExpr.elements.size();
        SEMANTIC_ERROR(
            oss.str(),
            loc,
            "Match brace element count to flattened struct fields"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    for (const auto& f : flatFields) {
        std::string fieldPath = f.first;

        if (!m_scopePtr->symbolExists(fieldPath)) {
            SourceLocation loc = getNodeLocation(node);
            std::ostringstream oss;
            oss << "struct field '" << fieldPath
                << "' is not defined; cannot assign brace initializer to '"
                << leftVarName << "'";
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Declare the struct variable or ensure all fields exist"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }
    }

    // 重新绑定语义检查 (与普通变量/数组定义赋值一致).
    if (m_scopePtr->isSymbolInitialized(leftVarName)) {
        validateRebinding(leftVarName, node);
        LOG_DEBUG("Rebinding detected for struct: " + leftVarName);
    } else {
        m_scopePtr->markSymbolInitialized(leftVarName);
        LOG_DEBUG("First binding for struct: " + leftVarName);
    }

    LOG_DEBUG("Detected struct brace assignment for variable: " + leftVarName);

    visitExpr(*node.value);

    std::vector<tbc::StackElement> rhsValues;
    rhsValues.reserve(flatFields.size());
    for (size_t k = 0; k < flatFields.size(); ++k) {
        auto rhsOpt = m_scopePtr->pop();
        if (!rhsOpt.has_value()) {
            SourceLocation loc = getNodeLocation(node);
            std::ostringstream oss;
            oss << "not enough elements in brace expression to assign to "
                   "struct fields";
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Ensure brace values count matches struct fields"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }
        rhsValues.push_back(rhsOpt.value());
    }

    // 按声明顺序赋值; rhsValues[0] 为原栈顶 (最后一个 brace 元素),
    // 故反向索引.
    for (size_t i = 0; i < flatFields.size(); ++i) {
        const std::string fieldPath = flatFields[i].first;
        const std::string expectedType = flatFields[i].second;
        const auto& rhsVal = rhsValues[flatFields.size() - 1 - i];
        applyLeafFieldAssignment(fieldPath, expectedType, rhsVal, node);
    }

    LOG_DEBUG("Struct brace assignment completed for: " + leftVarName);
    return true;
}

// 与普通变量赋值路径对齐 (常量/占位符/变量引用); 复合字段
// (__compound__) 作为整体单槽 blob 走同一路径.
void ASTToBytecodeVisitor::applyLeafFieldAssignment(
    const std::string& fieldPath,
    const std::string& expectedType,
    const tbc::StackElement& rhsVal,
    [[maybe_unused]] const ASTNode& locNode
)
{
    const std::string rhsName = rhsVal.getName();
    const std::string& rhsType = rhsVal.getType();
    const std::string& rhsData = rhsVal.getData();

    const bool rhsIsScript = isScript(rhsName);
    const bool rhsIsPlaceholder = CompilerPlaceholder::isPlaceholder(rhsName);
    const bool isCompoundField = (expectedType == "__compound__");

    if (isCompoundField) {
        LOG_DEBUG(
            "applyLeafFieldAssignment: compound-type field '" + fieldPath +
            "' assigned as a whole-slot value from '" + rhsName + "'."
        );
    }

    auto fieldPosOpt = m_scopePtr->getPos(fieldPath);

    std::optional<int64_t> rhsPosOpt;
    if (!rhsIsScript && !rhsIsPlaceholder) {
        rhsPosOpt = m_scopePtr->getPos(rhsName);
    }

    // getFixed / removeFixed 需非 const std::string&.
    std::string fieldPathMut = fieldPath;
    std::string rhsNameMut = rhsName;

    if (rhsIsScript) {
        if (fieldPosOpt.has_value()) {
            int64_t pos = fieldPosOpt.value();
            if (pos == STACK_TOP_POS) {
                m_generator.emit(tbc::BytOpcode::OP_DROP);
                m_scopePtr->pop();
            } else if (pos == 1) {
                m_scopePtr->dropAt(1);
                m_generator.emit(tbc::BytOpcode::OP_NIP);
            } else {
                emitRoll(pos);
                m_scopePtr->roll(pos);
                m_generator.emit(tbc::BytOpcode::OP_DROP);
                m_scopePtr->pop();
            }
            LOG_DEBUG("Removed old stack slot for field: " + fieldPath);
        }
        m_scopePtr->setFixed(tbc::StackElement(fieldPath, rhsType, rhsData));
        LOG_DEBUG(
            "Field '" + fieldPath +
            "' stored as constant in fixed area: " + rhsData
        );

    } else if (rhsIsPlaceholder) {
        if (fieldPosOpt.has_value()) {
            // 占位符已在栈顶, 旧字段深度 +1.
            int64_t vmPos = fieldPosOpt.value() + 1;
            if (vmPos == STACK_TOP_POS) {
                m_generator.emit(tbc::BytOpcode::OP_DROP);
                m_scopePtr->pop();
            } else if (vmPos == 1) {
                m_scopePtr->dropAt(1);
                m_generator.emit(tbc::BytOpcode::OP_NIP);
            } else {
                emitRoll(vmPos);
                m_scopePtr->roll(vmPos);
                m_generator.emit(tbc::BytOpcode::OP_DROP);
                m_scopePtr->pop();
            }
            LOG_DEBUG("Removed old stack slot for field: " + fieldPath);
        }
        m_scopePtr->removeFixed(fieldPathMut);
        m_scopePtr->push(fieldPath, rhsType, rhsData);
        LOG_DEBUG(
            "Field '" + fieldPath + "' claimed placeholder runtime slot."
        );

    } else {
        if (rhsPosOpt.has_value() && !fieldPosOpt.has_value()) {
            // 零成本重命名, 无 emit.
            m_scopePtr->removeFixed(fieldPathMut);
            m_scopePtr->renameAtPosition(
                static_cast<int>(rhsPosOpt.value()), fieldPath
            );
            LOG_DEBUG(
                "Field '" + fieldPath + "' zero-cost renamed from '" + rhsName +
                "'."
            );

        } else if (rhsPosOpt.has_value() && fieldPosOpt.has_value()) {
            // 栈到栈拷贝, 不消耗源变量.
            int64_t posA = rhsPosOpt.value();
            int64_t posB = fieldPosOpt.value();
            if (posB == posA) {
                LOG_DEBUG("Field '" + fieldPath + "' self-assignment (no-op).");
            } else {
#ifdef ENABLE_DEBUGGER
                setCurrentLocationForGenerator(locNode);
#endif
                const auto copyEmission = emitCopyAssignment(
                    m_generator,
                    static_cast<size_t>(posA),
                    static_cast<size_t>(posB)
                );
                if (copyEmission.usedPlanner) {
                    LOG_DEBUG(
                        "Shortest shallow field-copy plan selected: ",
                        copyEmission.legacyBytes,
                        " -> ",
                        copyEmission.emittedBytes,
                        " bytes"
                    );
                }
            }
            LOG_DEBUG(
                "Field '" + fieldPath + "' stack-to-stack copy from '" +
                rhsName + "'."
            );

        } else if (!rhsPosOpt.has_value() && fieldPosOpt.has_value()) {
            auto fixedRhsOpt = m_scopePtr->getFixed(rhsNameMut);
            if (fixedRhsOpt.has_value()) {
                m_generator.emit(fixedRhsOpt->getData());
                int64_t vmPos = fieldPosOpt.value() + 1;
                if (vmPos == STACK_TOP_POS) {
                    m_generator.emit(tbc::BytOpcode::OP_DROP);
                    m_scopePtr->pop();
                } else if (vmPos == 1) {
                    m_scopePtr->dropAt(1);
                    m_generator.emit(tbc::BytOpcode::OP_NIP);
                } else {
                    emitRoll(vmPos);
                    m_scopePtr->roll(vmPos);
                    m_generator.emit(tbc::BytOpcode::OP_DROP);
                    m_scopePtr->pop();
                }
                m_scopePtr->push(
                    fieldPath, fixedRhsOpt->getType(), fixedRhsOpt->getData()
                );
                LOG_DEBUG(
                    "Field '" + fieldPath + "' assigned from fixed-area var '" +
                    rhsName + "'."
                );
            } else {
                m_scopePtr->setFixed(
                    tbc::StackElement(fieldPath, rhsType, rhsData)
                );
                LOG_DEBUG(
                    "Field '" + fieldPath + "' fallback setFixed (rhsName='" +
                    rhsName + "')."
                );
            }

        } else {
            m_scopePtr->setFixed(tbc::StackElement(fieldPath, rhsType, rhsData)
            );
            LOG_DEBUG("Field '" + fieldPath + "' both not on stack, setFixed.");
        }
    }

    if (auto expectedArray = apc::util::parseFixedArrayType(expectedType);
        expectedArray && expectedArray->elementType == "uint64") {
        registerWholeArrayElement(fieldPath, expectedArray->size, 8);
    }

    m_scopePtr->markSymbolInitialized(fieldPath);
}

bool ASTToBytecodeVisitor::tryHandleArrayDefAssignment(
    const AssignNode& node,
    std::string leftVarName,
    ArrayDefNode& arrayDef
)
{
    // uint64[N] 是整体栈元素, 不可用 {a,b,c} 逐元素替换 (RHS 需完整
    // 字节串). 必须先于 isArraySymbol 判定: uint64[] 参数走 defineSymbol
    // 而非 defineArray, isArraySymbol 会返回 false.
    if (isWholeArrayElement(leftVarName)) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "whole array variable '" << leftVarName
            << "' (uint64[]) cannot be reassigned with an array literal "
               "'{...}'; assign element by element via '"
            << leftVarName << "[i] = ...' or replace the whole value with a "
            << "byte-string expression";
        SEMANTIC_ERROR(
            oss.str(),
            loc,
            "uint64[] is stored as a single whole stack element; array-def "
            "syntax expects per-element replacement which is not supported for "
            "this layout"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    // 未声明 / 已声明非数组都由 isArraySymbol 捕获, 文案按 symbolExists 区分.
    if (!m_scopePtr->isArraySymbol(leftVarName)) {
        const bool exists = m_scopePtr->symbolExists(leftVarName);
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "cannot assign array definition to "
            << (exists ? "non-array variable '" : "undefined variable '")
            << leftVarName << "'";
        SEMANTIC_ERROR(
            oss.str(),
            loc,
            exists ? "Left-hand side must be an array type"
                   : "Variable must be declared as array type first"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    auto arrayInfoOpt = m_scopePtr->getArrayInfo(leftVarName);
    if (!arrayInfoOpt.has_value()) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "failed to get array information for variable '" << leftVarName
            << "'";
        SEMANTIC_ERROR(oss.str(), loc, "Array variable information is missing");
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    size_t arraySize = arrayInfoOpt.value().size;
    std::string elementType = arrayInfoOpt.value().elementType;
    size_t initListSize = arrayDef.getSize();

    if (initListSize != arraySize) {
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "array initialization list size mismatch: expected " << arraySize
            << " elements, got " << initListSize << " for array '"
            << leftVarName << "'";
        SEMANTIC_ERROR(
            oss.str(), loc, "Initializer list size must match array size"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

    LOG_DEBUG(
        "Array definition assignment validated: " + leftVarName + " with " +
        std::to_string(initListSize) + " elements"
    );

    if (m_scopePtr->isSymbolInitialized(leftVarName)) {
        validateRebinding(leftVarName, node);
        LOG_DEBUG("Rebinding detected for array: " + leftVarName);
    } else {
        m_scopePtr->markSymbolInitialized(leftVarName);
        LOG_DEBUG("First binding for array: " + leftVarName);
    }

    visitArrayDef(arrayDef);

    bool isStructArray = m_structDefinitions.find(elementType) !=
                         m_structDefinitions.end();

    if (isStructArray) {
        // 结构体数组: 每元素按扁平字段拆解, 与 visitArrayDef+visitBraceExpr
        // 的 push 顺序对齐, 逐字段调 applyLeafFieldAssignment (复用零成本
        // 重命名/栈到栈拷贝/setFixed 等分支).
        std::vector<std::vector<std::pair<std::string, std::string>>>
            perElemFlat;
        perElemFlat.reserve(arraySize);
        size_t totalFieldsExpected = 0;
        for (size_t i = 0; i < arraySize; ++i) {
            std::string elemLabel =
                leftVarName + "[" +
                numberToScriptHex(static_cast<int64_t>(i)) + "]";
            auto flat = getStructFieldsExpanded(
                elementType, elemLabel, m_structDefinitions
            );
            if (flat.empty()) {
                SourceLocation loc = getNodeLocation(node);
                std::ostringstream oss;
                oss << "failed to flatten struct '" << elementType
                    << "' for array assignment of '" << leftVarName << "'";
                SEMANTIC_ERROR(
                    oss.str(),
                    loc,
                    "Check struct definition and nested field types"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }
            totalFieldsExpected += flat.size();
            perElemFlat.push_back(std::move(flat));
        }

        LOG_DEBUG(
            "Processing struct array assignment: " + leftVarName +
            ", arraySize=" + std::to_string(arraySize) +
            ", totalFields=" + std::to_string(totalFieldsExpected)
        );

        // 预校验: 每个元素必须是 BraceExpr, 扁平数量匹配.
        for (size_t i = 0; i < arraySize; ++i) {
            auto* brace =
                dynamic_cast<BraceExprNode*>(arrayDef.elements[i].get());
            if (!brace) {
                SourceLocation loc = getNodeLocation(node);
                std::ostringstream oss;
                oss << "struct array element [" << i
                    << "] must be a brace expression '{...}' matching the "
                       "struct fields";
                SEMANTIC_ERROR(
                    oss.str(),
                    loc,
                    "Each struct array element must use brace initializer"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }
            if (brace->elements.size() != perElemFlat[i].size()) {
                SourceLocation loc = getNodeLocation(node);
                std::ostringstream oss;
                oss << "struct array element [" << i << "] has "
                    << brace->elements.size() << " value(s) but struct '"
                    << elementType << "' requires "
                    << perElemFlat[i].size() << " flattened field(s)";
                SEMANTIC_ERROR(
                    oss.str(),
                    loc,
                    "Match brace element count to flattened struct fields"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }
        }

        // 字段必须已声明 (由 visitArrayDecl/参数展开预登记); 与
        // tryHandleStructBraceAssignment 一致, 未声明即报错.
        for (size_t i = 0; i < arraySize; ++i) {
            for (const auto& f : perElemFlat[i]) {
                std::string fieldPathCopy = f.first;
                if (!m_scopePtr->symbolExists(fieldPathCopy)) {
                    SourceLocation loc = getNodeLocation(node);
                    std::ostringstream oss;
                    oss << "struct array field '" << f.first
                        << "' is not defined; cannot assign array literal to '"
                        << leftVarName << "'";
                    SEMANTIC_ERROR(
                        oss.str(),
                        loc,
                        "Declare the struct array or ensure all fields exist"
                    );
                    LOG_ERROR(oss.str());
                    throw std::runtime_error(oss.str());
                }
            }
        }

        visitArrayDef(arrayDef);

        std::vector<tbc::StackElement> allValues;
        allValues.reserve(totalFieldsExpected);
        for (size_t k = 0; k < totalFieldsExpected; ++k) {
            auto rhsOpt = m_scopePtr->pop();
            if (!rhsOpt.has_value()) {
                SourceLocation loc = getNodeLocation(node);
                std::ostringstream oss;
                oss << "not enough values on stack for struct array '"
                    << leftVarName << "' (expected "
                    << std::to_string(totalFieldsExpected) << ", got "
                    << std::to_string(k) << ")";
                SEMANTIC_ERROR(
                    oss.str(), loc, "Ensure all struct fields are provided"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }
            allValues.push_back(rhsOpt.value());
        }

        // visitArrayDef 逆序 visit 元素, visitBraceExpr 正序 push 字段:
        // 栈顶是 arr[0] 的最后一个扁平字段, pop 序:
        //   arr[0].f(M-1)..arr[0].f0, arr[1].f(M-1)..arr[N-1].f0.
        // 按 allValues 线性游标访问, 字段索引从高到低.
        size_t cursor = 0;
        for (size_t arrayIdx = 0; arrayIdx < arraySize; ++arrayIdx) {
            const auto& flat = perElemFlat[arrayIdx];
            size_t M = flat.size();
            for (size_t rev = 0; rev < M; ++rev) {
                size_t fieldIdx = M - 1 - rev;
                const auto& rhsVal = allValues[cursor++];
                const std::string& fieldPath = flat[fieldIdx].first;
                const std::string& expectedType = flat[fieldIdx].second;
                applyLeafFieldAssignment(
                    fieldPath, expectedType, rhsVal, node
                );
            }
        }
    } else {
        // 非结构体数组: 逐元素复用 applyLeafFieldAssignment, 与其他赋值
        // 路径语义一致 (零成本重命名/栈到栈拷贝/setFixed).
        for (size_t i = 0; i < arraySize; ++i) {
            auto valueOpt = m_scopePtr->pop();
            if (!valueOpt.has_value()) {
                SourceLocation loc = getNodeLocation(node);
                std::ostringstream oss;
                oss << "not enough initializer values for array " << leftVarName
                    << ", expected " << std::to_string(arraySize) << ", got "
                    << std::to_string(i);
                SEMANTIC_ERROR(
                    oss.str(), loc, "Ensure all array elements are provided"
                );
                LOG_ERROR(oss.str());
                throw std::runtime_error(oss.str());
            }

            // 数组元素标签 (逆序赋值, visitArrayDef 按逆序压栈).
            std::string elementLabel = m_scopePtr->getArrayElementLabel(
                leftVarName, arraySize - 1 - i
            );

            applyLeafFieldAssignment(
                elementLabel, elementType, valueOpt.value(), node
            );
        }
    }

    LOG_DEBUG("Array definition assignment completed for: " + leftVarName);
    return true;
}

bool ASTToBytecodeVisitor::checkExpressionContainsVariable(
    const ExprNode& expr,
    const std::string& varName
) const
{
    if (auto identifierNode = dynamic_cast<const IdentifierNode*>(&expr)) {
        return identifierNode->name == varName;
    }

    if (auto opNode = dynamic_cast<const OpNode*>(&expr)) {
        bool foundInLeft = false;
        bool foundInRight = false;

        if (opNode->lhs) {
            foundInLeft =
                checkExpressionContainsVariable(*opNode->lhs, varName);
        }

        if (opNode->rhs) {
            foundInRight =
                checkExpressionContainsVariable(*opNode->rhs, varName);
        }

        return foundInLeft || foundInRight;
    }

    if (auto callNode = dynamic_cast<const CallNode*>(&expr)) {
        for (const auto& arg : callNode->args) {
            if (checkExpressionContainsVariable(*arg, varName)) {
                return true;
            }
        }
        return false;
    }

    if (auto methodCallNode = dynamic_cast<const MethodCallNode*>(&expr)) {
        if (methodCallNode->object &&
            checkExpressionContainsVariable(*methodCallNode->object, varName)) {
            return true;
        }

        for (const auto& arg : methodCallNode->args) {
            if (checkExpressionContainsVariable(*arg, varName)) {
                return true;
            }
        }
        return false;
    }

    if (auto fieldAccessNode = dynamic_cast<const FieldAccessNode*>(&expr)) {
        if (fieldAccessNode->base) {
            return checkExpressionContainsVariable(
                *fieldAccessNode->base, varName
            );
        }
        return false;
    }

    if (auto indexAccessNode = dynamic_cast<const IndexAccessNode*>(&expr)) {
        bool foundInBase = false;
        bool foundInIndex = false;

        if (indexAccessNode->base) {
            foundInBase = checkExpressionContainsVariable(
                *indexAccessNode->base, varName
            );
        }

        if (indexAccessNode->index) {
            foundInIndex = checkExpressionContainsVariable(
                *indexAccessNode->index, varName
            );
        }

        return foundInBase || foundInIndex;
    }

    if (dynamic_cast<const LiteralNode*>(&expr)) {
        return false;
    }

    // 其他未处理节点类型保守返回 false.
    return false;
}

void ASTToBytecodeVisitor::keep(std::vector<tbc::StackElement>& elementVec)
{
    LOG_DEBUG("start set keep symbol.");
    SymbolTable& currentSymtab = m_scopePtr->getCurrentSymtab();

    // 加单个标签到 keep; 主/副栈都没有则报语义错误.
    auto keepOne = [&](const std::string& label, const char* kind) {
        if (!currentSymtab.getPos(label).has_value() &&
            !currentSymtab.getPos(label, true).has_value()) {
            std::ostringstream oss;
            oss << "Keep: " << kind << " '" << label
                << "' is not found on main stack or alt stack";
            SourceLocation loc("", 0, 0);
            SEMANTIC_ERROR(
                oss.str(),
                loc,
                "Ensure the variable is pushed onto main or alt stack "
                "before calling Keep"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
        }
        currentSymtab.m_keepSymbol.push_back(label);
    };

    for (auto it : elementVec) {
        const std::string& name = it.getName();

        // 数组变量: keep 每个元素.
        if (m_scopePtr->isArraySymbol(name)) {
            auto arrayInfoOpt = m_scopePtr->getArrayInfo(name);
            if (arrayInfoOpt.has_value()) {
                for (const auto& element : arrayInfoOpt.value().elements) {
                    keepOne(element.qualifiedName, "array element");
                }
                continue;
            }
        }

        // 结构体: 已拆分则 keep 每字段, 未拆分则按整体 keep.
        if (m_scopePtr->isCompoundTypeSymbol(name) &&
            m_scopePtr->isCompoundTypeSplitted(name)) {
            auto compoundInfoOpt = m_scopePtr->getCompoundTypeInfo(name);
            if (compoundInfoOpt.has_value()) {
                for (const auto& field : compoundInfoOpt.value().fields) {
                    keepOne(name + "." + field.name, "struct field");
                }
                continue;
            }
        }

        keepOne(name, "variable");
    }
}

void ASTToBytecodeVisitor::executeStatements(
    const std::vector<std::unique_ptr<StmtNode>>& statements,
    size_t startIndex
)
{
    FlowResult sequenceFlow = FlowResult::FallsThrough;
    bool immutableSuffixEligible = false;
    std::string codeLevel{};
    for (auto it : m_codeBlockLevel) {
        codeLevel = codeLevel + std::to_string(it) + "-";
    }
    m_codeBlockLevel.push_back(0);
    DEFER_BLOCK(m_codeBlockLevel.pop_back(););

    // Only a Push following a Return in the public function's own statement
    // sequence is deployment suffix data. A Push after a nested/branch-local
    // Return is unreachable code and must not leak into the executable
    // template (or demand a fixed self-placeholder length).
    const bool allowImmutableSuffix =
        m_immutableSuffixBlock &&
        &statements == &m_immutableSuffixBlock->statements &&
        m_activePrivateFunctions.empty();

    for (size_t statementIndex = startIndex;
         statementIndex < statements.size();
         ++statementIndex) {
        const auto& stmt = statements[statementIndex];

        const bool isImmutableSuffix =
            immutableSuffixEligible &&
            isImmutableSuffixStatement(stmt.get());

        // Uppercase Return makes ordinary following statements unreachable.
        // A trailing Push is the contract's explicit immutable SuffixData and
        // must still be emitted behind the finalizer's padding boundary.
        if (sequenceFlow == FlowResult::ScriptTerminate &&
            !isImmutableSuffix) {
            continue;
        }

        const bool previousImmutableSuffixState =
            m_isEmittingImmutableSuffix;
        m_isEmittingImmutableSuffix = isImmutableSuffix;
        DEFER_BLOCK(
            m_isEmittingImmutableSuffix = previousImmutableSuffixState;
        );

        m_codeBlockLevel.back()++;
        std::string stmtStr = codeLevel +
                              (m_codeBlockLevel.empty()
                                   ? ""
                                   : std::to_string(*m_codeBlockLevel.rbegin())
                              );

        LOG_DEBUG("Start parsing statement #" + stmtStr);
        std::string newSymbol;
        std::string stackStatus;
        m_scopePtr->symbolStatus(newSymbol, stackStatus);
        LOG_DEBUG(newSymbol);
        LOG_DEBUG(stackStatus);

        const auto* previousContinuationStatements =
            m_inlineContinuationStatements;
        const size_t previousContinuationStart = m_inlineContinuationStart;
        DEFER_BLOCK(
            m_inlineContinuationStatements = previousContinuationStatements;
            m_inlineContinuationStart = previousContinuationStart;
        );

        if (statementIndex + 1 < statements.size() &&
            inlineReturnArity(stmt.get()).has_value()) {
            // 不只记录当前层的直接单边 return。祖先语句包含嵌套
            // lowercase return 时，最内层 if 也需要看到祖先 block 的
            // continuation，才能逐层生成 returned guard。
            m_inlineContinuationStatements = &statements;
            m_inlineContinuationStart = statementIndex + 1;
        }

        m_lastFlowResult = FlowResult::FallsThrough;
        try {
            stmt->accept(*this);
        } catch (const std::runtime_error& e) {
            throw;
        } catch (const std::exception& e) {
            SourceLocation loc("", 0, 0);
            std::ostringstream oss;
            oss << "internal error in statement #" << stmtStr << ": "
                << e.what();
            LOG_CRITICAL("Internal error - ", oss.str());
            INTERNAL_ERROR(
                oss.str(),
                loc,
                "This is an internal compiler error. Please report this "
                "issue"
            );
            throw;
        } catch (...) {
            // 未知异常: 记录后继续, 不终止编译.
            SourceLocation loc("", 0, 0);
            std::ostringstream oss;
            oss << "unknown error in statement #" << stmtStr;
            LOG_CRITICAL("Internal error - ", oss.str());
            INTERNAL_ERROR(
                oss.str(),
                loc,
                "This is an internal compiler error. Please report this "
                "issue"
            );
            throw;
        }

        if (m_lastFlowResult == FlowResult::FallsThrough &&
            !compiler_flow::statementAlwaysTerminates(stmt.get()) &&
            !isImmutableSuffix) {
            emitLifetimeCleanupAfter(*stmt);
        }

        auto subStr = m_generator.subStr();
        newSymbol.clear();
        stackStatus.clear();
        m_scopePtr->symbolStatus(newSymbol, stackStatus);
        LOG_DEBUG(newSymbol);
        LOG_DEBUG(stackStatus);
        LOG_DEBUG("sub instrut #", stmtStr, ": ", subStr);

        m_generator.mergeSubOverall();

        if (m_currentReturnNode != nullptr) {
            m_currentReturnNode = nullptr;
        }

        if (m_lastFlowResult == FlowResult::InlineReturn &&
            sequenceFlow == FlowResult::FallsThrough) {
            sequenceFlow = FlowResult::InlineReturn;
            break;
        }
        if (m_lastFlowResult == FlowResult::ScriptTerminate &&
            sequenceFlow == FlowResult::FallsThrough) {
            // 普通后续语句会跳过；显式 Push suffix 仍允许在 padding 后生成。
            sequenceFlow = FlowResult::ScriptTerminate;
            immutableSuffixEligible =
                allowImmutableSuffix && isDirectScriptReturn(stmt.get());
        }
    }

    m_lastFlowResult = sequenceFlow;
}

const apc::compiler::AstLifetimePlan*
ASTToBytecodeVisitor::lifetimePlanFor(const FunctionNode& function) const
{
    if (!m_lifetimePlans) {
        return nullptr;
    }
    const auto found = m_lifetimePlans->find(&function);
    if (found == m_lifetimePlans->end() || !found->second.valid()) {
        return nullptr;
    }
    return &found->second;
}

void ASTToBytecodeVisitor::emitLifetimeCleanupAfter(
    const StmtNode& statement
)
{
    if (!m_currentLifetimePlan || m_isEmittingImmutableSuffix ||
        m_lifetimeControlFlowDepth != 0) {
        return;
    }

    SymbolTable& symbolTable = m_scopePtr->getCurrentSymtab();
    for (const auto& suggestion :
         m_currentLifetimePlan->suggestionsAfter(&statement)) {
        if (suggestion.bindings.empty()) {
            continue;
        }

        std::optional<std::string> physicalName;
        std::optional<int64_t> physicalPosition;
        std::vector<std::string> logicalNames;
        bool safe = true;
        for (const auto& binding : suggestion.bindings) {
            // The symbol table currently tracks names and physical slots,
            // not the planner's ValueId. If a binding has been redefined, a
            // kill for an older version cannot be resolved safely by name.
            size_t bindingVersionCount = 0;
            bool exactVersionPresent = false;
            for (const auto& record :
                 m_currentLifetimePlan->valueRecords) {
                if (record.binding != binding.binding) {
                    continue;
                }
                ++bindingVersionCount;
                exactVersionPresent = exactVersionPresent ||
                                      record.value == binding.value;
            }
            if (bindingVersionCount != 1 || !exactVersionPresent) {
                safe = false;
                break;
            }

            const std::string& logicalName = binding.name;
            if (logicalName.empty() ||
                logicalName.find('.') != std::string::npos ||
                logicalName.find('[') != std::string::npos ||
                isWholeArrayElement(logicalName) ||
                symbolTable.isArraySymbol(logicalName) ||
                symbolTable.isCompoundTypeSymbol(logicalName)) {
                safe = false;
                break;
            }

            const std::string resolved =
                symbolTable.resolveBindSymbol(logicalName);
            if (resolved.empty() ||
                tbc::CompilerPlaceholder::isPlaceholder(resolved) ||
                isWholeArrayElement(resolved) ||
                symbolTable.isArraySymbol(resolved) ||
                symbolTable.isCompoundTypeSymbol(resolved) ||
                std::find(
                    symbolTable.m_keepSymbol.begin(),
                    symbolTable.m_keepSymbol.end(),
                    logicalName
                ) != symbolTable.m_keepSymbol.end() ||
                std::find(
                    symbolTable.m_keepSymbol.begin(),
                    symbolTable.m_keepSymbol.end(),
                    resolved
                ) != symbolTable.m_keepSymbol.end()) {
                safe = false;
                break;
            }

            const auto mainPosition =
                symbolTable.getPhysicalPos(resolved, false);
            const auto altPosition =
                symbolTable.getPhysicalPos(resolved, true);
            if (!mainPosition.has_value() || altPosition.has_value() ||
                *mainPosition < 0 ||
                *mainPosition > std::numeric_limits<int>::max()) {
                safe = false;
                break;
            }
            if (physicalName.has_value() &&
                (*physicalName != resolved ||
                 *physicalPosition != *mainPosition)) {
                // An atom may describe aliases, but one cleanup may retire
                // only one proven physical slot.
                safe = false;
                break;
            }
            physicalName = resolved;
            physicalPosition = *mainPosition;
            if (std::find(
                    logicalNames.begin(), logicalNames.end(), logicalName
                ) == logicalNames.end()) {
                logicalNames.push_back(logicalName);
            }
        }

        if (!safe || !physicalName.has_value() ||
            !physicalPosition.has_value()) {
            continue;
        }

        const int position = static_cast<int>(*physicalPosition);
        if (position == 0) {
            m_generator.emit(tbc::BytOpcode::OP_DROP);
            m_scopePtr->pop();
        } else if (position == 1) {
            m_generator.emit(tbc::BytOpcode::OP_NIP);
            m_scopePtr->dropAt(1);
        } else {
            emitRoll(position);
            m_scopePtr->roll(position);
            m_generator.emit(tbc::BytOpcode::OP_DROP);
            m_scopePtr->pop();
        }
        ++m_lifetimeCleanupCount;

#ifdef ENABLE_DEBUGGER
        if (m_debugInfoGen) {
            const size_t endPC = m_generator.getCurrentPC();
            for (const auto& logicalName : logicalNames) {
                m_debugInfoGen->onVariableEnd(logicalName, endPC);
            }
        }
#endif
    }
}

void ASTToBytecodeVisitor::registerWholeArrayElement(
    const std::string& name,
    size_t arraySize,
    size_t elementByteSize
)
{
    LOG_DEBUG(
        "Registering whole array element: " + name +
        " (size=" + std::to_string(arraySize) +
        ", elementByteSize=" + std::to_string(elementByteSize) + ")"
    );
    m_wholeArrayElements[name] = std::make_pair(arraySize, elementByteSize);
}

void ASTToBytecodeVisitor::unregisterWholeArrayElement(const std::string& name)
{
    LOG_DEBUG("Removed whole array element: " + name);
    m_wholeArrayElements.erase(name);
}

bool ASTToBytecodeVisitor::isWholeArrayElement(const std::string& name) const
{
    return m_wholeArrayElements.find(name) != m_wholeArrayElements.end();
}

std::optional<std::pair<size_t, size_t>>
ASTToBytecodeVisitor::getWholeArrayInfo(const std::string& name) const
{
    auto it = m_wholeArrayElements.find(name);
    if (it != m_wholeArrayElements.end()) {
        return it->second;
    }
    return std::nullopt;
}

void ASTToBytecodeVisitor::splitWholeArrayElement(const std::string& name)
{
    LOG_DEBUG("Splitting whole array element: " + name);

    auto infoOpt = getWholeArrayInfo(name);
    if (!infoOpt.has_value()) {
        LOG_ERROR("Cannot split non-whole array element: " + name);
        return;
    }

    auto [arraySize, elementByteSize] = infoOpt.value();

    auto posOpt = m_scopePtr->getPos(name);
    if (!posOpt.has_value()) {
        LOG_ERROR("Array element not found in stack: " + name);
        return;
    }

    int position = static_cast<int>(posOpt.value());

    if (position == 0) {
        LOG_DEBUG("Array at position 0, no move needed");
    } else if (position == 1) {
        m_generator.emit(tbc::BytOpcode::OP_SWAP);
        m_scopePtr->roll(position);
    } else if (position == 2) {
        m_generator.emit(tbc::BytOpcode::OP_ROT);
        m_scopePtr->roll(position);
    } else {
        LOG_DEBUG(
            "Moving array to stack top from position " +
            std::to_string(position)
        );
        m_generator.emit(numberToScriptHex(position));
        m_generator.emit(tbc::BytOpcode::OP_ROLL);
        m_scopePtr->roll(position);
    }

    // OP_SPLIT 将数组拆为独立元素.
    auto element = m_scopePtr->pop();
    if (!element.has_value()) {
        LOG_ERROR("Failed to pop element from stack: " + name);
        return;
    }
    if (arraySize >= 2) {
        for (size_t i = 0; i < arraySize - 1; ++i) {
            m_generator.emit(tbc::BytOpcode::OP_8);
            m_generator.emit(tbc::BytOpcode::OP_SPLIT);
        }
    }

    // 同名符号存在则仅做元素标签同步, 不重复 defineArray.
    bool arrayDefined =
        m_scopePtr->defineArray(name, "uint64", arraySize, true);
    if (!arrayDefined) {
        LOG_DEBUG("defineArray skipped (symbol exists). "
                  "Proceeding with element label sync");
    }
    // OP_SPLIT 在运行时留下 N 个 8 字节元素; 这里把 N 个 name[i] 标签
    // 显式 push 到跟踪栈以与运行时一致.
    for (size_t i = 0; i < arraySize; ++i) {
        std::string elementLabel =
            name + "[" + numberToScriptHex(static_cast<int64_t>(i)) + "]";
        m_scopePtr->push(elementLabel, "uint64", elementLabel);
    }

    unregisterWholeArrayElement(name);

    LOG_DEBUG("Array element splitting completed: " + name);
}

// 首次绑定 a = b 时把 b 的复合身份搬到 a, 覆盖四类元数据:
//   1) m_wholeArrayElements (uint64[N] + b.* 前缀条目).
//   2) 普通数组 (renameArraySymbol).
//   3) 复合类型 (renameCompoundSymbol + 前缀迁移).
//   4) 结构体变量 (符号表 + 栈上 b.* -> a.*).
bool ASTToBytecodeVisitor::transferCompositeIdentity(
    const std::string& oldName,
    const std::string& newName
)
{
    if (oldName.empty() || newName.empty() || oldName == newName) {
        return false;
    }

    bool transferred = false;

    if (auto arrInfoOpt = getWholeArrayInfo(oldName); arrInfoOpt.has_value()) {
        auto [arraySize, elementByteSize] = arrInfoOpt.value();
        unregisterWholeArrayElement(oldName);
        registerWholeArrayElement(newName, arraySize, elementByteSize);
        LOG_DEBUG(
            "Transferred whole array from " + oldName + " to " + newName
        );
        transferred = true;
    }

    if (m_scopePtr->isArraySymbol(oldName)) {
        if (m_scopePtr->renameArraySymbol(oldName, newName)) {
            LOG_DEBUG(
                "Transferred array symbol from " + oldName + " to " + newName
            );
            transferred = true;
        }
    }

    if (m_scopePtr->isCompoundTypeSymbol(oldName)) {
        if (m_scopePtr->renameCompoundSymbol(oldName, newName)) {
            LOG_DEBUG(
                "Transferred compound type from " + oldName + " to " + newName
            );
            transferred = true;
        }
    }

    // 结构体: 扁平字段以 oldName. 为前缀, 作用域条目与栈槽一并改名.
    const std::string oldPrefix = oldName + ".";
    const std::string newPrefix = newName + ".";

    const SymbolTable& currentState = m_scopePtr->getCurrentSymtab();
    const bool hasStructuredChildren = std::any_of(
        currentState.m_currentScope.begin(),
        currentState.m_currentScope.end(),
        [&](const auto& entry) {
            return entry.first.starts_with(oldPrefix);
        }
    ) ||
                                       (currentState.m_stackPtr &&
                                        std::any_of(
                                            currentState.m_stackPtr
                                                ->getStackContent()
                                                .begin(),
                                            currentState.m_stackPtr
                                                ->getStackContent()
                                                .end(),
                                            [&](const StackElement& element) {
                                                return element.getName()
                                                    .starts_with(oldPrefix);
                                            }
                                        ));

    // 仅复合值转移 bare 符号。标量 a=b 必须走后续拷贝语义，不能把 a
    // 在编译期偷偷改名成 b。
    if (transferred || hasStructuredChildren) {
        std::string probe = oldName;
        if (m_scopePtr->symbolExists(probe)) {
            if (m_scopePtr->renameSymbolEntry(oldName, newName)) {
                LOG_DEBUG(
                    "Transferred bare symbol entry from " + oldName + " to " +
                    newName
                );
                transferred = true;
            }
        }
    }

    // 作用域内 oldName. 前缀的字段条目 + 栈槽 + ArrayInfo.
    m_scopePtr->renameEntriesByPrefix(oldPrefix, newPrefix);

    // m_wholeArrayElements 中 oldName. 前缀条目 (嵌套 uint64[N] 字段).
    {
        std::vector<std::pair<std::string, std::pair<size_t, size_t>>>
            renamedEntries;
        for (auto it = m_wholeArrayElements.begin();
             it != m_wholeArrayElements.end();) {
            const std::string& key = it->first;
            if (key.size() > oldPrefix.size() &&
                key.compare(0, oldPrefix.size(), oldPrefix) == 0) {
                std::string newKey = newPrefix + key.substr(oldPrefix.size());
                renamedEntries.emplace_back(newKey, it->second);
                it = m_wholeArrayElements.erase(it);
                transferred = true;
            } else {
                ++it;
            }
        }
        for (auto& [k, v] : renamedEntries) {
            m_wholeArrayElements.emplace(k, v);
            LOG_DEBUG("Transferred whole-array field registration to " + k);
        }
    }

    return transferred;
}

bool ASTToBytecodeVisitor::ensureStructArrayInfo(
    const std::string& arrayBase,
    StructArrayInfo& outInfo,
    const SourceLocation& loc
)
{
    auto arrayInfoOpt = m_scopePtr->getArrayInfo(arrayBase);
    if (!arrayInfoOpt.has_value()) {
        return false;
    }
    const std::string& elementType = arrayInfoOpt.value().elementType;
    if (m_structDefinitions.find(elementType) == m_structDefinitions.end()) {
        return false;
    }

    outInfo.elementType = elementType;

    auto flatFields =
        getStructFieldsExpanded(elementType, "", m_structDefinitions);
    if (flatFields.empty()) {
        std::ostringstream oss;
        oss << "struct '" << elementType << "' flatten failed";
        SEMANTIC_ERROR(oss.str(), loc, "Check struct definition");
        LOG_ERROR(oss.str());
        return false;
    }
    outInfo.stride = static_cast<int64_t>(flatFields.size());
    outInfo.fieldOffsets.clear();
    for (size_t i = 0; i < flatFields.size(); ++i) {
        outInfo.fieldOffsets[flatFields[i].first] = static_cast<int64_t>(i);
    }

    // basePos: 第 0 元素首个扁平字段的位置.
    std::string firstFlatField = flatFields.front().first;
    std::string zeroIdx = numberToScriptHex(0);
    std::string basePath = arrayBase + "[" + zeroIdx + "]" + firstFlatField;
    auto basePosOpt = m_scopePtr->getPos(basePath);
    if (!basePosOpt.has_value()) {
        std::ostringstream oss;
        oss << "base position not found for '" << basePath << "'";
        SEMANTIC_ERROR(
            oss.str(), loc, "Ensure struct array is expanded on stack"
        );
        LOG_ERROR(oss.str());
        return false;
    }
    outInfo.basePos = static_cast<int64_t>(basePosOpt.value()) - 1;
    return true;
}

bool ASTToBytecodeVisitor::isStructArrayFieldSubfield(
    const std::string& fieldPath
) const
{
    size_t bracketPos = fieldPath.find('[');
    size_t closingBracketPos = fieldPath.find(']', bracketPos);

    if (bracketPos == std::string::npos ||
        closingBracketPos == std::string::npos) {
        return false;
    }

    std::string arrayBase = fieldPath.substr(0, bracketPos);

    // ']' 后跟 '.' 则有字段访问.
    if (closingBracketPos + 1 < fieldPath.size() &&
        fieldPath[closingBracketPos + 1] == '.') {
        std::string afterBracket = fieldPath.substr(closingBracketPos + 1);

        SourceLocation loc("", 0, 0);
        StructArrayInfo info;

        auto arrayInfoOpt = const_cast<ASTToBytecodeVisitor*>(this)
                                ->m_scopePtr->getArrayInfo(arrayBase);
        if (!arrayInfoOpt.has_value()) {
            return false;
        }

        const std::string& elementType = arrayInfoOpt.value().elementType;

        if (m_structDefinitions.find(elementType) ==
            m_structDefinitions.end()) {
            return false;
        }

        auto flatFields =
            const_cast<ASTToBytecodeVisitor*>(this)
                ->getStructFieldsExpanded(elementType, "", m_structDefinitions);

        for (const auto& field : flatFields) {
            if (field.first == afterBracket) {
                return true;
            }
        }
    }

    return false;
}

void ASTToBytecodeVisitor::findAllReturnNodes(
    const StmtNode* stmt,
    std::vector<ReturnNode*>& returns
) const
{
    if (!stmt) {
        return;
    }

    if (auto returnNode =
            const_cast<ReturnNode*>(dynamic_cast<const ReturnNode*>(stmt))) {
        returns.push_back(returnNode);
        return;
    }

    if (auto blockNode = dynamic_cast<const BlockNode*>(stmt)) {
        for (const auto& innerStmt : blockNode->statements) {
            findAllReturnNodes(innerStmt.get(), returns);
        }
        return;
    }

    if (auto ifNode = dynamic_cast<const IfNode*>(stmt)) {
        if (ifNode->thenBranch) {
            findAllReturnNodes(ifNode->thenBranch.get(), returns);
        }
        if (ifNode->elseBranch) {
            findAllReturnNodes(ifNode->elseBranch.get(), returns);
        }
        return;
    }

    if (auto forNode = dynamic_cast<const ForNode*>(stmt)) {
        if (forNode->body) {
            findAllReturnNodes(forNode->body.get(), returns);
        }
        return;
    }
}
