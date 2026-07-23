#include "ast_to_bytecode_visitor.h"

#include <climits>
#include <cctype>
#include <functional>
#include <optional>
#include <sstream>
#include <unordered_map>

#include "../bytecode/bytecode_builtin_function.h"
#include "../bytecode/bytecode_builtin_struct.h"
#include "../bytecode/bytecode_helper_fun.h"
#include "../bytecode/bytecode_operation_calcu.h"
#include "../bytecode/bytecode_operation_functions.h"
#include "../bytecode/type_validator.h"
#include "../util/compiler_placeholder.h"
#include "../util/defer.h"
#include "../util/type_utils.h"

using namespace tbc;

void ASTToBytecodeVisitor::visit(ContractNode& node)
{
    LOG_DEBUG("Visiting contract node start. name: " + node.name);

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
            m_scopePtr->defineSymbol(paramName);
            m_scopePtr->push(paramName, paramType, paramName);
            m_generator.emitUnlock("<" + paramName + ">");
        }
    }
    m_generator.emitUnlockName(node.name);
    m_generator.mergeSubUnoverall();

    std::string previousReturnType = m_currentFunctionReturnType;
    m_currentFunctionReturnType = node.returnType;
    if (node.block) {
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

    m_scopePtr->enterScope();

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
    auto altStack = SymbolTable::getSharedAltStack();
    if (altStack) {
        snapshot.elements = altStack->getStackContent();
        snapshot.combinedStackSize = altStack->getCombinedStackSize();
    }
    return snapshot;
}

