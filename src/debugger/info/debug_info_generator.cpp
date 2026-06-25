#include "debug_info_generator.h"

#include <algorithm>

namespace apc_debug
{

DebugInfoGenerator::DebugInfoGenerator(const std::string& sourceFilename)
    : m_sourceFilename(sourceFilename), m_currentFunction(nullptr),
      m_lastSourceLine(0), m_lastPC(0)
{
    m_debugInfo = std::make_shared<DebugInfo>();
    m_debugInfo->sourceFilename = sourceFilename;
    m_debugInfo->version = "1.0";

    auto globalScope =
        std::make_shared<ScopeDebugInfo>("global", ScopeType::GLOBAL);
    m_debugInfo->globalScope = globalScope;
    m_debugInfo->addScope(globalScope);
    m_scopeStack.push(globalScope);
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

    m_debugInfo->addInstruction(instInfo);
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
    funcScope->parent = m_scopeStack.top();

    m_debugInfo->addScope(funcScope);
    m_scopeStack.push(funcScope);

    m_currentFunction->scope = funcScope;
}

void DebugInfoGenerator::onExitFunction(size_t endPC)
{
    if (m_currentFunction) {
        m_currentFunction->endPC = endPC;
        if (m_currentFunction->scope) {
            m_currentFunction->scope->endPC = endPC;
        }
    }

    if (!m_scopeStack.empty()) {
        m_scopeStack.pop();
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

void DebugInfoGenerator::onEnterScope(
    const std::string& scopeName,
    const SourceLocation& loc,
    size_t startPC
)
{
    auto scope = std::make_shared<ScopeDebugInfo>(scopeName, ScopeType::BLOCK);
    scope->location = loc;
    scope->startPC = startPC;

    if (!m_scopeStack.empty()) {
        scope->parent = m_scopeStack.top();
    }

    m_debugInfo->addScope(scope);
    m_scopeStack.push(scope);
}

void DebugInfoGenerator::onExitScope(size_t endPC)
{
    if (!m_scopeStack.empty()) {
        auto scope = m_scopeStack.top();
        scope->endPC = endPC;
        m_scopeStack.pop();
    }
}

void DebugInfoGenerator::onVariableDecl(
    const std::string& varName,
    const std::string& type,
    const SourceLocation& loc,
    bool isStackVar,
    int stackOffset,
    bool isParameter
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

    if (!m_scopeStack.empty()) {
        auto currentScope = m_scopeStack.top();
        varInfo.scopeName = currentScope->name;
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
}

} // namespace apc_debug
