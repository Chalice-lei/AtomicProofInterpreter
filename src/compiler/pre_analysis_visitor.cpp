#include "pre_analysis_visitor.h"

#include <climits>

#include "../bytecode/bytecode_builtin_function.h"
#include "../bytecode/bytecode_operation_functions.h"
#include "../log/logger.h"
#include "../util/type_utils.h"
#include "control_flow_analysis.h"
#include "static_integer_evaluator.h"

PreAnalysisVisitor::PreAnalysisVisitor()
    : m_hasErrors(false), m_allowSubscopeAltstack(false),
      m_inIfElseScope(false), m_inPrivateFunction(false)
{
    // Initialize consuming operations list
    m_consumingOperations.insert("Hash160");
    m_consumingOperations.insert("CheckSig");
    m_consumingOperations.insert("Sha256");
    m_consumingOperations.insert("Ripemd160");
    m_consumingOperations.insert("Verify");
}

bool PreAnalysisVisitor::analyze(ASTNode& root)
{
    m_hasErrors = false;
    m_errors.clear();
    m_warnings.clear();
    m_variables.clear();
    m_validatedValueExpressions.clear();
    m_functionDefinitions.clear();
    m_structDefinitions.clear();
    m_altstackArrayExportSchemas.clear();
    m_conflictingAltstackArrayExportSchemas.clear();
    m_staticIntegerValues.clear();
    m_loopExpansionBudget = apc::compiler::RangeExpansionBudget(
        apc::compiler::kMaxExpandedLoopBodies
    );
    m_lastStatementTerminates = false;

    try {
        root.accept(*this);
        checkUnusedVariables();
    } catch (const std::exception& e) {
        reportError(
            "Exception occurred during analysis: " + std::string(e.what()),
            SourceLocation()
        );
        m_hasErrors = true;
    }

    return !m_hasErrors;
}

void PreAnalysisVisitor::visit(ContractNode& node)
{
    LOG_INFO("Analyzing contract: " + node.name);

    // Type-shape calculations used by the interprocedural altstack protocol
    // must not depend on whether a struct is declared before a function.
    for (auto& member : node.members) {
        if (auto* structure = dynamic_cast<StructDefNode*>(member.get())) {
            std::vector<std::pair<std::string, std::string>> fields;
            fields.reserve(structure->fields.size());
            for (const auto& field : structure->fields) {
                fields.emplace_back(
                    field.first, field.second.getTypeString()
                );
            }
            m_structDefinitions[structure->name] = std::move(fields);
        }
    }

    if (m_allowSubscopeAltstack) {
        collectAltstackArrayExportSchemas(node);
    }

    // Register definitions before analyzing bodies so fixed-array arguments
    // can be matched to their inline function parameters.
    m_functionDefinitions.clear();
    for (auto& member : node.members) {
        if (auto* function = dynamic_cast<FunctionNode*>(member.get())) {
            m_functionDefinitions[function->name] = function;
        }
    }

    for (auto& member : node.members) {
        member->accept(*this);
    }
}

void PreAnalysisVisitor::visit(FunctionNode& node)
{
    LOG_INFO("Analyzing function: " + node.name);

    std::string previousFunctionName = m_currentFunctionName;
    bool previousInPrivateFunction = m_inPrivateFunction;
    bool previousInLibraryFunction = m_inLibraryFunction;
    auto previousDeferredParams = m_deferredOwnershipParams;
    auto previousVariables = m_variables;
    auto previousStaticIntegerValues = m_staticIntegerValues;

    m_currentFunctionName = node.name;
    m_inPrivateFunction = isPrivateFunction(node.name);
    m_inLibraryFunction = node.fromLibrary;
    m_deferredOwnershipParams.clear();
    m_staticIntegerValues.clear();

    if (m_inPrivateFunction) {
        LOG_DEBUG("Private function detected: " + node.name);
    }
    if (m_inLibraryFunction) {
        LOG_DEBUG("Library function detected: " + node.name);
    }

    // 私有 / 库函数的形参绑定上下文敏感: 调用点可能传 <self.X> (允许多读)
    // 或栈值 (受 move). PreAnalysis 静态无法判别, 把这些形参登记到
    // m_deferredOwnershipParams, useVariable 见到则跳过 move-once 检查,
    // 留 ast_to_bytecode 阶段用 boundName 兜底.
    if (m_inPrivateFunction || m_inLibraryFunction) {
        for (const auto& param : node.parameters) {
            m_deferredOwnershipParams.insert(param.name);
        }
    }

    // 私有 / 库函数的定义只做隔离分析，真实栈副作用在字节码内联到
    // 调用点时才发生。定义本身不能污染后续 public 函数的状态；
    // public 函数之间仍允许通过共享副栈传递值。
    if (m_inPrivateFunction || m_inLibraryFunction) {
        m_variables.clear();
    } else {
        std::map<std::string, VariableInfo> altStackVariables;
        for (const auto& varPair : m_variables) {
            if (varPair.second.isInAltStack()) {
                altStackVariables.insert(varPair);
                LOG_DEBUG(
                    "Preserving altstack variable for public function: " +
                    varPair.first
                );
            }
        }
        m_variables = altStackVariables;
    }

    // Declare function parameters as stack data
    for (const auto& param : node.parameters) {
        auto arrayType = parseFixedArrayType(param.type);
        if (arrayType.has_value()) {
            const auto& [elementType, arraySize] = arrayType.value();
            declareArrayVariable(
                param.name,
                elementType,
                getNodeLocation(node),
                arraySize,
                calculateElementStackSize(elementType),
                StorageResidency::MAIN_STACK
            );
        } else {
            declareVariable(
                param.name,
                param.type,
                getNodeLocation(node),
                nullptr,
                StorageResidency::MAIN_STACK
            );
        }
    }

    // Analyze function body
    if (node.block) {
        node.block->accept(*this);
    }

    if (m_inPrivateFunction || m_inLibraryFunction) {
        m_variables = previousVariables;
    }

    m_currentFunctionName = previousFunctionName;
    m_inPrivateFunction = previousInPrivateFunction;
    m_inLibraryFunction = previousInLibraryFunction;
    m_deferredOwnershipParams = previousDeferredParams;
    m_staticIntegerValues = previousStaticIntegerValues;
}

void PreAnalysisVisitor::visit(StructDefNode& node)
{
    LOG_INFO("Analyzing struct definition: " + node.name);

    // 转换为兼容格式后存储结构体定义.
    std::vector<std::pair<std::string, std::string>> legacyFields;
    for (const auto& field : node.fields) {
        legacyFields.emplace_back(field.first, field.second.getTypeString());
    }
    m_structDefinitions[node.name] = legacyFields;

    LOG_DEBUG(
        "Stored struct definition: " + node.name + " with " +
        std::to_string(node.fields.size()) + " fields"
    );
}

void PreAnalysisVisitor::visit(BlockNode& node)
{
    bool blockTerminates = false;
    for (auto& stmt : node.statements) {
        m_lastStatementTerminates = false;
        stmt->accept(*this);
        if (m_lastStatementTerminates ||
            !compiler_flow::reachesContinuation(stmt.get())) {
            blockTerminates = true;
            break;
        }
    }
    m_lastStatementTerminates = blockTerminates;
}

void PreAnalysisVisitor::visit(IfNode& node)
{
    LOG_INFO("Analyzing if statement");

    const auto compileTimeCondition =
        node.condition ? evaluateIntegerConstant(*node.condition)
                       : std::nullopt;

    // Analyze condition expression - if consumes the condition value
    if (node.condition) {
        analyzeConditionalExpression(*node.condition);
    }

    bool previousInIfElseScope = m_inIfElseScope;
    m_inIfElseScope = true;

    // Save current variable state before analyzing branches
    auto savedState = saveVariableState();
    const auto savedStaticIntegerValues = m_staticIntegerValues;

    if (compileTimeCondition.has_value()) {
        StmtNode* selectedBranch = compileTimeCondition.value() != 0
                                       ? node.thenBranch.get()
                                       : node.elseBranch.get();
        m_lastStatementTerminates = false;
        if (selectedBranch) {
            selectedBranch->accept(*this);
        }
        const bool selectedTerminates =
            m_lastStatementTerminates ||
            !compiler_flow::reachesContinuation(selectedBranch);
        const auto selectedState = saveVariableState();
        const auto selectedStaticIntegerValues = m_staticIntegerValues;

        // Commit changes to entry-visible bindings without leaking locals from
        // the selected lexical block.
        restoreVariableState(savedState);
        for (auto& [name, variable] : m_variables) {
            auto selected = selectedState.find(name);
            if (selected != selectedState.end()) {
                variable = selected->second;
            }
        }
        m_staticIntegerValues = savedStaticIntegerValues;
        for (const auto& [name, variable] : savedState) {
            (void)variable;
            auto selected = selectedStaticIntegerValues.find(name);
            if (selected != selectedStaticIntegerValues.end()) {
                m_staticIntegerValues[name] = selected->second;
            } else {
                m_staticIntegerValues.erase(name);
            }
        }

        m_inIfElseScope = previousInIfElseScope;
        m_lastStatementTerminates = selectedTerminates;
        return;
    }

    // Analyze then branch
    std::map<std::string, VariableInfo> thenState;
    std::map<std::string, int64_t> thenStaticIntegerValues;
    bool thenTerminates = false;
    if (node.thenBranch) {
        m_lastStatementTerminates = false;
        node.thenBranch->accept(*this);
        thenTerminates =
            m_lastStatementTerminates ||
            !compiler_flow::reachesContinuation(node.thenBranch.get());
        thenState = saveVariableState();
        thenStaticIntegerValues = m_staticIntegerValues;
    } else {
        thenState = savedState;
        thenStaticIntegerValues = savedStaticIntegerValues;
    }

    // Restore state and analyze else branch
    restoreVariableState(savedState);
    m_staticIntegerValues = savedStaticIntegerValues;
    std::map<std::string, VariableInfo> elseState;
    std::map<std::string, int64_t> elseStaticIntegerValues;
    bool elseTerminates = false;
    if (node.elseBranch) {
        m_lastStatementTerminates = false;
        node.elseBranch->accept(*this);
        elseTerminates =
            m_lastStatementTerminates ||
            !compiler_flow::reachesContinuation(node.elseBranch.get());
        elseState = saveVariableState();
        elseStaticIntegerValues = m_staticIntegerValues;
    } else {
        elseState = savedState;
        elseStaticIntegerValues = savedStaticIntegerValues;
    }

    m_inIfElseScope = previousInIfElseScope;

    const bool thenFallsThrough = !thenTerminates;
    const bool elseFallsThrough = !elseTerminates;

    // A terminated path has no ownership state at the join point.
    if (thenFallsThrough && elseFallsThrough) {
        mergeBranchStates(thenState, elseState, savedState);
        m_staticIntegerValues.clear();
        for (const auto& [name, variable] : m_variables) {
            (void)variable;
            const auto thenIt = thenStaticIntegerValues.find(name);
            const auto elseIt = elseStaticIntegerValues.find(name);
            if (thenIt != thenStaticIntegerValues.end() &&
                elseIt != elseStaticIntegerValues.end() &&
                thenIt->second == elseIt->second) {
                m_staticIntegerValues.emplace(name, thenIt->second);
            }
        }
    } else if (thenFallsThrough) {
        restoreVariableState(thenState);
        m_staticIntegerValues = thenStaticIntegerValues;
    } else if (elseFallsThrough) {
        restoreVariableState(elseState);
        m_staticIntegerValues = elseStaticIntegerValues;
    } else {
        restoreVariableState(savedState);
        m_staticIntegerValues = savedStaticIntegerValues;
    }

    // Ownership after the join only comes from reachable paths, but unused
    // diagnostics should still remember a borrow that occurred on either
    // branch (including a branch that terminates).
    for (auto& [name, variable] : m_variables) {
        const auto thenIt = thenState.find(name);
        const auto elseIt = elseState.find(name);
        variable.wasBorrowed = variable.wasBorrowed ||
                               (thenIt != thenState.end() &&
                                thenIt->second.wasBorrowed) ||
                               (elseIt != elseState.end() &&
                                elseIt->second.wasBorrowed);
    }
    m_lastStatementTerminates = thenTerminates && elseTerminates;
}

