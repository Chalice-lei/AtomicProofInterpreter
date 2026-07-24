#include "pre_analysis_visitor.h"

#include <climits>

#include "../bytecode/bytecode_builtin_function.h"
#include "../bytecode/bytecode_operation_functions.h"
#include "../log/logger.h"
#include "../util/type_utils.h"

PreAnalysisVisitor::PreAnalysisVisitor()
    : m_hasErrors(false), m_allowSubscopeAltstack(false),
      m_inIfElseScope(false), m_inPrivateFunction(false)
{
    // Initialize consuming operations list
    m_consumingOperations.insert("Hash160");
    m_consumingOperations.insert("CheckSig");
    m_consumingOperations.insert("Sha256");
    m_consumingOperations.insert("Ripemd160");
}

bool PreAnalysisVisitor::analyze(ASTNode& root)
{
    m_hasErrors = false;
    m_errors.clear();
    m_warnings.clear();
    m_variables.clear();

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

    m_currentFunctionName = node.name;
    m_inPrivateFunction = isPrivateFunction(node.name);
    m_inLibraryFunction = node.fromLibrary;
    m_deferredOwnershipParams.clear();

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

    // 私有函数清空所有变量; 公有函数保留副栈中的变量.
    if (m_inPrivateFunction) {
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
                calculateElementStackSize(elementType)
            );
        } else {
            declareVariable(param.name, param.type, getNodeLocation(node));
        }
    }

    // Analyze function body
    if (node.block) {
        node.block->accept(*this);
    }

    m_currentFunctionName = previousFunctionName;
    m_inPrivateFunction = previousInPrivateFunction;
    m_inLibraryFunction = previousInLibraryFunction;
    m_deferredOwnershipParams = previousDeferredParams;
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
    for (auto& stmt : node.statements) {
        stmt->accept(*this);
    }
}

void PreAnalysisVisitor::visit(IfNode& node)
{
    LOG_INFO("Analyzing if statement");

    // Analyze condition expression - if consumes the condition value
    if (node.condition) {
        analyzeConditionalExpression(*node.condition);
    }

    bool previousInIfElseScope = m_inIfElseScope;
    m_inIfElseScope = true;

    // Save current variable state before analyzing branches
    auto savedState = saveVariableState();

    // Analyze then branch
    std::map<std::string, VariableInfo> thenState;
    if (node.thenBranch) {
        node.thenBranch->accept(*this);
        thenState = saveVariableState();
    } else {
        thenState = savedState;
    }

    // Restore state and analyze else branch
    restoreVariableState(savedState);
    std::map<std::string, VariableInfo> elseState;
    if (node.elseBranch) {
        node.elseBranch->accept(*this);
        elseState = saveVariableState();
    } else {
        elseState = savedState;
    }

    m_inIfElseScope = previousInIfElseScope;

    // Merge branch states to determine post-if state
    mergeBranchStates(thenState, elseState);
}

