#ifndef SCOPE_INSPECTOR_H
#define SCOPE_INSPECTOR_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../info/debug_info.h"

namespace apc_debug {

/**
 * @brief 根据 PC 定位活动作用域，分析变量可见性。
 */
class ScopeInspector {
public:
    explicit ScopeInspector(std::shared_ptr<DebugInfo> debugInfo);
    ~ScopeInspector() = default;

    std::shared_ptr<ScopeDebugInfo> getCurrentScope(size_t currentPC);
    std::shared_ptr<ScopeDebugInfo> getCurrentScope(
        size_t currentPC,
        const BranchTrace& branchTrace
    );
    const FunctionDebugInfo* getCurrentFunction(size_t currentPC);

    // 父作用域链（从内到外）
    std::vector<std::shared_ptr<ScopeDebugInfo>> getScopeChain(size_t currentPC);
    std::vector<std::shared_ptr<ScopeDebugInfo>> getScopeChain(
        size_t currentPC,
        const BranchTrace& branchTrace
    );

    // 所有可见变量
    std::vector<VariableDebugInfo> getVisibleVariables(size_t currentPC);
    std::vector<VariableDebugInfo> getVisibleVariables(
        size_t currentPC,
        const BranchTrace& branchTrace
    );

    // 仅当前作用域的局部变量
    std::vector<VariableDebugInfo> getLocalVariables(size_t currentPC);
    std::vector<VariableDebugInfo> getLocalVariables(
        size_t currentPC,
        const BranchTrace& branchTrace
    );

    bool isVariableVisible(const std::string& varName, size_t currentPC);
    bool isVariableVisible(
        const std::string& varName,
        size_t currentPC,
        const BranchTrace& branchTrace
    );

    std::string getScopeDescription(size_t currentPC);
    std::string getScopeDescription(
        size_t currentPC,
        const BranchTrace& branchTrace
    );

    std::shared_ptr<DebugInfo> getDebugInfo() const { return m_debugInfo; }

private:
    std::shared_ptr<DebugInfo> m_debugInfo;

    std::shared_ptr<ScopeDebugInfo> findScopeAtPC(size_t pc);
    std::shared_ptr<ScopeDebugInfo> findScopeAtPC(
        size_t pc,
        const BranchTrace& branchTrace
    );
    const FunctionDebugInfo* findFunctionAtPC(size_t pc);

    void collectVariablesFromScope(
        std::shared_ptr<ScopeDebugInfo> scope,
        std::vector<VariableDebugInfo>& variables
    );
};

} // namespace apc_debug

#endif // SCOPE_INSPECTOR_H
