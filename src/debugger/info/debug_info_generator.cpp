#include "debug_info_generator.h"

#include <algorithm>
#include <iterator>

namespace apc_debug
{

DebugInfoGenerator::DebugInfoGenerator(const std::string& sourceFilename)
    : m_sourceFilename(sourceFilename), m_currentFunction(nullptr),
      m_lastSourceLine(0), m_lastPC(0), m_nextPC(0)
{
    m_debugInfo = std::make_shared<DebugInfo>();
    m_debugInfo->sourceFilename = sourceFilename;
    m_debugInfo->version = "2.0";

    auto globalScope =
        std::make_shared<ScopeDebugInfo>("global", ScopeType::GLOBAL);
    m_debugInfo->globalScope = globalScope;
    m_debugInfo->addScope(globalScope);
    m_scopeStack.push(globalScope);
}

std::shared_ptr<DebugInfo> DebugInfoGenerator::getDebugInfo() const
{
    // Keep still-open top-level/function bindings useful to callers without
    // closing them; generation may continue after an intermediate snapshot.
    synchronizeOpenVariables(m_nextPC);
    return m_debugInfo;
}

void DebugInfoGenerator::finalizeScopes()
{
    if (m_scopeStack.empty() || m_scopeStack.size() != 1 ||
        m_scopeStack.top() != m_debugInfo->globalScope ||
        !m_functionStack.empty() || m_currentFunction) {
        m_debugInfo->scopeNestingValid = false;
    }
}

void DebugInfoGenerator::setContractName(const std::string& name)
{
    m_debugInfo->contractName = name;
}

void DebugInfoGenerator::onEmitInstruction(
    size_t pc,
    const std::string& opcode,
    const std::string& operand,
    const SourceLocation& loc
)
{
    m_nextPC = std::max(m_nextPC, pc + 1);
    onEmitInstruction(
        pc,
        opcode,
        operand,
        loc,
        m_currentBranchPath,
        {}
    );
}

void DebugInfoGenerator::onEmitInstruction(
    size_t pc,
    const std::string& opcode,
    const std::string& operand,
    const SourceLocation& loc,
    const std::vector<BranchPredicate>& branchPath,
    const std::vector<std::string>& affectedVars
)
{
    // 每条指令都记录源码映射；断点和单步需要精确到最终 PC。
    if (loc.isValid() && (loc.line != m_lastSourceLine || pc != m_lastPC)) {
        m_debugInfo->addSourceMapping(pc, loc);
        m_lastSourceLine = loc.line;
        m_lastPC = pc;
    }

    // 指令信息仍完整记录
    InstructionDebugInfo instInfo;
    instInfo.pc = pc;
    instInfo.opcode = opcode;
    instInfo.operand = operand;
    instInfo.location = loc;
    instInfo.affectedVars = affectedVars;

    if (loc.isValid()) {
        SourceOrigin origin(loc);
        origin.scopeId = getCurrentScopeId();
        origin.functionName = m_currentFunctionName;
        origin.originalPC = pc;
        origin.path = branchPath;
        origin.affectedVars = affectedVars;
        instInfo.origins.push_back(std::move(origin));
    }

    m_debugInfo->addInstruction(instInfo);

    if (m_debugInfo->globalScope) {
        auto& global = m_debugInfo->globalScope;
        if (global->startPC > pc || global->endPC == 0) {
            global->startPC = pc;
        }
        global->endPC = std::max(global->endPC, pc + 1);
    }
}

void DebugInfoGenerator::onEnterFunction(
    const std::string& funcName,
    bool isPublic,
    const SourceLocation& loc,
    size_t startPC
)
{
    if (!m_currentFunctionName.empty()) {
        m_functionStack.push(m_currentFunctionName);
    }

    FunctionDebugInfo funcInfo(funcName);
    funcInfo.location = loc;
    funcInfo.startPC = startPC;
    funcInfo.isPublic = isPublic;
    if (loc.isValid()) {
        m_debugInfo->addSourceMapping(startPC, loc);
    }

    // endPC 退出时再更新
    m_debugInfo->addFunction(funcInfo);
    m_currentFunctionName = funcName;
    m_currentFunction = &(m_debugInfo->functions[funcName]);

    auto funcScope =
        std::make_shared<ScopeDebugInfo>(funcName, ScopeType::FUNCTION);
    funcScope->location = loc;
    funcScope->startPC = startPC;
    // Interpreter private functions are emitted inline, but function scopes
    // remain structural children of global rather than of their call site.
    auto parentScope = m_debugInfo->globalScope;
    funcScope->parent = parentScope;

    m_debugInfo->addScope(funcScope);
    if (parentScope) {
        parentScope->children.push_back(funcScope);
    }
    m_scopeStack.push(funcScope);

    m_currentFunction->scope = funcScope;
    m_currentFunction->scopeId = funcScope->scopeId;
}

void DebugInfoGenerator::onExitFunction(size_t endPC)
{
    auto expectedFunctionScope =
        m_currentFunction ? m_currentFunction->scope : nullptr;
    const std::string exitingFunction = m_currentFunctionName;
    if (!exitingFunction.empty()) {
        closeVariablesForFunction(exitingFunction, endPC);
    }
    if (m_currentFunction) {
        m_currentFunction->endPC = endPC;
        if (m_currentFunction->scope) {
            m_currentFunction->scope->setRange(
                m_currentFunction->startPC,
                endPC
            );
        }
    }

    if (!m_scopeStack.empty() && expectedFunctionScope &&
        m_scopeStack.top() == expectedFunctionScope) {
        m_scopeStack.pop();
    } else {
        m_debugInfo->scopeNestingValid = false;
    }

    // 回到上层函数
    if (!m_functionStack.empty()) {
        m_currentFunctionName = m_functionStack.top();
        m_functionStack.pop();
        auto it = m_debugInfo->functions.find(m_currentFunctionName);
        if (it != m_debugInfo->functions.end()) {
            m_currentFunction = &(it->second);
        } else {
            m_currentFunction = nullptr;
        }
    } else {
        m_currentFunction = nullptr;
        m_currentFunctionName.clear();
    }
}

std::shared_ptr<ScopeDebugInfo> DebugInfoGenerator::onEnterScope(
    const std::string& scopeName,
    const SourceLocation& loc,
    size_t startPC
)
{
    const ScopeId scopeId = onEnterScope(
        scopeName,
        ScopeType::BLOCK,
        loc,
        startPC
    );
    return m_debugInfo->getScopeById(scopeId);
}

ScopeId DebugInfoGenerator::onEnterScope(
    const std::string& scopeName,
    ScopeType scopeType,
    const SourceLocation& loc,
    size_t startPC
)
{
    auto scope = std::make_shared<ScopeDebugInfo>(scopeName, scopeType);
    const auto parentScope =
        m_scopeStack.empty() ? nullptr : m_scopeStack.top();
    scope->location = loc.isValid()
                          ? loc
                          : (parentScope ? parentScope->location
                                         : SourceLocation());
    scope->startPC = startPC;

    if (parentScope) {
        scope->parent = parentScope;
        parentScope->children.push_back(scope);
    }

    m_debugInfo->addScope(scope);
    m_scopeStack.push(scope);
    return scope->scopeId;
}

void DebugInfoGenerator::onExitScope(size_t endPC)
{
    if (m_scopeStack.size() > 1) {
        auto scope = m_scopeStack.top();
        closeVariablesForScope(scope->scopeId, endPC);
        scope->setRange(scope->startPC, endPC);
        m_scopeStack.pop();
    } else {
        m_debugInfo->scopeNestingValid = false;
    }
}

void DebugInfoGenerator::onExitScope(
    const std::shared_ptr<ScopeDebugInfo>& expectedScope,
    size_t endPC
)
{
    if (m_scopeStack.size() <= 1 || !expectedScope ||
        m_scopeStack.top() != expectedScope) {
        m_debugInfo->scopeNestingValid = false;
        return;
    }

    closeVariablesForScope(expectedScope->scopeId, endPC);
    expectedScope->setRange(expectedScope->startPC, endPC);
    m_scopeStack.pop();
}

void DebugInfoGenerator::onVariableDecl(
    const std::string& varName,
    const std::string& type,
    const SourceLocation& loc,
    bool isStackVar,
    int stackOffset,
    bool isParameter,
    size_t startPC
)
{
    VariableDebugInfo varInfo;
    varInfo.name = varName;
    varInfo.type = type;
    varInfo.declLine = loc.line;
    varInfo.declColumn = loc.column;
    varInfo.isStackVar = isStackVar;
    varInfo.stackOffset = stackOffset;
    varInfo.isParameter = isParameter;

    const size_t resolvedStartPC =
        startPC == UNKNOWN_ORIGINAL_PC ? m_nextPC : startPC;
    varInfo.setAvailabilityRange(resolvedStartPC, resolvedStartPC);

    if (!m_scopeStack.empty()) {
        auto currentScope = m_scopeStack.top();
        varInfo.scopeName = currentScope->name;
        varInfo.scopeId = currentScope->scopeId;
        currentScope->addVariable(varInfo);
    }

    m_debugInfo->addVariable(varInfo);

    if (m_currentFunction) {
        if (isParameter) {
            m_currentFunction->parameters.push_back(varInfo);
        } else {
            m_currentFunction->localVars.push_back(varInfo);
        }
    }

    m_activeVariables.push_back(
        {varName, varInfo.scopeId, m_currentFunctionName, resolvedStartPC}
    );
}

void DebugInfoGenerator::onVariableEnd(
    const std::string& varName,
    size_t endPC
)
{
    for (auto it = m_activeVariables.rbegin();
         it != m_activeVariables.rend();
         ++it) {
        if (it->name != varName) {
            continue;
        }
        synchronizeVariable(*it, endPC);
        m_activeVariables.erase(std::next(it).base());
        return;
    }
}

void DebugInfoGenerator::synchronizeVariable(
    const ActiveVariable& active,
    size_t endPC
) const
{
    auto matches = [&](const VariableDebugInfo& variable) {
        return variable.name == active.name &&
               variable.scopeId == active.scopeId;
    };
    auto update = [&](VariableDebugInfo& variable) {
        if (matches(variable)) {
            variable.setAvailabilityRange(active.startPC, endPC);
        }
    };

    if (auto scope = m_debugInfo->getScopeById(active.scopeId)) {
        for (auto& variable : scope->variables) {
            update(variable);
        }
    }

    if (!active.functionName.empty()) {
        auto function = m_debugInfo->functions.find(active.functionName);
        if (function != m_debugInfo->functions.end()) {
            for (auto& parameter : function->second.parameters) {
                update(parameter);
            }
            for (auto& local : function->second.localVars) {
                update(local);
            }
        }
    }

    auto topLevel = m_debugInfo->variables.find(active.name);
    if (topLevel != m_debugInfo->variables.end()) {
        update(topLevel->second);
    }
}

void DebugInfoGenerator::synchronizeOpenVariables(size_t endPC) const
{
    for (const auto& active : m_activeVariables) {
        synchronizeVariable(active, endPC);
    }
}

void DebugInfoGenerator::closeVariablesForScope(
    ScopeId scopeId,
    size_t endPC
)
{
    for (auto it = m_activeVariables.begin();
         it != m_activeVariables.end();) {
        if (it->scopeId != scopeId) {
            ++it;
            continue;
        }
        synchronizeVariable(*it, endPC);
        it = m_activeVariables.erase(it);
    }
}

void DebugInfoGenerator::closeVariablesForFunction(
    const std::string& functionName,
    size_t endPC
)
{
    for (auto it = m_activeVariables.begin();
         it != m_activeVariables.end();) {
        if (it->functionName != functionName) {
            ++it;
            continue;
        }
        synchronizeVariable(*it, endPC);
        it = m_activeVariables.erase(it);
    }
}

ScopeId DebugInfoGenerator::getCurrentScopeId() const
{
    return m_scopeStack.empty() ? INVALID_SCOPE_ID
                                : m_scopeStack.top()->scopeId;
}

void DebugInfoGenerator::setCurrentBranchPath(
    const std::vector<BranchPredicate>& path
)
{
    m_currentBranchPath = path;
}

} // namespace apc_debug
