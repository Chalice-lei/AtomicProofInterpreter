#ifndef DEBUG_INFO_GENERATOR_H
#define DEBUG_INFO_GENERATOR_H

#include <memory>
#include <stack>
#include <string>
#include <vector>

#include "debug_info.h"

namespace apc_debug
{

// 在字节码生成过程中收集调试信息
class DebugInfoGenerator
{
public:
    explicit DebugInfoGenerator(const std::string& sourceFilename);
    ~DebugInfoGenerator() = default;

    void onEmitInstruction(
        size_t pc,
        const std::string& opcode,
        const std::string& operand,
        const SourceLocation& loc
    );

    std::shared_ptr<DebugInfo> getDebugInfo() const
    {
        return m_debugInfo;
    }

    void setContractName(const std::string& name);

    void onEnterFunction(
        const std::string& funcName,
        bool isPublic,
        const SourceLocation& loc,
        size_t startPC
    );

    void onExitFunction(size_t endPC);

    void onEnterScope(
        const std::string& scopeName,
        const SourceLocation& loc,
        size_t startPC
    );

    void onExitScope(size_t endPC);

    void onVariableDecl(
        const std::string& varName,
        const std::string& type,
        const SourceLocation& loc,
        bool isStackVar,
        int stackOffset = -1,
        bool isParameter = false
    );

private:
    std::string m_sourceFilename;
    std::shared_ptr<DebugInfo> m_debugInfo;
    std::stack<std::shared_ptr<ScopeDebugInfo>> m_scopeStack;
    std::string m_currentFunctionName;
    FunctionDebugInfo* m_currentFunction;
    std::stack<std::string> m_functionStack;

    // 类似 Python co_lnotab：仅在源码行变化时记录
    size_t m_lastSourceLine;
    size_t m_lastPC;
};

} // namespace apc_debug

#endif // DEBUG_INFO_GENERATOR_H