void PreAnalysisVisitor::visit(AssignNode& node)
{
    // Analyze right-hand side first (may consume variables)
    if (node.value) {
        analyzeExpression(*node.value);
    }

    // Handle left-hand side assignment target
    if (node.name) {
        auto [varName, fieldPath] = getFieldPathFromExpr(*node.name);
        if (!varName.empty()) {
            if (dynamic_cast<IdentifierNode*>(node.name.get())) {
                // Simple identifier - could be new variable declaration or
                // reassignment
                VariableInfo* existingVar = findVariable(varName);
                if (!existingVar) {
                    // New variable declaration - classify based on RHS
                    declareVariable(
                        varName,
                        "auto",
                        getNodeLocation(*node.name),
                        node.value.get()
                    );
                    LOG_DEBUG(
                        "Auto-declared variable in assignment: " + varName
                    );
                } else {
                    // Existing variable reassignment (Rust-like move-after-move):
                    // allowed even if previously consumed
                    reassignVariable(varName, getNodeLocation(*node.name));
                }
            } else {
                // Complex expression (field access, etc.) - use, not reassignment
                if (fieldPath.empty()) {
                    useVariable(varName, getNodeLocation(*node.name));
                } else {
                    useField(varName, fieldPath, getNodeLocation(*node.name));
                }
            }
        }
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
        auto valOpt = evaluateIntegerConstant(*arg);
        if (!valOpt.has_value()) {
            reportError(
                "range() arguments must be compile-time integer constants",
                getNodeLocation(*arg)
            );
            return;
        }
        bounds.push_back(valOpt.value());
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
        if (step == 0) {
            reportError("range() step cannot be zero", getNodeLocation(node));
            return;
        }
    }

    std::vector<int64_t> iterations;
    if (step > 0) {
        for (int64_t v = start; v < stop; v += step) {
            iterations.push_back(v);
        }
    } else {
        for (int64_t v = start; v > stop; v += step) {
            iterations.push_back(v);
        }
    }

    node.setStaticIterations(iterations);
    node.setInferredType("int");

    // for 自身拥有一个词法作用域，循环体的 BlockNode 则在每次静态展开
    // 时拥有独立作用域。PreAnalysisVisitor 的变量表目前是扁平结构，因此
    // 在这里显式记录入口变量集合，并在每轮结束时丢弃该轮新声明的名字。
    // 已存在的外层变量仍从本轮结果延续到下一轮。
    auto preLoopState = saveVariableState();
    auto accumulatedState = preLoopState;
    const bool targetExistedBeforeLoop =
        preLoopState.find(node.target) != preLoopState.end();

    if (iterations.empty()) {
        restoreVariableState(preLoopState);
        return;
    }

    auto previousLoopValueIt = m_staticLoopValues.find(node.target);
    bool hadPreviousLoopValue =
        previousLoopValueIt != m_staticLoopValues.end();
    int64_t previousLoopValue =
        hadPreviousLoopValue ? previousLoopValueIt->second : 0;

    for (size_t idx = 0; idx < iterations.size(); ++idx) {
        restoreVariableState(accumulatedState);
        m_staticLoopValues[node.target] = iterations[idx];

        const auto iterationEntryState = saveVariableState();

        LiteralNode literal(
            LiteralNode::Type::Number, std::to_string(iterations[idx])
        );

        if (!targetExistedBeforeLoop && idx == 0) {
            declareVariable(
                node.target, "int", getNodeLocation(node), &literal
            );
        } else {
            reassignVariable(node.target, getNodeLocation(node));
        }

        if (node.body) {
            node.body->accept(*this);
        }

        auto iterationExitState = saveVariableState();

        // 循环 target 在首轮才加入 iterationEntryState，之后属于 loop
        // scope；body 中出现的其它新名字只属于本轮 body scope，不能泄漏
        // 到下一轮，否则相同声明会被误报为 redeclaration。
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

        accumulatedState = std::move(iterationExitState);
    }

    if (hadPreviousLoopValue) {
        m_staticLoopValues[node.target] = previousLoopValue;
    } else {
        m_staticLoopValues.erase(node.target);
    }

    // 新引入的 loop target 随 for scope 一起退出；若入口已有同名变量，
    // AST interpreter 会对该外层绑定赋值，因此保留最后一轮状态。
    if (!targetExistedBeforeLoop) {
        accumulatedState.erase(node.target);
    }

    restoreVariableState(accumulatedState);
}

