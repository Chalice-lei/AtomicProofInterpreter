#ifndef DEBUG_INFO_GENERATOR_H
#define DEBUG_INFO_GENERATOR_H

#include <limits>
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

    void onEmitInstruction(
        size_t pc,
        const std::string& opcode,
        const std::string& operand,
        const SourceLocation& loc,
        const std::vector<BranchPredicate>& branchPath,
        const std::vector<std::string>& affectedVars = {}
    );

    std::shared_ptr<DebugInfo> getDebugInfo() const;

    // Compatibility hook used by the Interpreter visitor after generation.
    void finalizeScopes();

    void setContractName(const std::string& name);

    void onEnterFunction(
        const std::string& funcName,
        bool isPublic,
        const SourceLocation& loc,
        size_t startPC
    );

    void onExitFunction(size_t endPC);

    std::shared_ptr<ScopeDebugInfo> onEnterScope(
        const std::string& scopeName,
        const SourceLocation& loc,
        size_t startPC
    );

    ScopeId onEnterScope(
        const std::string& scopeName,
        ScopeType scopeType,
        const SourceLocation& loc,
        size_t startPC
    );

    void onExitScope(size_t endPC);

    // Identity-aware legacy overload. It refuses an out-of-order exit and
    // leaves the stack intact so validation can report the mismatch.
    void onExitScope(
        const std::shared_ptr<ScopeDebugInfo>& expectedScope,
        size_t endPC
    );

    void onVariableDecl(
        const std::string& varName,
        const std::string& type,
        const SourceLocation& loc,
        bool isStackVar,
        int stackOffset = -1,
        bool isParameter = false,
        size_t startPC = UNKNOWN_ORIGINAL_PC
    );

    // Ends the innermost active binding with this name. The range is
    // half-open, so the variable is unavailable at endPC itself.
    void onVariableEnd(const std::string& varName, size_t endPC);

    ScopeId getCurrentScopeId() const;
    void setCurrentBranchPath(const std::vector<BranchPredicate>& path);

private:
    std::string m_sourceFilename;
    std::shared_ptr<DebugInfo> m_debugInfo;
    std::stack<std::shared_ptr<ScopeDebugInfo>> m_scopeStack;
    std::string m_currentFunctionName;
    FunctionDebugInfo* m_currentFunction;
    std::stack<std::string> m_functionStack;
    std::vector<BranchPredicate> m_currentBranchPath;

    struct ActiveVariable
    {
        std::string name;
        ScopeId scopeId = INVALID_SCOPE_ID;
        std::string functionName;
        size_t startPC = 0;
    };
    std::vector<ActiveVariable> m_activeVariables;

    void synchronizeVariable(
        const ActiveVariable& active,
        size_t endPC
    ) const;
    void synchronizeOpenVariables(size_t endPC) const;
    void closeVariablesForScope(ScopeId scopeId, size_t endPC);
    void closeVariablesForFunction(
        const std::string& functionName,
        size_t endPC
    );

    // 类似 Python co_lnotab：仅在源码行变化时记录
    size_t m_lastSourceLine;
    size_t m_lastPC;
    size_t m_nextPC;
};

} // namespace apc_debug

#endif // DEBUG_INFO_GENERATOR_H