void PreAnalysisVisitor::visit(AssignNode& node)
{
    auto* integerTarget =
        node.name ? dynamic_cast<IdentifierNode*>(node.name.get()) : nullptr;
    const std::optional<int64_t> assignedInteger =
        node.value ? evaluateIntegerConstant(*node.value) : std::nullopt;

    // An existing main-stack destination makes a main-stack RHS a real copy
    // (OP_PICK in code generation). A first binding has no destination slot
    // and remains a zero-cost rename/move.
    const bool copiesStackValue =
        node.name && node.value &&
        assignmentTargetHasMainStackSlot(*node.name) &&
        expressionHasMainStackSlot(*node.value);

    if (node.value) {
        if (copiesStackValue) {
            analyzeBorrowedExpression(*node.value);
        } else {
            analyzeExpression(*node.value);
        }
    }

    if (node.name) {
        bindAssignmentTarget(
            *node.name, classifyStorage(node.value.get()), node.value.get()
        );
    }

    if (integerTarget && assignedInteger.has_value()) {
        m_staticIntegerValues[integerTarget->name] = assignedInteger.value();
    } else if (integerTarget) {
        m_staticIntegerValues.erase(integerTarget->name);
    }
}

void PreAnalysisVisitor::visit(ForNode& node)
{
    LOG_INFO("Analyzing for loop");

    if (!node.iterable) {
        reportError(
            "for loop missing iterable expression", getNodeLocation(node)
        );
        return;
    }

    // 目前仅支持 Range.
    auto rangeCall = dynamic_cast<CallNode*>(node.iterable.get());
    if (!rangeCall || rangeCall->funcName != "Range") {
        reportError(
            "Only Range(...) is supported in for loops for now",
            getNodeLocation(*node.iterable)
        );
        return;
    }

    if (rangeCall->args.empty() || rangeCall->args.size() > 3) {
        reportError(
            "range() expects 1 to 3 arguments", getNodeLocation(*node.iterable)
        );
        return;
    }

    std::vector<int64_t> bounds;
    bounds.reserve(rangeCall->args.size());

    for (auto& arg : rangeCall->args) {
        const auto evaluated = compiler::StaticIntegerEvaluator::evaluate(
            *arg, m_staticIntegerValues
        );
        if (!evaluated.isKnown()) {
            std::string message =
                "range() arguments must be compile-time integer constants";
            if (evaluated.isError()) {
                message += ": " + evaluated.diagnostic;
            }
            reportError(message, getNodeLocation(*arg));
            return;
        }
        bounds.push_back(evaluated.value);
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
        reportError(planResult.error().message, getNodeLocation(node));
        return;
    }
    const auto& plan = planResult.value();

    node.setInferredType("int");

    // Pre-analysis uses a flat variable map, so preserve outer mutations
    // between expanded iterations while discarding names declared only in an
    // iteration body. The induction target itself survives a non-empty loop.
    auto preLoopState = saveVariableState();
    auto accumulatedState = preLoopState;
    auto preLoopStaticValues = m_staticIntegerValues;
    auto accumulatedStaticValues = preLoopStaticValues;
    const bool targetExistedBeforeLoop =
        preLoopState.find(node.target) != preLoopState.end();

    if (plan.empty()) {
        restoreVariableState(preLoopState);
        m_staticIntegerValues = preLoopStaticValues;
        m_lastStatementTerminates = false;
        return;
    }

    VariableInfo* existingTarget = findVariable(node.target);
    if (existingTarget) {
        const bool isNumericScalar =
            !existingTarget->isArrayType() &&
            existingTarget->fieldOwnership.empty() &&
            apc::compiler::isCompatibleLoopTargetType(existingTarget->type);
        if (!isNumericScalar) {
            reportError(
                "for loop target '" + node.target +
                    "' must be a numeric scalar",
                getNodeLocation(node)
            );
            return;
        }
    }
    bool loopTerminates = false;
    for (uint64_t idx = 0; idx < plan.count(); ++idx) {
        if (auto budgetError = m_loopExpansionBudget.consume()) {
            reportError(budgetError->message, getNodeLocation(node));
            return;
        }

        const auto iterationResult = plan.valueAt(idx);
        const auto* iterationValue = std::get_if<int64_t>(&iterationResult);
        if (!iterationValue) {
            reportError(
                std::get<apc::compiler::RangeError>(iterationResult).message,
                getNodeLocation(node)
            );
            return;
        }

        restoreVariableState(accumulatedState);
        m_staticIntegerValues = accumulatedStaticValues;
        m_staticIntegerValues[node.target] = *iterationValue;

        if (!targetExistedBeforeLoop && idx == 0) {
            declareVariable(
                node.target,
                "int",
                getNodeLocation(node),
                nullptr,
                StorageResidency::FIXED_VALUE
            );
        } else {
            reassignVariable(
                node.target,
                getNodeLocation(node),
                StorageResidency::FIXED_VALUE
            );
        }

        const auto iterationEntryState = saveVariableState();

        bool bodyTerminates = false;
        if (node.body) {
            m_lastStatementTerminates = false;
            node.body->accept(*this);
            bodyTerminates =
                m_lastStatementTerminates ||
                !compiler_flow::reachesContinuation(node.body.get());
        }

        auto iterationExitState = saveVariableState();
        auto iterationExitStaticValues = m_staticIntegerValues;

        for (auto it = iterationExitState.begin();
             it != iterationExitState.end();) {
            const bool isLoopTarget = it->first == node.target;
            const bool existedAtIterationEntry =
                iterationEntryState.find(it->first) !=
                iterationEntryState.end();
            if (!isLoopTarget && !existedAtIterationEntry) {
                it = iterationExitState.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = iterationExitStaticValues.begin();
             it != iterationExitStaticValues.end();) {
            if (iterationEntryState.find(it->first) ==
                iterationEntryState.end()) {
                it = iterationExitStaticValues.erase(it);
            } else {
                ++it;
            }
        }

        accumulatedState = std::move(iterationExitState);
        accumulatedStaticValues = std::move(iterationExitStaticValues);

        if (bodyTerminates) {
            loopTerminates = true;
            break;
        }
    }

    restoreVariableState(accumulatedState);
    m_staticIntegerValues = std::move(accumulatedStaticValues);
    m_lastStatementTerminates = loopTerminates;
}

std::optional<int64_t> PreAnalysisVisitor::evaluateIntegerConstant(
    ExprNode& expr
) const
{
    return evaluateIntegerConstant(expr, m_staticIntegerValues);
}

std::optional<int64_t> PreAnalysisVisitor::evaluateIntegerConstant(
    ExprNode& expr,
    const std::map<std::string, int64_t>& staticIntegerBindings
) const
{
    const auto result = compiler::StaticIntegerEvaluator::evaluate(
        expr, staticIntegerBindings
    );
    return result.isKnown() ? std::optional<int64_t>(result.value)
                            : std::nullopt;
}

void PreAnalysisVisitor::visit(DestructureAssignNode& node)
{
    LOG_DEBUG(
        "Analyzing destructure assignment with " +
        std::to_string(node.targets.size()) + " targets"
    );

    // Analyze right-hand side expression first (may consume variables)
    if (node.value) {
        analyzeExpression(*node.value);
    }

    // Declare all target variables as new variables
    for (const std::string& targetName : node.targets) {
        m_staticIntegerValues.erase(targetName);
        // Check if variable already exists
        VariableInfo* existingVar = findVariable(targetName);
        if (existingVar) {
            // Variable already exists - this is a reassignment
            reassignVariable(targetName, getNodeLocation(node));
            LOG_DEBUG(
                "Reassigning existing variable in destructure: " + targetName
            );
        } else {
            // New variable declaration
            declareVariable(
                targetName,
                "auto", // Type will be inferred from the assignment
                getNodeLocation(node),
                node.value.get()
            );
            LOG_DEBUG("Declaring new variable in destructure: " + targetName);
        }
    }
}

void PreAnalysisVisitor::visit(ExprStmtNode& node)
{
    if (node.expr) {
        // Check if this is a function call with unused return value
        checkUnusedFunctionResult(*node.expr, getNodeLocation(*node.expr));

        // 表达式语句不要求根表达式产生值; 它是 Verify() 这类
        // 零返回值内置函数的合法调用位置. 调用参数仍由 visit(CallNode)
        // 通过 analyzeExpression 按值上下文分析.
        node.expr->accept(*this);
    }
}

void PreAnalysisVisitor::visit(ReturnNode& node)
{
    if (node.expr) {
        // 小写 return: 仅使用不消耗; 大写 Return: 正常分析 (可能消耗).
        if (node.isValueReturn) {
            analyzeExpressionForValueReturn(*node.expr);
        } else {
            analyzeExpression(*node.expr);
        }
    }
    m_lastStatementTerminates = true;
}

void PreAnalysisVisitor::visit(VarDeclNode& node)
{
    const std::optional<int64_t> declaredInteger =
        node.initValue ? evaluateIntegerConstant(*node.initValue)
                       : std::nullopt;

    if (auto fixedArrayType = parseFixedArrayType(node.type);
        fixedArrayType.has_value()) {
        const auto& [elementType, arraySize] = fixedArrayType.value();
        declareArrayVariable(
            node.name,
            elementType,
            getNodeLocation(node),
            arraySize,
            calculateElementStackSize(elementType),
            node.initValue ? StorageResidency::MAIN_STACK
                           : StorageResidency::UNBOUND
        );
    } else {
        declareVariable(
            node.name, node.type, getNodeLocation(node), node.initValue.get()
        );
    }

    if (node.initValue) {
        analyzeExpression(*node.initValue);
    }

    if (declaredInteger.has_value()) {
        m_staticIntegerValues[node.name] = declaredInteger.value();
    } else {
        m_staticIntegerValues.erase(node.name);
    }
}

void PreAnalysisVisitor::visit(LiteralNode& /*node*/)
{
    // Literals don't involve variable usage
}

void PreAnalysisVisitor::visit(IdentifierNode& node)
{
    useVariable(node.name, getNodeLocation(node));
}

void PreAnalysisVisitor::visit(CallNode& node)
{
    checkAltstackOperationAllowed(node.funcName, getNodeLocation(node));

    // 所有调用参数都处于值上下文. 在特殊分支提前校验, 避免
    // SetAlt/SetMain/Move 等分支因提前 return 跳过通用表达式分析.
    for (auto& arg : node.args) {
        if (arg) {
            validateValueProducingExpression(*arg);
        }
    }

    if (node.isRangeCall || node.funcName == "Range") {
        validateRangeCall(node);
        return;
    }

    // Keep only records which values survive scope cleanup. It emits no
    // opcode and therefore borrows all arguments.
    if (node.funcName == "Keep") {
        if (node.args.empty()) {
            reportError(
                "Keep() expects at least one argument", getNodeLocation(node)
            );
            return;
        }
        for (auto& arg : node.args) {
            analyzeBorrowedExpression(*arg);
        }
        return;
    }

    // OP_SIZE has stack effect `x -> x size`: it borrows its argument instead
    // of moving it. Keep this ownership rule aligned with code generation.
    if (node.funcName == "Size") {
        if (node.args.size() != 1) {
            reportError(
                "Size() expects exactly one argument", getNodeLocation(node)
            );
            return;
        }

        analyzeBorrowedExpression(*node.args[0]);
        return;
    }

    // Delete needs element-aware handling for fixed arrays. Other expressions
    // keep the generic analysis path so repeated scalar uses are still caught.
    if (node.funcName == "Delete") {
        if (node.args.empty()) {
            reportError(
                "Delete() expects at least one argument", getNodeLocation(node)
            );
            return;
        }

        for (auto& arg : node.args) {
            if (auto* indexAccess =
                    dynamic_cast<IndexAccessNode*>(arg.get())) {
                std::string arrayName =
                    getVariableFromExpr(*indexAccess->base);
                VariableInfo* var = arrayName.empty()
                                        ? nullptr
                                        : findVariable(arrayName);
                if (var && var->isArrayType()) {
                    if (indexAccess->index) {
                        analyzeExpression(*indexAccess->index);
                    }
                    auto indexOpt =
                        calculateIndexValue(indexAccess->index.get());
                    if (indexOpt.has_value()) {
                        consumeArrayElement(
                            arrayName,
                            indexOpt.value(),
                            getNodeLocation(*indexAccess)
                        );
                    } else {
                        reportError(
                            "Failed to calculate array index for Delete()",
                            getNodeLocation(*indexAccess)
                        );
                    }
                    continue;
                }
            }

            auto [varName, fieldPath] = getFieldPathFromExpr(*arg);
            if (!varName.empty() && fieldPath.empty()) {
                VariableInfo* var = findVariable(varName);
                if (var && var->isArrayType()) {
                    consumeWholeArray(varName, getNodeLocation(*arg));
                    continue;
                }
                if (var) {
                    // Delete removes both runtime/fixed storage and the
                    // compile-time integer fact. A later assignment can
                    // rebind the name, but reads before that point must not
                    // observe the deleted value.
                    m_staticIntegerValues.erase(varName);
                    const VariableState stateBeforeDelete = var->state;
                    useVariable(varName, getNodeLocation(*arg));
                    if (stateBeforeDelete == VariableState::DECLARED) {
                        var->state = VariableState::CONSUMED;
                        var->storage = StorageResidency::UNBOUND;
                        var->lastUseLocation = getNodeLocation(*arg);
                    }
                    continue;
                }
            }

            analyzeExpression(*arg);
        }
        return;
    }

    // SetAlt: 把变量移到副栈.
    if (node.funcName == "SetAlt" && node.args.size() == 1) {
        if (auto* indexAccess =
                dynamic_cast<IndexAccessNode*>(node.args[0].get())) {
            std::string arrayName = indexAccess->base
                                        ? getVariableFromExpr(
                                              *indexAccess->base
                                          )
                                        : std::string();
            VariableInfo* array = arrayName.empty()
                                      ? nullptr
                                      : findVariable(arrayName);
            if (array && array->isArrayType()) {
                if (indexAccess->index) {
                    analyzeBorrowedExpression(*indexAccess->index);
                }

                auto index = calculateIndexValue(indexAccess->index.get());
                if (!index.has_value()) {
                    if (indexAccess->index) {
                        auto constant =
                            evaluateIntegerConstant(*indexAccess->index);
                        if (constant.has_value()) {
                            reportError(
                                "Array index " +
                                    std::to_string(constant.value()) +
                                    " is out of bounds for '" + arrayName +
                                    "' of length " +
                                    std::to_string(array->getArraySize()),
                                getNodeLocation(*indexAccess)
                            );
                            return;
                        }
                    }
                    if (array->elementStackSize != 1) {
                        reportError(
                            "SetAlt() requires a compile-time index for "
                            "multi-slot array '" +
                                arrayName + "'; each element occupies " +
                                std::to_string(array->elementStackSize) +
                                " stack slots",
                            getNodeLocation(*indexAccess)
                        );
                        return;
                    }

                    // Do not create a generic runtime-index path here. The
                    // ordinary backend retains its existing fail-closed rule.
                    borrowArrayElement(*indexAccess);
                    return;
                }

                if (index.value() >= array->getArraySize()) {
                    reportError(
                        "Array index " + std::to_string(index.value()) +
                            " is out of bounds for '" + arrayName +
                            "' of length " +
                            std::to_string(array->getArraySize()),
                        getNodeLocation(*indexAccess)
                    );
                    return;
                }
                if (!array->isElementAvailable(index.value())) {
                    reportError(
                        "Array element '" + arrayName + "[" +
                            std::to_string(index.value()) +
                            "]' has been consumed and cannot be moved to "
                            "altstack",
                        getNodeLocation(*indexAccess)
                    );
                    return;
                }

                array->elementStorage[index.value()] =
                    StorageResidency::ALT_STACK;
                array->lastUseLocation = getNodeLocation(*indexAccess);
                LOG_DEBUG(
                    "Array element '" + arrayName + "[" +
                    std::to_string(index.value()) +
                    "]' moved to altstack"
                );
                return;
            }
        }

        std::string varName = getVariableFromExpr(*node.args[0]);
        if (!varName.empty()) {
            VariableInfo* var = findVariable(varName);
            if (!var) {
                reportError(
                    "Undeclared variable: '" + varName + "'",
                    getNodeLocation(*node.args[0])
                );
                return;
            }
            if (var->hasOwnership() && var->state == VariableState::CONSUMED) {
                reportError(
                    "Variable '" + varName +
                        "' has been consumed and cannot be moved to altstack",
                    getNodeLocation(*node.args[0])
                );
                return;
            }
            // SetAlt 只搬位置不消费, 状态保持. 之前 DECLARED→USED 的升级是
            // 旧 useVariable (不查 USED) 的遗留, 在 move-once 规则下会让后续
            // `Keep(x)` / `x op ...` 等正常消费首次访问就报错, 故移除.
            m_staticIntegerValues.erase(varName);
            var->source = DataSource::STACK_DATA;
            var->markInAltStack();
            var->storage = StorageResidency::ALT_STACK;
            LOG_DEBUG("Variable '" + varName + "' moved to altstack");
        } else {
            analyzeBorrowedExpression(*node.args[0]);
        }
        return;
    }

    // SetMain: 把变量从副栈移回主栈.
    if (node.funcName == "SetMain" && node.args.size() == 1) {
        if (auto* indexAccess =
                dynamic_cast<IndexAccessNode*>(node.args[0].get())) {
            if (restoreAltstackArrayElement(*indexAccess)) {
                return;
            }
        }

        std::string varName = getVariableFromExpr(*node.args[0]);
        if (!varName.empty()) {
            VariableInfo* var = findVariable(varName);
            if (!var) {
                // 私有函数会在调用点内联，并可能把其局部值或调用者实参
                // 留在共享副栈。PreAnalysis 不重放调用体，因此先登记一个
                // 延迟绑定，最终由 bytecode 后端的真实副栈符号表验证。
                declareVariable(
                    varName,
                    "auto",
                    getNodeLocation(*node.args[0]),
                    nullptr,
                    StorageResidency::ALT_STACK
                );
                var = findVariable(varName);
                if (var) {
                    var->markInAltStack();
                }
            }
            if (!var) {
                return;
            }
            if (!var->isInAltStack()) {
                // 该值也可能由前一个内联 private 调用从主栈移入副栈。
                // 存储位置检查延迟到后端；后端找不到目标时仍会报错。
                LOG_DEBUG(
                    "Deferring altstack residency check for SetMain('" +
                    varName + "') to bytecode generation"
                );
            }
            // SetMain 把变量搬回主栈, 视为重新绑定, 状态重置为 DECLARED 让后续
            // 消费可走一次完整 move 路径 (原 USED 升级在新规则下会阻断首次消费).
            var->markNotInAltStack();
            var->state = VariableState::DECLARED;
            var->storage = StorageResidency::MAIN_STACK;
            LOG_DEBUG(
                "Variable '" + varName + "' moved from altstack to main stack"
            );
        } else {
            // Do not let complex expressions bypass their own validation.
            // In particular, computed IndexAccess bases must reach their own
            // compile-time-index validation.
            analyzeExpression(*node.args[0]);
        }
        return;
    }

    // Move(): 优先处理数组索引访问的元素消耗.
    if (node.funcName == "Move" && node.args.size() == 1) {
        ExprNode* arg = node.args[0].get();

        if (auto indexAccess = dynamic_cast<IndexAccessNode*>(arg)) {
            std::string arrayName = getVariableFromExpr(*indexAccess->base);
            if (!arrayName.empty()) {
                VariableInfo* var = findVariable(arrayName);
                if (var && var->isArrayType()) {
                    auto indexOpt = calculateIndexValue(indexAccess->index.get()
                    );
                    if (indexOpt.has_value()) {
                        size_t index = indexOpt.value();
                        consumeArrayElement(
                            arrayName, index, getNodeLocation(*indexAccess)
                        );
                        return;
                    } else {
                        reportError(
                            "Failed to calculate array index for move()",
                            getNodeLocation(*indexAccess)
                        );
                        return;
                    }
                }
            }
        }

        // 非数组元素 move: 按常规变量处理.
        std::string varName = getVariableFromExpr(*arg);
        if (!varName.empty()) {
            consumeVariable(varName, getNodeLocation(*arg));
        } else {
            // Complex move operands still need semantic validation even when
            // they cannot participate in the simple variable ownership path.
            analyzeExpression(*arg);
        }
        return;
    }

    // Analyze function arguments. A fixed array passed to an inline function
    // transfers its complete backing identity; bytecode cleanup removes those
    // slots when the callee returns, so later caller access must be rejected
    // rather than treated as an element-wise borrow.
    const FunctionNode* calledFunction = nullptr;
    if (auto functionIt = m_functionDefinitions.find(node.funcName);
        functionIt != m_functionDefinitions.end()) {
        calledFunction = functionIt->second;
    }
    for (size_t i = 0; i < node.args.size(); ++i) {
        auto& arg = node.args[i];
        const bool expectsFixedArray =
            calledFunction && i < calledFunction->parameters.size() &&
            parseFixedArrayType(calledFunction->parameters[i].type).has_value();
        if (expectsFixedArray) {
            const std::string arrayName = getVariableFromExpr(*arg);
            VariableInfo* array = arrayName.empty()
                                      ? nullptr
                                      : findVariable(arrayName);
            if (array && array->isArrayType()) {
                consumeWholeArray(arrayName, getNodeLocation(*arg));
                continue;
            }
        }
        analyzeExpression(*arg);
    }

    if (isConsumingOperation(node.funcName)) {
        for (auto& arg : node.args) {
            auto [varName, fieldPath] = getFieldPathFromExpr(*arg);
            if (!varName.empty()) {
                if (fieldPath.empty()) {
                    consumeVariable(varName, getNodeLocation(*arg));
                } else {
                    consumeField(varName, fieldPath, getNodeLocation(*arg));
                }
            }
        }
    }
}

void PreAnalysisVisitor::validateRangeCall(CallNode& node)
{
    auto location = getNodeLocation(node);

    if (node.args.empty() || node.args.size() > 3) {
        reportError(
            "range() expects between 1 and 3 integer literal arguments",
            location
        );
        return;
    }

    for (size_t i = 0; i < node.args.size(); ++i) {
        auto& arg = node.args[i];
        if (!arg) {
            continue;
        }

        analyzeExpression(*arg);

        auto literal = dynamic_cast<LiteralNode*>(arg.get());
        if (!literal || literal->type != LiteralNode::Type::Number) {
            reportError(
                "range() currently supports only integer literal arguments",
                getNodeLocation(*arg)
            );
            continue;
        }

        if (i == 2) {
            try {
                long long stepValue = std::stoll(literal->value);
                if (stepValue == 0) {
                    reportError(
                        "range() step argument must not be zero",
                        getNodeLocation(*arg)
                    );
                }
            } catch (const std::exception&) {
                reportError(
                    "range() step argument must be a valid integer literal",
                    getNodeLocation(*arg)
                );
            }
        }
    }
}

void PreAnalysisVisitor::visit(MethodCallNode& node)
{
    // Analyze object expression
    if (node.object) {
        // 方法调用的对象必须产生值; 无法解析成变量/字段路径时
        // 下方的 ownership 分支不会递归分析, 因此需先独立校验.
        validateValueProducingExpression(*node.object);

        if (node.methodName == "Clone") {
            if (auto* indexAccess =
                    dynamic_cast<IndexAccessNode*>(node.object.get())) {
                borrowArrayElement(*indexAccess);
                for (auto& arg : node.args) {
                    analyzeExpression(*arg);
                }
                return;
            }
        }

        auto [objName, fieldPath] = getFieldPathFromExpr(*node.object);
        if (!objName.empty()) {
            // .Clone() 是 zero-cost rename 模型里的核心借用操作: emit OP_DUP
            // 产生独立副本, 原对象仍占栈位且可继续消费. 不标 USED, 但需检查
            // 已 CONSUMED 状态 (避免对已 move 走的变量 Clone).
            if (node.methodName == "Clone") {
                VariableInfo* var = findVariable(objName);
                if (var && var->hasOwnership() &&
                    m_deferredOwnershipParams.count(objName) == 0 &&
                    var->state != VariableState::DECLARED) {
                    reportError(
                        "variable '" + objName +
                            "' has been consumed and cannot be cloned "
                            "(move semantics violation)",
                        getNodeLocation(*node.object)
                    );
                }
                LOG_DEBUG("Borrowing object '" + objName + "' via .Clone()");
            } else if (isConsumingOperation(node.methodName)) {
                if (fieldPath.empty()) {
                    consumeVariable(objName, getNodeLocation(*node.object));
                } else {
                    consumeField(
                        objName, fieldPath, getNodeLocation(*node.object)
                    );
                }
            } else {
                if (fieldPath.empty()) {
                    useVariable(objName, getNodeLocation(*node.object));
                } else {
                    useField(objName, fieldPath, getNodeLocation(*node.object));
                }
            }
        } else {
            // Computed receivers such as box.pairs[index].left do not have a
            // simple ownership path. Visit the receiver normally so nested
            // IndexAccess validation is never skipped by a method call.
            analyzeExpression(*node.object);
        }
    }

    // Analyze method arguments
    for (auto& arg : node.args) {
        analyzeExpression(*arg);
    }
}

void PreAnalysisVisitor::visit(OpNode& node)
{
    if (node.lhs) {
        analyzeExpression(*node.lhs);
    }
    if (node.rhs) {
        analyzeExpression(*node.rhs);
    }
}

void PreAnalysisVisitor::visit(FieldAccessNode& node)
{
    auto [varName, fieldPath] = getFieldPathFromExpr(node);

    if (!varName.empty()) {
        if (fieldPath.empty()) {
            // 不应触发, 防御性回退到变量级别.
            useVariable(varName, getNodeLocation(node));
        } else {
            useField(varName, fieldPath, getNodeLocation(node));
        }
    }
}

void PreAnalysisVisitor::visit(IndexAccessNode& node)
{
    if (node.index) {
        analyzeExpression(*node.index);
    }

    std::string arrayName;
    if (node.base) {
        arrayName = getVariableFromExpr(*node.base);
    }

    if (!arrayName.empty()) {
        VariableInfo* var = findVariable(arrayName);
        if (var && var->isArrayType()) {
            // 数组索引访问只借用整个数组, 元素级 ownership 由 useArrayElement
            // 跟踪. 这里跳过 base 的 useVariable, 避免 `for i in Range(N):
            // ... arr[i]` 循环里把整个数组反复标 USED 触发 move-once 误报.
            auto indexOpt = calculateIndexValue(node.index.get());
            if (indexOpt.has_value()) {
                size_t index = indexOpt.value();
                useArrayElement(arrayName, index, getNodeLocation(node));
            } else {
                LOG_WARNING(
                    "Failed to calculate array index for access, skipping "
                    "ownership check"
                );
            }
            return;
        }
    }

    // 非数组 / 复杂 base 表达式: 走常规分析路径.
    if (node.base) {
        analyzeExpression(*node.base);
    }
}

void PreAnalysisVisitor::visit(ArrayDeclNode& node)
{
    const SourceLocation loc = getNodeLocation(node);

    size_t arraySize = 0;
    if (node.initArray) {
        arraySize = node.initArray->elements.size();
    } else if (node.sizeExpr) {
        auto sizeValue = evaluateIntegerConstant(*node.sizeExpr);
        if (!sizeValue.has_value() || sizeValue.value() < 0) {
            reportError(
                "Array '" + node.name +
                    "' size must be a non-negative compile-time integer",
                getNodeLocation(*node.sizeExpr)
            );
            return;
        }
        arraySize = static_cast<size_t>(sizeValue.value());
    } else {
        reportError(
            "Array '" + node.name + "' must have size or initializer", loc
        );
        return;
    }

    size_t elementStackSize = calculateElementStackSize(node.elementType);

    declareArrayVariable(
        node.name,
        node.elementType,
        loc,
        arraySize,
        elementStackSize,
        node.initArray ? StorageResidency::MAIN_STACK
                       : StorageResidency::UNBOUND
    );

    if (node.initArray) {
        node.initArray->accept(*this);
    }
}

void PreAnalysisVisitor::visit(ArrayDefNode& node)
{
    for (const auto& element : node.elements) {
        if (element) {
            analyzeExpression(*element);
        }
    }
}

void PreAnalysisVisitor::visit(BraceExprNode& node)
{
    for (const auto& element : node.elements) {
        if (element) {
            analyzeExpression(*element);
        }
    }
}

// Private method implementations

DataSource PreAnalysisVisitor::classifyVariable(
    const std::string& name,
    ExprNode* initValue
)
{
    // 1. Check if it's a contract member variable
    if (isContractMember(name)) {
        return DataSource::CONTRACT_MEMBER;
    }

    // 2. Check if it's a builtin object
    if (isBuiltinObject(name)) {
        return DataSource::BUILTIN_OBJECT;
    }

    // 3. Match classifyStorage(): statically evaluable integer expressions
    // are fixed values too, not move-only runtime stack data.
    if (initValue &&
        (isConstantValue(*initValue) ||
         evaluateIntegerConstant(*initValue).has_value())) {
        return DataSource::CONSTANT_VALUE;
    }

    // 4. Default to stack data (function parameters, op_function generated
    // data)
    return DataSource::STACK_DATA;
}

void PreAnalysisVisitor::declareVariable(
    const std::string& name,
    const std::string& type,
    const SourceLocation& location,
    ExprNode* initValue,
    StorageResidency storage
)
{
    // Check for redeclaration
    if (m_variables.find(name) != m_variables.end()) {
        reportError("Variable '" + name + "' redeclared", location);
        return;
    }

    // Classify variable data source
    DataSource source = storage == StorageResidency::FIXED_VALUE
                            ? DataSource::CONSTANT_VALUE
                            : classifyVariable(name, initValue);
    if (storage == StorageResidency::UNBOUND && initValue) {
        storage = classifyStorage(initValue);
    }

    // Add variable to current scope
    m_variables.emplace(
        name, VariableInfo(name, type, source, location, storage)
    );

    LOG_DEBUG(
        "Declared variable: " + name + " type: " + type +
        " source: " + std::to_string(static_cast<int>(source))
    );
}

void PreAnalysisVisitor::useVariable(
    const std::string& name,
    const SourceLocation& location
)
{
    // Check if it's a contract member or builtin object first
    if (isContractMember(name) || isBuiltinObject(name)) {
        LOG_DEBUG(
            "Using contract member/builtin object (no ownership check): " + name
        );
        return;
    }

    // 私有 / 库函数形参: 绑定到 <self.X> 还是栈值在 PreAnalysis 阶段静态不可知,
    // 这里跳过 move-once 检查, 让 ast_to_bytecode 阶段用 boundName 兜底.
    if (m_deferredOwnershipParams.count(name) > 0) {
        LOG_DEBUG(
            "Using deferred-ownership param (skip move-once check): " + name
        );
        return;
    }

    VariableInfo* var = findVariable(name);
    if (!var) {
        reportError("Undeclared variable: '" + name + "'", location);
        return;
    }

    // Delete can consume compiler-only fixed bindings as well as runtime
    // stack values. Their source classification remains CONSTANT_VALUE, so
    // check the terminal state before the ownership-only fast path.
    if (var->state == VariableState::CONSUMED) {
        reportError(
            "Variable '" + name +
                "' has been consumed and cannot be used again",
            location
        );
        return;
    }

    // Only check ownership for stack data
    if (!var->hasOwnership()) {
        LOG_DEBUG("Using non-stack variable (no ownership check): " + name);
        return;
    }

    // Check if variable has been consumed
    checkOwnershipViolation(name, location);

    // zero-cost rename / move-once 模型: 栈变量每次 "读" 就是一次消费
    // (visitIdentifier 不 emit OP_DUP, 而是把符号栈位置 rename 给消费者).
    // 已 USED 后再读 -> 违反 move 语义. 重新绑定 (a = expr) 会让
    // reassignVariable 把 state 重置回 DECLARED, 允许下一轮使用.
    if (var->state == VariableState::USED) {
        reportError(
            "variable '" + name +
                "' is consumed more than once (move semantics violation)",
            location
        );
        return;
    }

    var->state = VariableState::USED;
    var->lastUseLocation = location;

    LOG_DEBUG("Using stack variable: " + name);
}

void PreAnalysisVisitor::borrowVariable(
    const std::string& name,
    const SourceLocation& location
)
{
    if (isContractMember(name) || isBuiltinObject(name) ||
        m_deferredOwnershipParams.count(name) > 0) {
        return;
    }

    VariableInfo* var = findVariable(name);
    if (!var) {
        reportError("Undeclared variable: '" + name + "'", location);
        return;
    }

    if (!var->hasOwnership()) {
        return;
    }

    if (var->state != VariableState::DECLARED) {
        reportError(
            "variable '" + name +
                "' has been consumed and cannot be borrowed "
                "(move semantics violation)",
            location
        );
        return;
    }

    var->lastUseLocation = location;
    var->wasBorrowed = true;
    LOG_DEBUG("Borrowing stack variable without consuming it: " + name);
}

void PreAnalysisVisitor::consumeVariable(
    const std::string& name,
    const SourceLocation& location
)
{
    // Check if it's a contract member or builtin object first
    if (isContractMember(name) || isBuiltinObject(name)) {
        LOG_DEBUG(
            "Consuming contract member/builtin object (no ownership check): " +
            name
        );
        return;
    }

    VariableInfo* var = findVariable(name);
    if (!var) {
        reportError("Undeclared variable: '" + name + "'", location);
        return;
    }

    // Only check ownership for stack data
    if (!var->hasOwnership()) {
        LOG_DEBUG("Consuming non-stack variable (no ownership check): " + name);
        return;
    }

    // Consuming calls first analyze their argument, which transitions a fresh
    // binding to USED. Repeated access is reported by useVariable itself.
    checkOwnershipViolation(name, location);

    var->state = VariableState::CONSUMED;
    var->storage = StorageResidency::UNBOUND;
    std::fill(
        var->elementStorage.begin(),
        var->elementStorage.end(),
        StorageResidency::UNBOUND
    );
    var->lastUseLocation = location;

    LOG_DEBUG("Consuming stack variable: " + name);
}

void PreAnalysisVisitor::useField(
    const std::string& varName,
    const std::string& fieldPath,
    const SourceLocation& location
)
{
    if (fieldPath.empty()) {
        useVariable(varName, location);
        return;
    }

    // Check if it's a contract member or builtin object first
    if (isContractMember(varName) || isBuiltinObject(varName)) {
        LOG_DEBUG(
            "Using contract member/builtin object field (no ownership "
            "check): " +
            varName + "." + fieldPath
        );
        return;
    }

    VariableInfo* var = findVariable(varName);
    if (!var) {
        reportError("Undeclared variable: '" + varName + "'", location);
        return;
    }

    // Only check ownership for stack data
    if (!var->hasOwnership()) {
        LOG_DEBUG(
            "Using non-stack variable field (no ownership check): " + varName +
            "." + fieldPath
        );
        return;
    }

    // Check if field has been consumed
    checkFieldOwnershipViolation(varName, fieldPath, location);

    var->markFieldUsed(fieldPath);
    var->lastUseLocation = location;

    LOG_DEBUG("Using field: " + varName + "." + fieldPath);
}

void PreAnalysisVisitor::borrowField(
    const std::string& varName,
    const std::string& fieldPath,
    const SourceLocation& location
)
{
    if (fieldPath.empty()) {
        borrowVariable(varName, location);
        return;
    }

    if (isContractMember(varName) || isBuiltinObject(varName) ||
        m_deferredOwnershipParams.count(varName) > 0) {
        return;
    }

    VariableInfo* var = findVariable(varName);
    if (!var) {
        reportError("Undeclared variable: '" + varName + "'", location);
        return;
    }

    if (!var->hasOwnership()) {
        return;
    }

    if (var->getFieldState(fieldPath) != VariableState::DECLARED) {
        reportError(
            "Field '" + varName + "." + fieldPath +
                "' has been consumed and cannot be borrowed",
            location
        );
        return;
    }

    var->lastUseLocation = location;
    var->wasBorrowed = true;
    LOG_DEBUG(
        "Borrowing field without consuming it: " + varName + "." + fieldPath
    );
}

void PreAnalysisVisitor::consumeField(
    const std::string& varName,
    const std::string& fieldPath,
    const SourceLocation& location
)
{
    if (fieldPath.empty()) {
        consumeVariable(varName, location);
        return;
    }

    // Check if it's a contract member or builtin object first
    if (isContractMember(varName) || isBuiltinObject(varName)) {
        LOG_DEBUG(
            "Consuming contract member/builtin object field (no ownership "
            "check): " +
            varName + "." + fieldPath
        );
        return;
    }

    VariableInfo* var = findVariable(varName);
    if (!var) {
        reportError("Undeclared variable: '" + varName + "'", location);
        return;
    }

    // Only check ownership for stack data
    if (!var->hasOwnership()) {
        LOG_DEBUG(
            "Consuming non-stack variable field (no ownership check): " +
            varName + "." + fieldPath
        );
        return;
    }

    checkFieldOwnershipViolation(varName, fieldPath, location);

    var->markFieldConsumed(fieldPath);
    var->fieldStorage[fieldPath] = StorageResidency::UNBOUND;
    var->lastUseLocation = location;

    LOG_DEBUG("Consuming field: " + varName + "." + fieldPath);
}

void PreAnalysisVisitor::checkFieldOwnershipViolation(
    const std::string& varName,
    const std::string& fieldPath,
    const SourceLocation& location
)
{
    VariableInfo* var = findVariable(varName);
    if (!var || !var->hasOwnership()) {
        return;
    }

    if (var->isFieldConsumed(fieldPath)) {
        reportError(
            "Field '" + varName + "." + fieldPath +
                "' has been consumed and cannot be used again",
            location
        );
    }
}

void PreAnalysisVisitor::declareArrayVariable(
    const std::string& name,
    const std::string& elementType,
    const SourceLocation& location,
    size_t arraySize,
    size_t elementStackSize,
    StorageResidency storage
)
{
    if (m_variables.find(name) != m_variables.end()) {
        reportError("Array variable '" + name + "' redeclared", location);
        return;
    }

    DataSource source = DataSource::STACK_DATA; // 数组默认栈上.
    m_variables.emplace(
        name,
        VariableInfo(
            name,
            elementType + "[]",
            source,
            location,
            arraySize,
            elementStackSize,
            storage
        )
    );

    LOG_DEBUG(
        "Declared array variable: " + name + " elementType: " + elementType +
        " size: " + std::to_string(arraySize) +
        " elementStackSize: " + std::to_string(elementStackSize)
    );
}

void PreAnalysisVisitor::useArrayElement(
    const std::string& arrayName,
    size_t index,
    const SourceLocation& location
)
{
    VariableInfo* var = findVariable(arrayName);
    if (!var) {
        reportError("Undeclared array: '" + arrayName + "'", location);
        return;
    }

    if (!var->isArrayType()) {
        reportError("Variable '" + arrayName + "' is not an array", location);
        return;
    }

    if (!var->hasOwnership()) {
        LOG_DEBUG(
            "Using non-stack array element (no ownership check): " + arrayName +
            "[" + std::to_string(index) + "]"
        );
        return;
    }

    if (!var->isElementAvailable(index)) {
        reportError(
            "Array element '" + arrayName + "[" + std::to_string(index) +
                "]' has been consumed",
            location
        );
        return;
    }

    if (var->state == VariableState::DECLARED) {
        var->state = VariableState::USED;
    }
    var->lastUseLocation = location;

    LOG_DEBUG(
        "Using array element: " + arrayName + "[" + std::to_string(index) + "]"
    );
}

void PreAnalysisVisitor::consumeArrayElement(
    const std::string& arrayName,
    size_t index,
    const SourceLocation& location
)
{
    VariableInfo* var = findVariable(arrayName);
    if (!var) {
        reportError("Undeclared array: '" + arrayName + "'", location);
        return;
    }

    if (!var->isArrayType()) {
        reportError("Variable '" + arrayName + "' is not an array", location);
        return;
    }

    if (!var->hasOwnership()) {
        LOG_DEBUG(
            "Consuming non-stack array element (no ownership check): " +
            arrayName + "[" + std::to_string(index) + "]"
        );
        return;
    }

    if (!var->isElementAvailable(index)) {
        reportError(
            "Array element '" + arrayName + "[" + std::to_string(index) +
                "]' already consumed",
            location
        );
        return;
    }

    if (index < var->elementOwnership.size()) {
        var->elementOwnership[index] = false;
        var->elementStorage[index] = StorageResidency::UNBOUND;
    }

    if (var->isFullyConsumed()) {
        var->state = VariableState::CONSUMED;
        LOG_DEBUG("Array '" + arrayName + "' fully consumed");
    }

    var->lastUseLocation = location;

    LOG_DEBUG(
        "Consumed array element: " + arrayName + "[" + std::to_string(index) +
        "]"
    );
}

void PreAnalysisVisitor::borrowArrayElement(IndexAccessNode& node)
{
    if (!node.base) {
        return;
    }

    std::string arrayName = getVariableFromExpr(*node.base);
    VariableInfo* var =
        arrayName.empty() ? nullptr : findVariable(arrayName);
    if (!var || !var->isArrayType()) {
        analyzeExpression(node);
        return;
    }

    if (node.index) {
        analyzeExpression(*node.index);
    }

    auto indexOpt = calculateIndexValue(node.index.get());
    if (!indexOpt.has_value()) {
        LOG_WARNING(
            "Failed to calculate borrowed array index, skipping ownership check"
        );
        return;
    }

    if (!var->isElementAvailable(indexOpt.value())) {
        reportError(
            "Array element '" + arrayName + "[" +
                std::to_string(indexOpt.value()) +
                "]' has been consumed and cannot be borrowed",
            getNodeLocation(node)
        );
        return;
    }

    var->lastUseLocation = getNodeLocation(node);
    var->wasBorrowed = true;
    LOG_DEBUG(
        "Borrowing array element without consuming it: " + arrayName + "[" +
        std::to_string(indexOpt.value()) + "]"
    );
}

void PreAnalysisVisitor::consumeWholeArray(
    const std::string& arrayName,
    const SourceLocation& location
)
{
    VariableInfo* var = findVariable(arrayName);
    if (!var) {
        reportError("Undeclared array: '" + arrayName + "'", location);
        return;
    }

    if (!var->isArrayType()) {
        consumeVariable(arrayName, location);
        return;
    }

    if (!var->hasOwnership()) {
        return;
    }

    if (var->state == VariableState::CONSUMED || var->isFullyConsumed()) {
        reportError(
            "Array '" + arrayName + "' has already been consumed", location
        );
        return;
    }

    std::fill(
        var->elementOwnership.begin(), var->elementOwnership.end(), false
    );
    std::fill(
        var->elementStorage.begin(),
        var->elementStorage.end(),
        StorageResidency::UNBOUND
    );
    var->state = VariableState::CONSUMED;
    var->storage = StorageResidency::UNBOUND;
    var->lastUseLocation = location;
    LOG_DEBUG("Consumed whole array: " + arrayName);
}

bool PreAnalysisVisitor::isArrayElementAvailable(
    const std::string& arrayName,
    size_t index
) const
{
    auto it = m_variables.find(arrayName);
    if (it == m_variables.end()) {
        return false;
    }

    const VariableInfo& var = it->second;
    return var.isElementAvailable(index);
}

size_t PreAnalysisVisitor::calculateElementStackSize(
    const std::string& elementType
) const
{
    if (elementType == "int" || elementType == "uint64" ||
        elementType == "string" ||
        elementType == "hex" || elementType == "bool") {
        return 1;
    }

    auto structIt = m_structDefinitions.find(elementType);
    if (structIt != m_structDefinitions.end()) {
        // 结构体: 递归累加所有字段栈大小.
        size_t totalSize = 0;
        for (const auto& field : structIt->second) {
            const std::string& fieldType = field.second;
            totalSize += calculateElementStackSize(fieldType);
        }

        LOG_DEBUG(
            "Calculated stack size for struct " + elementType + ": " +
            std::to_string(totalSize) + " stack levels"
        );
        return totalSize;
    }

    // 数组类型 (如 int[]): 用元素类型的大小.
    if (elementType.size() > 2 &&
        elementType.substr(elementType.size() - 2) == "[]") {
        std::string baseType = elementType.substr(0, elementType.size() - 2);
        return calculateElementStackSize(baseType);
    }

    // 未知类型默认 1 个栈位.
    LOG_WARNING(
        "Unknown type '" + elementType + "', defaulting to 1 stack level"
    );
    return 1;
}

std::optional<size_t> PreAnalysisVisitor::calculateIndexValue(
    ExprNode* indexExpr
) const
{
    if (!indexExpr) {
        return std::nullopt;
    }

    auto value = evaluateIntegerConstant(*indexExpr);
    if (!value.has_value() || value.value() < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(value.value());
}

std::optional<std::pair<std::string, size_t>>
PreAnalysisVisitor::parseFixedArrayType(const std::string& type) const
{
    if (auto arrayType = apc::util::parseFixedArrayType(type)) {
        return std::make_pair(arrayType->elementType, arrayType->size);
    }
    return std::nullopt;
}

void PreAnalysisVisitor::collectAltstackArrayExportSchemas(
    ContractNode& contract
)
{
    for (auto& member : contract.members) {
        auto* function = dynamic_cast<FunctionNode*>(member.get());
        if (!function ||
            (!isPrivateFunction(function->name) && !function->fromLibrary)) {
            continue;
        }
        collectAltstackArrayExportSchemas(*function);
    }
}

void PreAnalysisVisitor::collectAltstackArrayExportSchemas(
    FunctionNode& function
)
{
    std::map<std::string, AltstackArrayExportSchema> arrayBindings;
    const SourceLocation functionLocation = getNodeLocation(function);
    for (const auto& parameter : function.parameters) {
        auto arrayType = parseFixedArrayType(parameter.type);
        if (!arrayType.has_value()) {
            continue;
        }
        const auto& [elementType, arraySize] = arrayType.value();
        arrayBindings[parameter.name] = AltstackArrayExportSchema{
            elementType,
            arraySize,
            calculateElementStackSize(elementType),
            functionLocation
        };
    }

    if (function.block) {
        collectAltstackArrayExportSchemas(*function.block, arrayBindings);
    }
}

void PreAnalysisVisitor::collectAltstackArrayExportSchemas(
    BlockNode& block,
    std::map<std::string, AltstackArrayExportSchema>& arrayBindings
)
{
    for (auto& statement : block.statements) {
        if (statement) {
            collectAltstackArrayExportSchemas(*statement, arrayBindings);
        }
    }
}

void PreAnalysisVisitor::collectAltstackArrayExportSchemas(
    StmtNode& statement,
    std::map<std::string, AltstackArrayExportSchema>& arrayBindings
)
{
    if (auto* block = dynamic_cast<BlockNode*>(&statement)) {
        auto nestedBindings = arrayBindings;
        collectAltstackArrayExportSchemas(*block, nestedBindings);
        return;
    }

    if (auto* declaration = dynamic_cast<VarDeclNode*>(&statement)) {
        if (auto arrayType = parseFixedArrayType(declaration->type);
            arrayType.has_value()) {
            const auto& [elementType, arraySize] = arrayType.value();
            arrayBindings[declaration->name] = AltstackArrayExportSchema{
                elementType,
                arraySize,
                calculateElementStackSize(elementType),
                getNodeLocation(*declaration)
            };
        }
        if (declaration->initValue) {
            collectAltstackArrayExportSchemas(
                *declaration->initValue, arrayBindings
            );
        }
        return;
    }

    if (auto* declaration = dynamic_cast<ArrayDeclNode*>(&statement)) {
        std::optional<size_t> arraySize;
        if (declaration->initArray) {
            arraySize = declaration->initArray->elements.size();
        } else if (declaration->sizeExpr) {
            auto value = evaluateIntegerConstant(*declaration->sizeExpr);
            if (value.has_value() && value.value() >= 0) {
                arraySize = static_cast<size_t>(value.value());
            }
        }
        if (arraySize.has_value()) {
            arrayBindings[declaration->name] = AltstackArrayExportSchema{
                declaration->elementType,
                arraySize.value(),
                calculateElementStackSize(declaration->elementType),
                getNodeLocation(*declaration)
            };
        }
        if (declaration->sizeExpr) {
            collectAltstackArrayExportSchemas(
                *declaration->sizeExpr, arrayBindings
            );
        }
        if (declaration->initArray) {
            collectAltstackArrayExportSchemas(
                *declaration->initArray, arrayBindings
            );
        }
        return;
    }

    if (auto* expressionStatement = dynamic_cast<ExprStmtNode*>(&statement)) {
        if (expressionStatement->expr) {
            collectAltstackArrayExportSchemas(
                *expressionStatement->expr, arrayBindings
            );
        }
        return;
    }

    if (auto* assignment = dynamic_cast<AssignNode*>(&statement)) {
        if (assignment->value) {
            collectAltstackArrayExportSchemas(
                *assignment->value, arrayBindings
            );
        }
        if (assignment->name) {
            collectAltstackArrayExportSchemas(
                *assignment->name, arrayBindings
            );
        }
        return;
    }

    if (auto* destructure =
            dynamic_cast<DestructureAssignNode*>(&statement)) {
        if (destructure->value) {
            collectAltstackArrayExportSchemas(
                *destructure->value, arrayBindings
            );
        }
        return;
    }

    if (auto* branch = dynamic_cast<IfNode*>(&statement)) {
        if (branch->condition) {
            collectAltstackArrayExportSchemas(
                *branch->condition, arrayBindings
            );
        }
        if (branch->thenBranch) {
            auto thenBindings = arrayBindings;
            collectAltstackArrayExportSchemas(
                *branch->thenBranch, thenBindings
            );
        }
        if (branch->elseBranch) {
            auto elseBindings = arrayBindings;
            collectAltstackArrayExportSchemas(
                *branch->elseBranch, elseBindings
            );
        }
        return;
    }

    if (auto* loop = dynamic_cast<ForNode*>(&statement)) {
        if (loop->iterable) {
            collectAltstackArrayExportSchemas(
                *loop->iterable, arrayBindings
            );
        }
        if (loop->body) {
            auto loopBindings = arrayBindings;
            collectAltstackArrayExportSchemas(*loop->body, loopBindings);
        }
        return;
    }

    if (auto* returnNode = dynamic_cast<ReturnNode*>(&statement)) {
        if (returnNode->expr) {
            collectAltstackArrayExportSchemas(
                *returnNode->expr, arrayBindings
            );
        }
    }
}

void PreAnalysisVisitor::collectAltstackArrayExportSchemas(
    ExprNode& expression,
    const std::map<std::string, AltstackArrayExportSchema>& arrayBindings
)
{
    if (auto* call = dynamic_cast<CallNode*>(&expression)) {
        if (call->funcName == "SetAlt" && call->args.size() == 1) {
            auto* index =
                dynamic_cast<IndexAccessNode*>(call->args[0].get());
            const std::string arrayName =
                index && index->base
                    ? getVariableFromExpr(*index->base)
                    : std::string();
            auto binding = arrayBindings.find(arrayName);
            if (binding != arrayBindings.end()) {
                registerAltstackArrayExportSchema(
                    arrayName,
                    binding->second,
                    getNodeLocation(*call->args[0])
                );
            }
        }
        for (auto& argument : call->args) {
            if (argument) {
                collectAltstackArrayExportSchemas(
                    *argument, arrayBindings
                );
            }
        }
        return;
    }

    if (auto* method = dynamic_cast<MethodCallNode*>(&expression)) {
        if (method->object) {
            collectAltstackArrayExportSchemas(
                *method->object, arrayBindings
            );
        }
        for (auto& argument : method->args) {
            if (argument) {
                collectAltstackArrayExportSchemas(
                    *argument, arrayBindings
                );
            }
        }
        return;
    }

    if (auto* operation = dynamic_cast<OpNode*>(&expression)) {
        if (operation->lhs) {
            collectAltstackArrayExportSchemas(
                *operation->lhs, arrayBindings
            );
        }
        if (operation->rhs) {
            collectAltstackArrayExportSchemas(
                *operation->rhs, arrayBindings
            );
        }
        return;
    }

    if (auto* field = dynamic_cast<FieldAccessNode*>(&expression)) {
        if (field->base) {
            collectAltstackArrayExportSchemas(*field->base, arrayBindings);
        }
        return;
    }

    if (auto* index = dynamic_cast<IndexAccessNode*>(&expression)) {
        if (index->base) {
            collectAltstackArrayExportSchemas(*index->base, arrayBindings);
        }
        if (index->index) {
            collectAltstackArrayExportSchemas(*index->index, arrayBindings);
        }
        return;
    }

    if (auto* array = dynamic_cast<ArrayDefNode*>(&expression)) {
        for (auto& element : array->elements) {
            if (element) {
                collectAltstackArrayExportSchemas(
                    *element, arrayBindings
                );
            }
        }
        return;
    }

    if (auto* brace = dynamic_cast<BraceExprNode*>(&expression)) {
        for (auto& element : brace->elements) {
            if (element) {
                collectAltstackArrayExportSchemas(
                    *element, arrayBindings
                );
            }
        }
    }
}

void PreAnalysisVisitor::registerAltstackArrayExportSchema(
    const std::string& channelName,
    const AltstackArrayExportSchema& schema,
    const SourceLocation& exportLocation
)
{
    auto [existing, inserted] =
        m_altstackArrayExportSchemas.emplace(channelName, schema);
    if (inserted) {
        existing->second.firstExportLocation = exportLocation;
        return;
    }

    const AltstackArrayExportSchema& previous = existing->second;
    if (previous.elementType == schema.elementType &&
        previous.arraySize == schema.arraySize &&
        previous.elementStackSize == schema.elementStackSize) {
        return;
    }

    if (m_conflictingAltstackArrayExportSchemas.insert(channelName).second) {
        LOG_DEBUG(
            "Recorded conflicting altstack export schemas for '" +
            channelName + "': " + formatAltstackArraySchema(previous) +
            " vs " + formatAltstackArraySchema(schema) +
            "; the conflict is diagnosed only if a consumer imports this "
            "channel"
        );
    }
}

bool PreAnalysisVisitor::restoreAltstackArrayElement(IndexAccessNode& node)
{
    // Publish only named fixed-array channels. Computed bases such as
    // box.values[index] continue through IndexAccessNode's existing checks.
    if (!node.base ||
        dynamic_cast<IdentifierNode*>(node.base.get()) == nullptr) {
        return false;
    }

    const std::string arrayName = getVariableFromExpr(*node.base);
    if (arrayName.empty()) {
        return false;
    }

    VariableInfo* array = findVariable(arrayName);
    bool materializedExternalView = false;
    if (!array) {
        if (!m_allowSubscopeAltstack) {
            return false;
        }

        if (m_conflictingAltstackArrayExportSchemas.count(arrayName) != 0) {
            reportError(
                "Conflicting altstack export schemas for channel '" +
                    arrayName + "'",
                getNodeLocation(node)
            );
            return true;
        }

        auto schema = m_altstackArrayExportSchemas.find(arrayName);
        if (schema == m_altstackArrayExportSchemas.end()) {
            return false;
        }

        declareArrayVariable(
            arrayName,
            schema->second.elementType,
            getNodeLocation(node),
            schema->second.arraySize,
            schema->second.elementStackSize,
            StorageResidency::UNBOUND
        );
        array = findVariable(arrayName);
        if (!array) {
            return true;
        }

        // The schema makes the logical type visible, but no element becomes
        // usable until its own SetMain restores the physical group.
        std::fill(
            array->elementOwnership.begin(),
            array->elementOwnership.end(),
            false
        );
        std::fill(
            array->elementStorage.begin(),
            array->elementStorage.end(),
            StorageResidency::UNBOUND
        );
        array->storage = StorageResidency::UNBOUND;
        materializedExternalView = true;
    }

    if (!array->isArrayType()) {
        reportError(
            "SetMain() target '" + arrayName + "' is not a fixed array",
            getNodeLocation(node)
        );
        return true;
    }

    if (node.index) {
        analyzeBorrowedExpression(*node.index);
    }
    auto index = calculateIndexValue(node.index.get());
    if (!index.has_value()) {
        if (node.index) {
            auto constant = evaluateIntegerConstant(*node.index);
            if (constant.has_value()) {
                reportError(
                    "Array index " + std::to_string(constant.value()) +
                        " is out of bounds for '" + arrayName +
                        "' of length " +
                        std::to_string(array->getArraySize()),
                    getNodeLocation(node)
                );
                return true;
            }
        }

        if (array->elementStackSize != 1) {
            reportError(
                "SetMain() requires a compile-time index for multi-slot "
                "array '" +
                    arrayName + "'; each element occupies " +
                    std::to_string(array->elementStackSize) +
                    " stack slots",
                getNodeLocation(node)
            );
            return true;
        }
        if (materializedExternalView) {
            reportError(
                "Cross-function SetMain() requires a compile-time array "
                "index for '" +
                    arrayName + "'",
                getNodeLocation(node)
            );
            return true;
        }
        return false;
    }

    if (index.value() >= array->getArraySize()) {
        reportError(
            "Array index " + std::to_string(index.value()) +
                " is out of bounds for '" + arrayName + "' of length " +
                std::to_string(array->getArraySize()),
            getNodeLocation(node)
        );
        return true;
    }

    array->elementOwnership[index.value()] = true;
    array->elementStorage[index.value()] = StorageResidency::MAIN_STACK;
    array->state = VariableState::DECLARED;
    array->lastUseLocation = getNodeLocation(node);
    LOG_DEBUG(
        "Array element '" + arrayName + "[" +
        std::to_string(index.value()) +
        "]' restored from altstack to main stack"
    );
    return true;
}

std::string PreAnalysisVisitor::formatAltstackArraySchema(
    const AltstackArrayExportSchema& schema
) const
{
    return schema.elementType + "[" + std::to_string(schema.arraySize) +
           "] (" + std::to_string(schema.elementStackSize) +
           " stack slots per element)";
}

VariableInfo* PreAnalysisVisitor::findVariable(const std::string& name)
{
    auto it = m_variables.find(name);
    return (it != m_variables.end()) ? &it->second : nullptr;
}

void PreAnalysisVisitor::reportError(
    const std::string& message,
    const SourceLocation& location
)
{
    m_errors.push_back(message);
    m_hasErrors = true;

    ErrorManager::getInstance().semanticError(message, location);
    LOG_ERROR("Pre-analysis error: " + message);
}

void PreAnalysisVisitor::reportWarning(
    const std::string& message,
    const SourceLocation& location
)
{
    m_warnings.push_back(message);

    ErrorManager::getInstance().warning(message, location);
    LOG_WARNING("Pre-analysis warning: " + message);
}

void PreAnalysisVisitor::analyzeExpression(ExprNode& expr)
{
    validateValueProducingExpression(expr);
    expr.accept(*this);
}

void PreAnalysisVisitor::validateValueProducingExpression(ExprNode& expr)
{
    if (!m_validatedValueExpressions.insert(&expr).second) {
        return;
    }

    if (auto* call = dynamic_cast<CallNode*>(&expr)) {
        if (call->funcName == "Verify") {
            reportError(
                "Verify() does not return a value and cannot be used in a "
                "value context",
                getNodeLocation(expr)
            );
        }
        for (auto& arg : call->args) {
            if (arg) {
                validateValueProducingExpression(*arg);
            }
        }
        return;
    }

    if (auto* method = dynamic_cast<MethodCallNode*>(&expr)) {
        if (method->object) {
            validateValueProducingExpression(*method->object);
        }
        for (auto& arg : method->args) {
            if (arg) {
                validateValueProducingExpression(*arg);
            }
        }
        return;
    }

    if (auto* op = dynamic_cast<OpNode*>(&expr)) {
        if (op->lhs) {
            validateValueProducingExpression(*op->lhs);
        }
        if (op->rhs) {
            validateValueProducingExpression(*op->rhs);
        }
        return;
    }

    if (auto* field = dynamic_cast<FieldAccessNode*>(&expr)) {
        if (field->base) {
            validateValueProducingExpression(*field->base);
        }
        return;
    }

    if (auto* index = dynamic_cast<IndexAccessNode*>(&expr)) {
        if (index->base) {
            validateValueProducingExpression(*index->base);
        }
        if (index->index) {
            validateValueProducingExpression(*index->index);
        }
        return;
    }

    if (auto* array = dynamic_cast<ArrayDefNode*>(&expr)) {
        for (auto& element : array->elements) {
            if (element) {
                validateValueProducingExpression(*element);
            }
        }
        return;
    }

    if (auto* brace = dynamic_cast<BraceExprNode*>(&expr)) {
        for (auto& element : brace->elements) {
            if (element) {
                validateValueProducingExpression(*element);
            }
        }
    }
}

void PreAnalysisVisitor::analyzeBorrowedExpression(ExprNode& expr)
{
    if (auto* identNode = dynamic_cast<IdentifierNode*>(&expr)) {
        borrowVariable(identNode->name, getNodeLocation(*identNode));
        return;
    }

    if (auto* fieldNode = dynamic_cast<FieldAccessNode*>(&expr)) {
        auto [varName, fieldPath] = getFieldPathFromExpr(*fieldNode);
        if (!varName.empty()) {
            if (fieldPath.empty()) {
                borrowVariable(varName, getNodeLocation(*fieldNode));
            } else {
                borrowField(varName, fieldPath, getNodeLocation(*fieldNode));
            }
            return;
        }
    }

    if (auto* indexNode = dynamic_cast<IndexAccessNode*>(&expr)) {
        borrowArrayElement(*indexNode);
        return;
    }

    // Expressions that produce temporaries still analyze their inputs using
    // normal ownership rules; only the resulting temporary is preserved by
    // OP_SIZE.
    analyzeExpression(expr);
}

StorageResidency PreAnalysisVisitor::classifyStorage(ExprNode* value)
{
    if (!value) {
        return StorageResidency::UNBOUND;
    }
    // Keep the ownership model aligned with integer propagation and backend
    // folding. An expression such as `j + 1` has no runtime stack effect when
    // all of its operands are statically known, even though its AST root is an
    // OpNode rather than a LiteralNode.
    if (isConstantValue(*value) ||
        evaluateIntegerConstant(*value).has_value()) {
        return StorageResidency::FIXED_VALUE;
    }

    if (auto* identifier = dynamic_cast<IdentifierNode*>(value)) {
        VariableInfo* var = findVariable(identifier->name);
        return var ? var->storage : StorageResidency::MAIN_STACK;
    }

    if (auto* field = dynamic_cast<FieldAccessNode*>(value)) {
        auto [varName, fieldPath] = getFieldPathFromExpr(*field);
        VariableInfo* var = varName.empty() ? nullptr : findVariable(varName);
        return var ? var->getFieldStorage(fieldPath)
                   : StorageResidency::MAIN_STACK;
    }

    if (auto* index = dynamic_cast<IndexAccessNode*>(value)) {
        std::string arrayName = index->base
                                    ? getVariableFromExpr(*index->base)
                                    : std::string();
        VariableInfo* var =
            arrayName.empty() ? nullptr : findVariable(arrayName);
        auto indexValue = calculateIndexValue(index->index.get());
        if (var && indexValue.has_value()) {
            return var->getElementStorage(indexValue.value());
        }
    }

    // Calls, operators and method calls materialize a runtime value.
    return StorageResidency::MAIN_STACK;
}

bool PreAnalysisVisitor::expressionHasMainStackSlot(ExprNode& expr)
{
    return classifyStorage(&expr) == StorageResidency::MAIN_STACK;
}

bool PreAnalysisVisitor::assignmentTargetHasMainStackSlot(ExprNode& target)
{
    if (auto* identifier = dynamic_cast<IdentifierNode*>(&target)) {
        VariableInfo* var = findVariable(identifier->name);
        return var && var->state == VariableState::DECLARED &&
               var->storage == StorageResidency::MAIN_STACK;
    }

    if (auto* field = dynamic_cast<FieldAccessNode*>(&target)) {
        auto [varName, fieldPath] = getFieldPathFromExpr(*field);
        VariableInfo* var = varName.empty() ? nullptr : findVariable(varName);
        return var && var->getFieldState(fieldPath) == VariableState::DECLARED &&
               var->getFieldStorage(fieldPath) == StorageResidency::MAIN_STACK;
    }

    if (auto* index = dynamic_cast<IndexAccessNode*>(&target)) {
        std::string arrayName = index->base
                                    ? getVariableFromExpr(*index->base)
                                    : std::string();
        VariableInfo* var =
            arrayName.empty() ? nullptr : findVariable(arrayName);
        auto indexValue = calculateIndexValue(index->index.get());
        return var && indexValue.has_value() &&
               var->isElementAvailable(indexValue.value()) &&
               var->getElementStorage(indexValue.value()) ==
                   StorageResidency::MAIN_STACK;
    }

    return false;
}

void PreAnalysisVisitor::bindAssignmentTarget(
    ExprNode& target,
    StorageResidency storage,
    ExprNode* value
)
{
    const SourceLocation location = getNodeLocation(target);

    if (auto* identifier = dynamic_cast<IdentifierNode*>(&target)) {
        VariableInfo* existing = findVariable(identifier->name);
        if (existing) {
            reassignVariable(identifier->name, location, storage);
        } else {
            // `target = source` is a zero-cost whole-array identity transfer
            // in bytecode generation. Mirror that shape here instead of
            // accidentally auto-declaring a scalar target.
            auto* sourceIdentifier =
                value ? dynamic_cast<IdentifierNode*>(value) : nullptr;
            VariableInfo* source = sourceIdentifier
                                       ? findVariable(sourceIdentifier->name)
                                       : nullptr;
            if (source && source->isArrayType()) {
                VariableInfo transferred = *source;
                transferred.name = identifier->name;
                transferred.state = VariableState::DECLARED;
                transferred.declLocation = location;
                transferred.lastUseLocation = location;
                m_variables.emplace(identifier->name, std::move(transferred));
                LOG_DEBUG(
                    "Transferred array analysis identity from " +
                    sourceIdentifier->name + " to " + identifier->name
                );
                return;
            }
            declareVariable(
                identifier->name, "auto", location, value, storage
            );
            LOG_DEBUG(
                "Auto-declared variable in assignment: " + identifier->name
            );
        }
        return;
    }

    if (auto* field = dynamic_cast<FieldAccessNode*>(&target)) {
        auto [varName, fieldPath] = getFieldPathFromExpr(*field);
        if (isContractMember(varName) || isBuiltinObject(varName)) {
            return;
        }

        VariableInfo* var = varName.empty() ? nullptr : findVariable(varName);
        if (!var) {
            reportError("Undeclared variable: '" + varName + "'", location);
            return;
        }
        if (fieldPath.empty()) {
            reportError("Invalid field assignment target", location);
            return;
        }

        var->fieldOwnership[fieldPath] = VariableState::DECLARED;
        var->fieldStorage[fieldPath] = storage;
        var->lastUseLocation = location;
        return;
    }

    if (auto* index = dynamic_cast<IndexAccessNode*>(&target)) {
        if (index->index) {
            analyzeExpression(*index->index);
        }

        std::string arrayName = index->base
                                    ? getVariableFromExpr(*index->base)
                                    : std::string();
        VariableInfo* var =
            arrayName.empty() ? nullptr : findVariable(arrayName);
        if (!var || !var->isArrayType()) {
            reportError("Invalid array assignment target", location);
            return;
        }

        auto indexValue = calculateIndexValue(index->index.get());
        if (!indexValue.has_value() ||
            indexValue.value() >= var->elementOwnership.size()) {
            reportError("Invalid array assignment index", location);
            return;
        }

        const size_t elementIndex = indexValue.value();
        var->elementOwnership[elementIndex] = true;
        var->elementStorage[elementIndex] = storage;
        if (var->state == VariableState::CONSUMED) {
            var->state = VariableState::DECLARED;
        }
        var->lastUseLocation = location;
        return;
    }

    reportError("Invalid assignment target", location);
}

// 小写 return 专用: 表达式中变量只使用不消耗.
void PreAnalysisVisitor::analyzeExpressionForValueReturn(ExprNode& expr)
{
    validateValueProducingExpression(expr);

    if (auto* identNode = dynamic_cast<IdentifierNode*>(&expr)) {
        borrowVariable(identNode->name, getNodeLocation(*identNode));
        return;
    }

    if (auto* braceExpr = dynamic_cast<BraceExprNode*>(&expr)) {
        for (const auto& element : braceExpr->elements) {
            if (element) {
                analyzeExpressionForValueReturn(*element);
            }
        }
        return;
    }

    if (auto* fieldNode = dynamic_cast<FieldAccessNode*>(&expr)) {
        auto [varName, fieldPath] = getFieldPathFromExpr(*fieldNode);
        if (!varName.empty()) {
            if (fieldPath.empty()) {
                borrowVariable(varName, getNodeLocation(*fieldNode));
            } else {
                borrowField(varName, fieldPath, getNodeLocation(*fieldNode));
            }
            return;
        }
        return;
    }

    if (auto* indexNode = dynamic_cast<IndexAccessNode*>(&expr)) {
        borrowArrayElement(*indexNode);
        return;
    }

    // 函数调用: 参数按调用语义消耗, 但返回值表达式中的变量不消耗.
    if (auto* callNode = dynamic_cast<CallNode*>(&expr)) {
        for (auto& arg : callNode->args) {
            analyzeExpression(*arg);
        }
        if (isConsumingOperation(callNode->funcName)) {
            for (auto& arg : callNode->args) {
                auto [varName, fieldPath] = getFieldPathFromExpr(*arg);
                if (!varName.empty()) {
                    if (fieldPath.empty()) {
                        consumeVariable(varName, getNodeLocation(*arg));
                    } else {
                        consumeField(varName, fieldPath, getNodeLocation(*arg));
                    }
                }
            }
        }
        return;
    }

    // 方法调用: 对象只使用, 参数照常分析.
    if (auto* methodNode = dynamic_cast<MethodCallNode*>(&expr)) {
        if (methodNode->object) {
            analyzeExpressionForValueReturn(*methodNode->object);
        }
        for (auto& arg : methodNode->args) {
            analyzeExpression(*arg);
        }
        return;
    }

    // 操作符表达式: 两侧操作数只使用不消耗.
    if (auto* opNode = dynamic_cast<OpNode*>(&expr)) {
        if (opNode->lhs) {
            analyzeExpressionForValueReturn(*opNode->lhs);
        }
        if (opNode->rhs) {
            analyzeExpressionForValueReturn(*opNode->rhs);
        }
        return;
    }

    if (dynamic_cast<LiteralNode*>(&expr)) {
        return;
    }

    LOG_DEBUG("Unhandled expression type in analyzeExpressionForValueReturn, "
              "falling back to normal analysis");
    analyzeExpression(expr);
}

void PreAnalysisVisitor::analyzeConditionalExpression(ExprNode& expr)
{
    // if 条件消耗栈顶值: 标识符消耗变量本身, 复杂表达式消耗其结果.
    if (auto* identNode = dynamic_cast<IdentifierNode*>(&expr)) {
        std::string varName = identNode->name;

        if (isContractMember(varName) || isBuiltinObject(varName)) {
            LOG_DEBUG(
                "Using contract member/builtin object in if condition "
                "(no ownership check): " +
                varName
            );
            return;
        }

        consumeVariable(varName, getNodeLocation(expr));
        LOG_DEBUG("If condition consumes variable: " + varName);
    } else {
        analyzeExpression(expr);
    }
}

std::string PreAnalysisVisitor::getVariableFromExpr(ExprNode& expr)
{
    if (auto* identNode = dynamic_cast<IdentifierNode*>(&expr)) {
        return identNode->name;
    }

    if (auto* fieldNode = dynamic_cast<FieldAccessNode*>(&expr)) {
        return getVariableFromExpr(*fieldNode->base);
    }

    return "";
}

// 例: ctx.FTbyChange.Tape.LockingScript -> {"ctx", "FTbyChange.Tape.LockingScript"}
std::pair<std::string, std::string> PreAnalysisVisitor::getFieldPathFromExpr(
    ExprNode& expr
)
{
    std::vector<std::string> pathParts;
    ExprNode* current = &expr;

    while (current) {
        if (auto* fieldNode = dynamic_cast<FieldAccessNode*>(current)) {
            pathParts.insert(pathParts.begin(), fieldNode->field);
            current = fieldNode->base.get();
        } else if (auto* identNode = dynamic_cast<IdentifierNode*>(current)) {
            std::string varName = identNode->name;

            if (pathParts.empty()) {
                return {varName, ""};
            }

            std::string fieldPath;
            for (size_t i = 0; i < pathParts.size(); ++i) {
                if (i > 0)
                    fieldPath += ".";
                fieldPath += pathParts[i];
            }

            return {varName, fieldPath};
        } else {
            break;
        }
    }

    return {"", ""};
}

bool PreAnalysisVisitor::isConsumingOperation(const std::string& operation)
{
    return m_consumingOperations.find(operation) != m_consumingOperations.end();
}

void PreAnalysisVisitor::checkOwnershipViolation(
    const std::string& varName,
    const SourceLocation& location
)
{
    VariableInfo* var = findVariable(varName);
    if (!var || !var->hasOwnership()) {
        return;
    }

    if (var->state == VariableState::CONSUMED) {
        reportError(
            "Variable '" + varName +
                "' has been consumed and cannot be used again",
            location
        );
    }
}

void PreAnalysisVisitor::checkUnusedVariables()
{
    for (const auto& varPair : m_variables) {
        const VariableInfo& var = varPair.second;

        if (var.state == VariableState::DECLARED && var.hasOwnership() &&
            !var.wasBorrowed) {
            reportWarning(
                "Variable '" + var.name + "' declared but not used",
                var.declLocation
            );
        }
    }
}

SourceLocation PreAnalysisVisitor::getNodeLocation(ASTNode& node)
{
    if (node.hasSourceLocation()) {
        return node.sourceLocation;
    }
    return SourceLocation("", node.pos.first, node.pos.second);
}

bool PreAnalysisVisitor::isConstantValue(ExprNode& expr)
{
    return dynamic_cast<LiteralNode*>(&expr) != nullptr;
}

bool PreAnalysisVisitor::isContractMember(const std::string& varName)
{
    // Check for self.* or <self.*> patterns
    if (varName.length() > 5 && varName.substr(0, 5) == "self.") {
        return true;
    }

    if (varName.length() > 7 && varName.substr(0, 6) == "<self." &&
        varName.back() == '>') {
        return true;
    }

    return false;
}

bool PreAnalysisVisitor::isBuiltinObject(const std::string& varName)
{
    // Check for builtin objects like BVM, self
    if (varName == "BVM" || varName == "self" || varName == "<self>") {
        return true;
    }

    // Check for builtin object field access
    if (varName.length() > 4) {
        std::string prefix = varName.substr(0, 4);
        if (prefix == "BVM.") {
            return true;
        }
    }

    return false;
}

bool PreAnalysisVisitor::hasReturnValue(
    const std::string& functionName,
    size_t argCount
)
{
    // Check OpFunction (operation functions like Hash160, CheckSig, etc.)
    auto opFunc =
        tbc::OpFunctionFactory::createFunction(functionName, argCount);
    if (opFunc && opFunc->getReturnCount() > 0) {
        return true;
    }

    // Check BuiltinFunction (builtin functions like clone, slice, etc.)
    auto builtinFunc =
        tbc::BuiltinFunctionFactory::createFunction(functionName, argCount);
    if (builtinFunc && builtinFunc->getReturnCount() > 0) {
        return true;
    }

    return false;
}

void PreAnalysisVisitor::checkUnusedFunctionResult(
    ExprNode& expr,
    const SourceLocation& location
)
{
    // Check if this is a CallNode (function call)
    if (auto* callNode = dynamic_cast<CallNode*>(&expr)) {
        if (hasReturnValue(callNode->funcName, callNode->args.size())) {
            reportWarning(
                "Function '" + callNode->funcName +
                    "' returns a value but the result is not used",
                location
            );
            LOG_DEBUG(
                "Unused function result warning for: " + callNode->funcName
            );
        }
    }
    // Check if this is a MethodCallNode (method call)
    else if (auto* methodNode = dynamic_cast<MethodCallNode*>(&expr)) {
        if (hasReturnValue(methodNode->methodName, methodNode->args.size())) {
            reportWarning(
                "Method '" + methodNode->methodName +
                    "' returns a value but the result is not used",
                location
            );
            LOG_DEBUG(
                "Unused method result warning for: " + methodNode->methodName
            );
        }
    }
}

std::map<std::string, VariableInfo> PreAnalysisVisitor::saveVariableState()
{
    return m_variables;
}

void PreAnalysisVisitor::restoreVariableState(
    const std::map<std::string, VariableInfo>& savedState
)
{
    m_variables = savedState;
}

void PreAnalysisVisitor::mergeBranchStates(
    const std::map<std::string, VariableInfo>& thenState,
    const std::map<std::string, VariableInfo>& elseState,
    const std::map<std::string, VariableInfo>& entryState
)
{
    // Start with a copy of the then state
    m_variables = thenState;

    // Create a set of all variables that exist in either branch
    std::set<std::string> allVariables;
    for (const auto& varPair : thenState) {
        allVariables.insert(varPair.first);
    }
    for (const auto& varPair : elseState) {
        allVariables.insert(varPair.first);
    }

    // For each variable that exists in either branch, determine the merged
    // state
    for (const std::string& varName : allVariables) {
        auto thenIt = thenState.find(varName);
        auto elseIt = elseState.find(varName);

        // Skip variables that only exist in one branch (branch-local variables)
        if (thenIt == thenState.end() || elseIt == elseState.end()) {
            if (thenIt == thenState.end()) {
                LOG_DEBUG(
                    "Variable '" + varName +
                    "' only exists in else branch, not adding to merged state"
                );
            }
            // Variables only in then branch are already in m_variables, no
            // action needed
            continue;
        }

        const VariableInfo& thenVar = thenIt->second;
        const VariableInfo& elseVar = elseIt->second;

        // Merge logic for stack variables with ownership
        if (thenVar.hasOwnership()) {
            // Find the variable in m_variables (should exist since we copied
            // thenState)
            auto varIt = m_variables.find(varName);
            if (varIt != m_variables.end()) {
                const auto entryIt = entryState.find(varName);
                const VariableInfo* entryVar =
                    entryIt == entryState.end() ? nullptr : &entryIt->second;
                varIt->second.wasBorrowed =
                    thenVar.wasBorrowed || elseVar.wasBorrowed;
                varIt->second.storage =
                    thenVar.storage == elseVar.storage
                        ? thenVar.storage
                        : (entryVar ? entryVar->storage
                                    : StorageResidency::UNBOUND);

                if (thenVar.isArrayType() && elseVar.isArrayType()) {
                    auto& mergedOwnership =
                        varIt->second.elementOwnership;
                    auto& mergedStorage = varIt->second.elementStorage;
                    for (size_t i = 0; i < mergedOwnership.size(); ++i) {
                        bool ownedInElse =
                            i < elseVar.elementOwnership.size() &&
                            elseVar.elementOwnership[i];
                        mergedOwnership[i] =
                            thenVar.elementOwnership[i] && ownedInElse;
                        const StorageResidency thenStorage =
                            thenVar.getElementStorage(i);
                        const StorageResidency elseStorage =
                            elseVar.getElementStorage(i);
                        mergedStorage[i] =
                            !mergedOwnership[i]
                                ? StorageResidency::UNBOUND
                                : (thenStorage == elseStorage
                                       ? thenStorage
                                       : (entryVar
                                              ? entryVar->getElementStorage(i)
                                              : StorageResidency::UNBOUND));
                    }
                }

                // CRITICAL: consumed in ANY branch -> consumed after if/else
                // (ensures stack state consistency across all paths)
                if (thenVar.state == VariableState::CONSUMED ||
                    elseVar.state == VariableState::CONSUMED) {
                    varIt->second.state = VariableState::CONSUMED;

                    // Use the location from whichever branch consumed it
                    if (thenVar.state == VariableState::CONSUMED) {
                        varIt->second.lastUseLocation = thenVar.lastUseLocation;
                    } else {
                        varIt->second.lastUseLocation = elseVar.lastUseLocation;
                    }

                    LOG_DEBUG(
                        "Variable '" + varName +
                        "' consumed in at least one branch, marking as "
                        "consumed for stack consistency"
                    );
                }
                // Used in any branch -> mark as used (unless already consumed)
                else if (thenVar.state == VariableState::USED ||
                         elseVar.state == VariableState::USED) {
                    varIt->second.state = VariableState::USED;

                    // Use the most recent use location
                    if (thenVar.state == VariableState::USED) {
                        varIt->second.lastUseLocation = thenVar.lastUseLocation;
                    } else if (elseVar.state == VariableState::USED) {
                        varIt->second.lastUseLocation = elseVar.lastUseLocation;
                    }
                }

                if (varIt->second.isArrayType() &&
                    varIt->second.isFullyConsumed()) {
                    std::fill(
                        varIt->second.elementOwnership.begin(),
                        varIt->second.elementOwnership.end(),
                        false
                    );
                    varIt->second.state = VariableState::CONSUMED;
                }

                // 字段级合并: 任一分支消耗则消耗, 否则任一使用则使用.
                std::set<std::string> allFields;
                for (const auto& fieldPair : thenVar.fieldOwnership) {
                    allFields.insert(fieldPair.first);
                }
                for (const auto& fieldPair : elseVar.fieldOwnership) {
                    allFields.insert(fieldPair.first);
                }

                for (const std::string& fieldPath : allFields) {
                    VariableState thenFieldState = thenVar.getFieldState(
                        fieldPath
                    );
                    VariableState elseFieldState = elseVar.getFieldState(
                        fieldPath
                    );

                    if (thenFieldState == VariableState::CONSUMED ||
                        elseFieldState == VariableState::CONSUMED) {
                        varIt->second.markFieldConsumed(fieldPath);
                        varIt->second.fieldStorage[fieldPath] =
                            StorageResidency::UNBOUND;
                        LOG_DEBUG(
                            "Field '" + varName + "." + fieldPath +
                            "' consumed in at least one branch, marking as "
                            "consumed"
                        );
                    }
                    else if (thenFieldState == VariableState::USED ||
                             elseFieldState == VariableState::USED) {
                        varIt->second.markFieldUsed(fieldPath);
                        varIt->second.fieldStorage[fieldPath] =
                            StorageResidency::UNBOUND;
                    } else {
                        const StorageResidency thenStorage =
                            thenVar.getFieldStorage(fieldPath);
                        const StorageResidency elseStorage =
                            elseVar.getFieldStorage(fieldPath);
                        varIt->second.fieldStorage[fieldPath] =
                            thenStorage == elseStorage
                                ? thenStorage
                                : (entryVar
                                       ? entryVar->getFieldStorage(fieldPath)
                                       : StorageResidency::UNBOUND);
                    }
                }

                if (varIt->second.state != VariableState::DECLARED) {
                    varIt->second.storage = StorageResidency::UNBOUND;
                }
            }
        }
    }
}

void PreAnalysisVisitor::reassignVariable(
    const std::string& name,
    const SourceLocation& location,
    StorageResidency storage
)
{
    // Check if it's a contract member or builtin object first
    if (isContractMember(name) || isBuiltinObject(name)) {
        LOG_DEBUG(
            "Reassigning contract member/builtin object (no ownership "
            "check): " +
            name
        );
        return;
    }

    VariableInfo* var = findVariable(name);
    if (!var) {
        reportError("Undeclared variable: '" + name + "'", location);
        return;
    }

    // Rust-like move-after-move: reassignment allowed even if previously
    // consumed/used. 重新绑定的新值视为可消费一次 (state = DECLARED), 这样
    // `a = a + 1; Keep(a)` 这类合法序列里 Keep(a) 仍可读 a.
    var->state = VariableState::DECLARED;
    var->wasBorrowed = false;
    var->storage = storage;
    var->source = storage == StorageResidency::FIXED_VALUE
                      ? DataSource::CONSTANT_VALUE
                      : DataSource::STACK_DATA;
    var->lastUseLocation = location;

    LOG_DEBUG(
        "Reassigning stack variable: " + name +
        " (previous state reset, variable now available for use)"
    );
}

// Altstack operation check related method implementations
bool PreAnalysisVisitor::isInSubscope() const
{
    return m_inIfElseScope || m_inPrivateFunction;
}

bool PreAnalysisVisitor::isPrivateFunction(const std::string& functionName
) const
{
    return !functionName.empty() && functionName[0] == '_';
}

bool PreAnalysisVisitor::isAltstackOperation(const std::string& functionName
) const
{
    return functionName == "SetAlt" || functionName == "SetMain";
}

void PreAnalysisVisitor::checkAltstackOperationAllowed(
    const std::string& functionName,
    const SourceLocation& location
)
{
    if (isAltstackOperation(functionName) && isInSubscope() &&
        !m_allowSubscopeAltstack) {
        std::string scopeType = m_inIfElseScope ? "if/else block"
                                                : "private function";
        std::string errorMsg =
            "Altstack operation '" + functionName + "' is not allowed in " +
            scopeType +
            ". Use --allow-subscope-altstack or --asa to enable this feature.";

        reportError(errorMsg, location);
        LOG_ERROR("Altstack operation check failed: " + errorMsg);
    }
}