std::optional<int64_t> PreAnalysisVisitor::evaluateIntegerConstant(
    ExprNode& expr
)
{
    if (auto literal = dynamic_cast<LiteralNode*>(&expr)) {
        if (literal->type == tbc::BytecodeType::Number) {
            try {
                return static_cast<int64_t>(std::stoll(literal->value));
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    if (auto op = dynamic_cast<OpNode*>(&expr)) {
        if (!op->lhs && op->op == "-" && op->rhs) {
            auto inner = evaluateIntegerConstant(*op->rhs);
            if (inner.has_value()) {
                return -inner.value();
            }
        }
    }
    return std::nullopt;
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

        analyzeExpression(*node.expr);
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
}

void PreAnalysisVisitor::visit(VarDeclNode& node)
{
    declareVariable(
        node.name, node.type, getNodeLocation(node), node.initValue.get()
    );

    if (node.initValue) {
        analyzeExpression(*node.initValue);
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

    if (node.isRangeCall || node.funcName == "Range") {
        validateRangeCall(node);
        return;
    }

    // SetAlt: 把变量移到副栈.
    if (node.funcName == "SetAlt" && node.args.size() == 1) {
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
            var->markInAltStack();
            LOG_DEBUG("Variable '" + varName + "' moved to altstack");
        }
        return;
    }

    // SetMain: 把变量从副栈移回主栈.
    if (node.funcName == "SetMain" && node.args.size() == 1) {
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
            if (!var->isInAltStack()) {
                reportError(
                    "Variable '" + varName + "' is not in altstack",
                    getNodeLocation(*node.args[0])
                );
                return;
            }
            // SetMain 把变量搬回主栈, 视为重新绑定, 状态重置为 DECLARED 让后续
            // 消费可走一次完整 move 路径 (原 USED 升级在新规则下会阻断首次消费).
            var->markNotInAltStack();
            var->state = VariableState::DECLARED;
            LOG_DEBUG(
                "Variable '" + varName + "' moved from altstack to main stack"
            );
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
        }
        return;
    }

    // Analyze function arguments
    for (auto& arg : node.args) {
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
    SourceLocation loc("", node.pos.first, node.pos.second);

    size_t arraySize = 0;
    if (node.initArray) {
        arraySize = node.initArray->elements.size();
    } else if (node.sizeExpr) {
        analyzeExpression(*node.sizeExpr);
        // TODO: 更精确的常量计算; 目前默认 10.
        arraySize = 10;
    } else {
        reportError(
            "Array '" + node.name + "' must have size or initializer", loc
        );
        return;
    }

    size_t elementStackSize = calculateElementStackSize(node.elementType);

    declareArrayVariable(
        node.name, node.elementType, loc, arraySize, elementStackSize
    );

    if (node.sizeExpr) {
        analyzeExpression(*node.sizeExpr);
    }

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

    // 3. Check if initialized with constant value
    if (initValue && isConstantValue(*initValue)) {
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
    ExprNode* initValue
)
{
    // Check for redeclaration
    if (m_variables.find(name) != m_variables.end()) {
        reportError("Variable '" + name + "' redeclared", location);
        return;
    }

    // Classify variable data source
    DataSource source = classifyVariable(name, initValue);

    // Add variable to current scope
    m_variables.emplace(name, VariableInfo(name, type, source, location));

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

    // Check if variable has already been consumed
    checkOwnershipViolation(name, location);

    var->state = VariableState::CONSUMED;
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

    // Check if field has already been consumed
    checkFieldOwnershipViolation(varName, fieldPath, location);

    var->markFieldConsumed(fieldPath);
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
    size_t elementStackSize
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
            elementStackSize
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
    if (elementType == "int" || elementType == "string" ||
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

    if (auto literalNode = dynamic_cast<LiteralNode*>(indexExpr)) {
        if (literalNode->type == tbc::BytecodeType::Number) {
            try {
                size_t index = static_cast<size_t>(
                    std::stoull(literalNode->value)
                );
                return index;
            } catch (const std::exception&) {
                LOG_WARNING(
                    "Failed to parse index literal: " + literalNode->value
                );
                return std::nullopt;
            }
        }
    }

    if (auto identNode = dynamic_cast<IdentifierNode*>(indexExpr)) {
        auto loopValueIt = m_staticLoopValues.find(identNode->name);
        if (loopValueIt != m_staticLoopValues.end() &&
            loopValueIt->second >= 0) {
            return static_cast<size_t>(loopValueIt->second);
        }
    }

    // TODO: 支持非字面量索引 (变量、运算表达式等).
    LOG_WARNING("Non-literal index expressions not yet supported");
    return std::nullopt;
}

std::optional<std::pair<std::string, size_t>>
PreAnalysisVisitor::parseFixedArrayType(const std::string& type) const
{
    if (auto arrayType = apc::util::parseFixedArrayType(type)) {
        return std::make_pair(arrayType->elementType, arrayType->size);
    }
    return std::nullopt;
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
    expr.accept(*this);
}

// 小写 return 专用: 表达式中变量只使用不消耗.
void PreAnalysisVisitor::analyzeExpressionForValueReturn(ExprNode& expr)
{
    if (auto* identNode = dynamic_cast<IdentifierNode*>(&expr)) {
        useVariable(identNode->name, getNodeLocation(*identNode));
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
                useVariable(varName, getNodeLocation(*fieldNode));
            } else {
                useField(varName, fieldPath, getNodeLocation(*fieldNode));
            }
        }
        if (fieldNode->base) {
            analyzeExpressionForValueReturn(*fieldNode->base);
        }
        return;
    }

    if (auto* indexNode = dynamic_cast<IndexAccessNode*>(&expr)) {
        // 索引表达式照常分析 (通常是字面量); base 只使用, 不消耗.
        if (indexNode->index) {
            analyzeExpression(*indexNode->index);
        }
        if (indexNode->base) {
            analyzeExpressionForValueReturn(*indexNode->base);
        }
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

        if (var.state == VariableState::DECLARED && var.hasOwnership()) {
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
    const std::map<std::string, VariableInfo>& elseState
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
                        LOG_DEBUG(
                            "Field '" + varName + "." + fieldPath +
                            "' consumed in at least one branch, marking as "
                            "consumed"
                        );
                    }
                    else if (thenFieldState == VariableState::USED ||
                             elseFieldState == VariableState::USED) {
                        varIt->second.markFieldUsed(fieldPath);
                    }
                }
            }
        }
    }
}

void PreAnalysisVisitor::reassignVariable(
    const std::string& name,
    const SourceLocation& location
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

    // Only check ownership for stack data
    if (!var->hasOwnership()) {
        LOG_DEBUG(
            "Reassigning non-stack variable (no ownership check): " + name
        );
        return;
    }

    // Rust-like move-after-move: reassignment allowed even if previously
    // consumed/used. 重新绑定的新值视为可消费一次 (state = DECLARED), 这样
    // `a = a + 1; Keep(a)` 这类合法序列里 Keep(a) 仍可读 a.
    var->state = VariableState::DECLARED;
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