void ASTToBytecodeVisitor::restoreAltStack(const AltStackSnapshot& snapshot)
{
    auto altStack = SymbolTable::getSharedAltStack();
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
        auto literal = dynamic_cast<const LiteralNode*>(index->index.get());
        if (!baseName.has_value() || !literal ||
            literal->type != LiteralNode::Type::Number) {
            return std::nullopt;
        }
        try {
            return baseName.value() + "[" +
                   numberToScriptHex(std::stoll(literal->value)) + "]";
        } catch (const std::exception&) {
            return std::nullopt;
        }
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
    const IfNode& node
)
{
    auto fail = [&](const std::string& symbol) {
        std::ostringstream oss;
        oss << "cannot merge outer variable '" << symbol
            << "' after if branch at line " << node.pos.first << ", column "
            << node.pos.second
            << ": the variable has no value on this control-flow path";
        SourceLocation loc = getNodeLocation(node);
        SEMANTIC_ERROR(
            oss.str(),
            loc,
            "Assign the variable on both branches before using it after the if"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    };

    for (const auto& symbol : symbols) {
        auto mainPos = m_scopePtr->getPos(symbol);
        if (mainPos.has_value()) {
            if (mainPos.value() != STACK_TOP_POS) {
                emitRoll(mainPos.value());
                m_scopePtr->roll(mainPos.value());
            }
            continue;
        }

        std::string mutableSymbol = symbol;
        auto fixedElement = m_scopePtr->getFixed(mutableSymbol);
        if (fixedElement.has_value()) {
            m_generator.emit(fixedElement->getData());
            m_scopePtr->removeFixed(mutableSymbol);
            m_scopePtr->push(StackElement(
                symbol, fixedElement->getType(), fixedElement->getData()
            ));
            continue;
        }

        auto altPos = m_scopePtr->getPos(symbol, true);
        if (!altPos.has_value()) {
            fail(symbol);
        }

        SymbolTable& state = m_scopePtr->getCurrentSymtab();
        auto mainStack = state.m_stackPtr;
        auto altStack = state.m_altStackPtr;
        const int64_t position = altPos.value();

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

bool ASTToBytecodeVisitor::statementAlwaysReturns(const StmtNode* stmt) const
{
    if (!stmt) {
        return false;
    }
    if (dynamic_cast<const ReturnNode*>(stmt)) {
        return true;
    }
    if (auto block = dynamic_cast<const BlockNode*>(stmt)) {
        for (const auto& innerStmt : block->statements) {
            if (statementAlwaysReturns(innerStmt.get())) {
                return true;
            }
        }
        return false;
    }
    if (auto ifNode = dynamic_cast<const IfNode*>(stmt)) {
        return ifNode->thenBranch && ifNode->elseBranch &&
               statementAlwaysReturns(ifNode->thenBranch.get()) &&
               statementAlwaysReturns(ifNode->elseBranch.get());
    }
    if (auto forNode = dynamic_cast<const ForNode*>(stmt)) {
        return !forNode->getStaticIterations().empty() &&
               statementAlwaysReturns(forNode->body.get());
    }
    return false;
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

    auto sameLayout = [](
                          const std::vector<StackElement>& lhs,
                          const std::vector<StackElement>& rhs
                      ) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (size_t i = 0; i < lhs.size(); ++i) {
            if (lhs[i].getName() != rhs[i].getName() ||
                lhs[i].getType() != rhs[i].getType()) {
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
    if (!sameLayout(thenMain, elseMain)) {
        fail("main stack layouts differ after branch materialization");
    }
    if (!sameLayout(thenAltStack.elements, elseAltStack.elements)) {
        fail("alternative stack layouts differ");
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

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    // 优化: if (a != b) 用 OP_EQUAL+OP_NOTIF 替 OP_EQUAL+OP_NOT+OP_IF (省 1 字节).
    auto* condOpNode = dynamic_cast<OpNode*>(node.condition.get());
    bool useNotIf = condOpNode && condOpNode->op == "!=" &&
                    condOpNode->lhs != nullptr;

    if (useNotIf) {
        // 临时把 != 改 ==, 仅生成 OP_EQUAL.
        condOpNode->op = "==";
        visitExpr(*node.condition);
        condOpNode->op = "!=";
    } else {
        visitExpr(*node.condition);
    }
    m_scopePtr->pop();

    m_generator.emit(
        useNotIf ? tbc::BytOpcode::OP_NOTIF : tbc::BytOpcode::OP_IF
    );

    const SymbolTable entryState = m_scopePtr->getCurrentSymtab();
    const AltStackSnapshot entryAltStack = captureAltStack();
    const auto mergeSymbols =
        collectIfMergeSymbols(node, entryState, entryAltStack);
    const bool thenAlwaysReturns =
        statementAlwaysReturns(node.thenBranch.get());
    const bool elseAlwaysReturns =
        statementAlwaysReturns(node.elseBranch.get());

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
        return; // 跳过这个 if, 让编译继续.
    } else {
        node.thenBranch->accept(*this);
    }

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif
    if (!thenAlwaysReturns) {
        materializeBranchSymbols(mergeSymbols, node);
    }
    LOG_DEBUG("End of if branch");
    SymbolTable thenState = m_scopePtr->exitScope();
    const AltStackSnapshot thenAltStack = captureAltStack();

    // else 必须从与 then 相同的入口状态开始，不能继承 then 的编译期固定值。
    m_scopePtr->replaceCurrentSymtab(entryState);
    restoreAltStack(entryAltStack);

    if (node.elseBranch) {
        m_generator.emit(tbc::BytOpcode::OP_ELSE);
        node.elseBranch->accept(*this);
    } else {
        // TODO: 暂不支持缺 else 分支.
        SourceLocation loc = getNodeLocation(node);
        std::ostringstream oss;
        oss << "if statement at line " << node.pos.first << ", column "
            << node.pos.second
            << " is missing else branch - this is currently not supported";
        SYNTAX_ERROR(
            oss.str(),
            loc,
            "Add an else branch to the if statement or use a different control "
            "structure"
        );
        LOG_ERROR(oss.str());
        throw std::runtime_error(oss.str());
    }

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif
    if (!elseAlwaysReturns) {
        materializeBranchSymbols(mergeSymbols, node);
    }
    SymbolTable elseState = m_scopePtr->exitScope();
    const AltStackSnapshot elseAltStack = captureAltStack();
    LOG_DEBUG("End of else branch");

    SymbolTable mergedState = entryState;
    AltStackSnapshot mergedAltStack = entryAltStack;

    if (!thenAlwaysReturns && !elseAlwaysReturns) {
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
    } else if (!thenAlwaysReturns) {
        // else 已终止，只有 then 会到达 OP_ENDIF 后的代码。
        mergedState =
            buildMergedBranchState(entryState, thenState, mergeSymbols);
        mergedAltStack = thenAltStack;
    } else if (!elseAlwaysReturns) {
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
}

void ASTToBytecodeVisitor::visit(ForNode& node)
{
    LOG_DEBUG("Visiting for node start.");

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    const auto& iterations = node.getStaticIterations();

    if (iterations.empty()) {
        LOG_WARNING(
            "for loop has no iterations after static analysis, skipping body"
        );
        return;
    }

    bool hasSymbol = m_scopePtr->symbolExists(node.target);
    bool firstIteration = true;
    const std::string& inferredType = node.getInferredType();

    for (size_t idx = 0; idx < iterations.size(); ++idx) {
        std::string literalStr = std::to_string(iterations[idx]);

        if (!hasSymbol && firstIteration) {
            auto literalExpr = std::make_unique<LiteralNode>(
                LiteralNode::Type::Number,
                literalStr,
                node.pos.first,
                node.pos.second
            );
            auto varDecl = VarDeclNode(
                node.target,
                inferredType,
                node.pos.first,
                node.pos.second,
                std::move(literalExpr)
            );
            varDecl.accept(*this);
            hasSymbol = true;
        } else {
            auto literalExpr = std::make_unique<LiteralNode>(
                LiteralNode::Type::Number,
                literalStr,
                node.pos.first,
                node.pos.second
            );
            auto identifierExpr = std::make_unique<IdentifierNode>(
                node.target, node.pos.first, node.pos.second
            );
            auto assign = AssignNode(
                std::move(identifierExpr),
                std::move(literalExpr),
                node.pos.first,
                node.pos.second
            );
            assign.accept(*this);
        }

        if (node.body) {
            // 循环体不开新作用域: 迭代间需共享栈状态 (例如累加器).
            executeStatements(node.body->statements);
        }

        firstIteration = false;
    }

    LOG_DEBUG("Visiting for node end.");
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
    auto rightHandSideElement =
        generalLamd(*node.value, valueElementOpt, "right-hand side");
    if (!rightHandSideElement.has_value()) {
        SourceLocation loc = getNodeLocation(*node.value);
        std::ostringstream oss;
        oss << "assigning a generated script element to variable may not be "
               "meaningful. Element: "
            << valueElementOpt.value().getName();
        COMPILER_WARNING(oss.str(), loc);
        LOG_WARNING(oss.str());
    }

    visitExpr(*node.name);

    auto nameElementOpt = m_scopePtr->pop();
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
        !CompilerPlaceholder::isPlaceholder(valueElementStr) &&
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
        !CompilerPlaceholder::isPlaceholder(valueElementStr) &&
        elementPosOpt.has_value() && comElementPos.has_value();

    if (isStackToStackCopy) {
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
        } else if (posB < posA) {
            // A: b 比 a 更靠近栈顶 (posB < posA).
            // 1. TOALTSTACK × posB: b 上方元素暂存到 alt.
            for (int64_t i = 0; i < posB; i++) {
                m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
            }
            // 2. DROP: 移除栈顶 b.
            m_generator.emit(tbc::BytOpcode::OP_DROP);
            // 3. PICK(posA - posB - 1): 按移动后深度修正, 拷贝 a 到栈顶.
            emitPick(posA - posB - 1);
            // 4. FROMALTSTACK × posB: 恢复保存的元素.
            for (int64_t i = 0; i < posB; i++) {
                m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
            }
        } else {
            // B: a 比 b 更靠近栈顶 (posA < posB).
            // 特例 posA==0 && posB==1: a, b, T -> a, a, T,
            // 用 OP_NIP+OP_DUP (2 字节) 替代通用 7 字节序列.
            if (posA == 0 && posB == 1) {
                m_generator.emit(tbc::BytOpcode::OP_NIP);
                m_generator.emit(tbc::BytOpcode::OP_DUP);
            } else {
                // 通用序列 (posA<posB): 约 2·posB + 4 字节.
                // 1. PICK(posA): 拷贝 a 到栈顶.
                emitPick(posA);
                // 2. (SWAP+TOALTSTACK) × posB: 元素转 alt, 保留 a 的拷贝.
                for (int64_t i = 0; i < posB; i++) {
                    emitRoll(1);
                    m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                }
                // 3. TOALTSTACK: a 的拷贝最后压入 alt (先弹出).
                m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                // 4. DROP: 移除栈顶 b.
                m_generator.emit(tbc::BytOpcode::OP_DROP);
                // 5. FROMALTSTACK × (posB+1): 弹 a 拷贝, 再恢复 s0..s_{posB-1}.
                for (int64_t i = 0; i <= posB; i++) {
                    m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
                }
            }
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
            !CompilerPlaceholder::isPlaceholder(valueElementStr);

        if (comElementPos.has_value()) {
            auto vmElementPos = 0;
            // PICK/占位符结果在栈顶+1, 目标槽位下移 1; RHS 不在主栈时不偏移.
            if (CompilerPlaceholder::isPlaceholder(valueElementStr) ||
                elementPosOpt.has_value()) {
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
            } else if (CompilerPlaceholder::isPlaceholder(valueElementStr)) {
                // 3b: 非栈左 = 占位符右 -> push 左值, 绑到栈顶占位符槽.
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
            if (CompilerPlaceholder::isPlaceholder(valueElementStr)) {
                // 1b: 栈左 = 占位符右 -> 字节码已含 +1 偏移 ROLL+DROP,
                // 编译器层 roll LHS 到栈顶, 占位符运行时值即 LHS 新内容.
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
    visitExpr(*node.expr);
    const size_t postSize = m_scopePtr->size();
    if (!isCall && postSize > preSize) {
        const size_t delta = postSize - preSize;
        for (size_t k = 0; k < delta; ++k) {
            m_scopePtr->pop();
            m_generator.emit(tbc::BytOpcode::OP_DROP);
        }
    }
    LOG_DEBUG("Visiting exprstmt node end.");
}

void ASTToBytecodeVisitor::visit(ReturnNode& node)
{
    LOG_DEBUG("Visiting return node start.");

#ifdef ENABLE_DEBUGGER
    setCurrentLocationForGenerator(node);
#endif

    m_currentReturnNode = &node;

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
        LOG_DEBUG("Value-return statement (lowercase return): using existing "
                  "top-of-stack value");

        // 小写 return: 返回值在作用域清理时必须被 keep, 不能 DROP.
        // 支持 `return someVar` 与 `return {a, b, c}` 多变量大括号.
        SymbolTable& symbolTable = m_scopePtr->getCurrentSymtab();

        if (auto identifierNode = dynamic_cast<IdentifierNode*>(node.expr.get()
            )) {
            const std::string& varName = identifierNode->name;
            auto mainPosOpt = symbolTable.getPos(varName);
            auto altPosOpt = symbolTable.getPos(varName, true);
            if (mainPosOpt.has_value() || altPosOpt.has_value()) {
                LOG_DEBUG("Marking return value variable as keep: " + varName);
                symbolTable.m_keepSymbol.push_back(varName);
            } else {
                LOG_DEBUG(
                    "Return value variable not found in current symtab (may "
                    "already be consumed): " +
                    varName
                );
            }
        }
        // 大括号: 多返回值.
        else if (auto braceExpr = dynamic_cast<BraceExprNode*>(node.expr.get()
                 )) {
            LOG_DEBUG(
                "Processing multi-value return with " +
                std::to_string(braceExpr->elements.size()) + " elements"
            );

            for (const auto& element : braceExpr->elements) {
                if (auto identifierNode =
                        dynamic_cast<IdentifierNode*>(element.get())) {
                    const std::string& varName = identifierNode->name;
                    auto mainPosOpt = symbolTable.getPos(varName);
                    auto altPosOpt = symbolTable.getPos(varName, true);
                    if (mainPosOpt.has_value() || altPosOpt.has_value()) {
                        LOG_DEBUG(
                            "Marking return value variable as keep: " + varName
                        );
                        symbolTable.m_keepSymbol.push_back(varName);
                    } else {
                        LOG_DEBUG(
                            "Return value variable not found in current symtab "
                            "(may already be consumed): " +
                            varName
                        );
                    }
                } else {
                    std::ostringstream oss;
                    oss << "lowercase 'return' brace expression only accepts "
                           "variable names (e.g. `return {a, b}`); "
                           "got a non-identifier element";
                    SourceLocation loc("", node.pos.first, node.pos.second);
                    SEMANTIC_ERROR(
                        oss.str(), loc,
                        "Use uppercase 'Return' if you need to compute a value; "
                        "lowercase 'return' only marks existing stack variables "
                        "as kept across scope cleanup"
                    );
                    LOG_ERROR(oss.str());
                    throw std::runtime_error(oss.str());
                }
            }
        } else {
            std::ostringstream oss;
            oss << "lowercase 'return' only accepts a variable name or a brace "
                   "expression of variable names "
                   "(e.g. `return x` or `return {a, b}`)";
            SourceLocation loc("", node.pos.first, node.pos.second);
            SEMANTIC_ERROR(
                oss.str(), loc,
                "Use uppercase 'Return' to compute and return a value; "
                "lowercase 'return' only marks existing stack variables "
                "as kept across scope cleanup"
            );
            LOG_ERROR(oss.str());
            throw std::runtime_error(oss.str());
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

                // 数组元素标签 (逆序赋值).
                std::string elementLabel = m_scopePtr->getArrayElementLabel(
                    node.name, arraySize - 1 - i
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

    // 把实参映射到形参名.
    LOG_DEBUG("Binding parameters");

    for (int i = node.parameters.size() - 1; i >= 0; --i) {
        const auto& paramInfo = node.parameters[i];
        const std::string& paramName = paramInfo.name;
        const std::string& paramType = paramInfo.type;
        auto argRootName = existingArgs[i].getName();

        // 不在此处对 argRootName 改名:
        //  - CompilerPlaceholder 实参: 走普通栈元素路径, 保留 /Compiler.../ 原名
        //    供 BindSymbol 解析后在真栈上匹配; 与普通局部变量 / main 入参一致.
        //  - <self.X> 实参: 不会进 BindSymbol 解析, 由 visitIdentifier 在
        //    形参访问时按需 push 名 + emit.
        // 早期版本曾对 CompilerPlaceholder 做 renameTopToBottom + 改 argRootName,
        // 但 Scope::getCurrentSymtab() const 是值返回, rename 实际作用在 SymbolTable
        // 副本上, 真栈毫无变化, 反而让 BindSymbol 指向真栈上不存在的 funcName_paramName,
        // 导致 visitIdentifier 形参访问失败.

        bool isStructType = !paramType.empty() &&
                            (m_structDefinitions.find(paramType) !=
                             m_structDefinitions.end());

        if (isStructType) {
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

    if (node.block) {
        LOG_DEBUG("Executing private function body inline");
        node.block->accept(*this);

        cleanupFunctionParameters(node);
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

        bool isStructType = m_structDefinitions.find(paramType) !=
                            m_structDefinitions.end();

        if (isStructType) {
            cleanupStructParameter(paramName, paramType);
        } else {
            cleanupBasicParameter(paramName);
        }
    }

    for (const auto& param : node.parameters) {
        const std::string& paramName = param.name;
        LOG_DEBUG("Removing parameter binding for: " + paramName);
        symbolTable.removeBindSymbol(paramName);
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

    // 逆序清理: 字段按声明顺序入栈, 弹出顺序与之相反.
    for (auto it = fieldPathsAndTypes.rbegin(); it != fieldPathsAndTypes.rend();
         ++it) {
        const std::string& fieldPath = it->first;
        const std::string& fieldType = it->second;

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
                    "Moving field from alt stack and dropping: " + fieldPath
                );
                // 副栈 -> 主栈 -> drop.
                auto moveCount = m_scopePtr->setMain(const_cast<std::string&>(
                    const_cast<std::string&>(fieldPath)
                ));
                for (int j = 0; j < moveCount; j++) {
                    m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
                }
                m_generator.emit(tbc::BytOpcode::OP_DROP);
                m_scopePtr->pop();

                // 其他元素回副栈.
                for (int j = 0; j < moveCount - 1; j++) {
                    m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                }
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
                "Moving parameter from alt stack and dropping: " + paramName
            );
            // 副栈 -> 主栈 -> drop.
            auto moveCount = m_scopePtr->setMain(
                const_cast<std::string&>(const_cast<std::string&>(paramName))
            );
            for (int i = 0; i < moveCount; i++) {
                m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
            }
            m_generator.emit(tbc::BytOpcode::OP_DROP);
            m_scopePtr->pop();

            // 其他元素回副栈.
            for (int i = 0; i < moveCount - 1; i++) {
                m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
            }
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

    // Delete: 删除 fixed 数据.
    if ("Delete" == functionName) {
        LOG_DEBUG(
            "Special handling for Delete function with " +
            std::to_string(args.size()) + " arguments"
        );

        for (const auto& arg : args) {
            if (auto identifierNode =
                    dynamic_cast<const IdentifierNode*>(arg.get())) {
                std::string varName = identifierNode->name;

                if (auto fixedVar = m_scopePtr->getFixed(varName)) {
                    LOG_INFO("Deleting fixed data: " + varName);
                    m_scopePtr->removeFixed(varName);
                }
            }
        }

        // 继续走主栈/副栈删除流程.
        LOG_DEBUG(
            "Fixed data deletion completed, continuing with stack deletion"
        );
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

        int expectedArgCount = builtFunPtr->getExpectedArgCount();
        auto processedArgs =
            processArguments(args, expectedArgCount, functionName);

        if ("Keep" == functionName) {
            keep(processedArgs);
            return;
        }
        auto opcodeHex = builtFunPtr->getOpcodeHex(
            isMethodCall ? objectElement.value() : StackElement(),
            processedArgs,
            m_scopePtr
        );
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
        m_generator.emit(opcodeHex);
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
    const std::vector<tbc::StackElement>& elementsVec,
    const std::vector<ParameterInfo>& paramInfos
)
{
    if (elementsVec.empty()) {
        return;
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

        LOG_DEBUG(
            "processArgsToTop: processing argument " + std::to_string(i) +
            " with name: " + elementStr + ", type: " + paramType
        );

        bool isStructType = !paramType.empty() &&
                            m_structDefinitions.find(paramType) !=
                                m_structDefinitions.end();

        if (isStructType) {
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
            } else if (posB < posA) {
                for (int64_t k = 0; k < posB; ++k)
                    m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                m_generator.emit(tbc::BytOpcode::OP_DROP);
                emitPick(posA - posB - 1);
                for (int64_t k = 0; k < posB; ++k)
                    m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
            } else {
                emitPick(posA);
                for (int64_t k = 0; k < posB; ++k) {
                    emitRoll(1);
                    m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                }
                m_generator.emit(tbc::BytOpcode::OP_TOALTSTACK);
                m_generator.emit(tbc::BytOpcode::OP_DROP);
                for (int64_t k = 0; k <= posB; ++k)
                    m_generator.emit(tbc::BytOpcode::OP_FROMALTSTACK);
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
    const std::vector<std::unique_ptr<StmtNode>>& statements
)
{
    std::string codeLevel{};
    for (auto it : m_codeBlockLevel) {
        codeLevel = codeLevel + std::to_string(it) + "-";
    }
    m_codeBlockLevel.push_back(0);
    DEFER_BLOCK(m_codeBlockLevel.pop_back(););

    for (const auto& stmt : statements) {
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

    // bare 符号 (结构体变量本身) 改名.
    {
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
