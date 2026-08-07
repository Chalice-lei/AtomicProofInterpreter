#include "scope_inspector.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace apc_debug
{

ScopeInspector::ScopeInspector(std::shared_ptr<DebugInfo> debugInfo)
    : m_debugInfo(std::move(debugInfo))
{}

std::shared_ptr<ScopeDebugInfo>
ScopeInspector::getCurrentScope(size_t currentPC)
{
    return getCurrentScope(currentPC, BranchTrace());
}

std::shared_ptr<ScopeDebugInfo> ScopeInspector::getCurrentScope(
    size_t currentPC,
    const BranchTrace& branchTrace
)
{
    return findScopeAtPC(currentPC, branchTrace);
}

const FunctionDebugInfo*
ScopeInspector::getCurrentFunction(size_t currentPC)
{
    return findFunctionAtPC(currentPC);
}

std::vector<std::shared_ptr<ScopeDebugInfo>>
ScopeInspector::getScopeChain(size_t currentPC)
{
    return getScopeChain(currentPC, BranchTrace());
}

std::vector<std::shared_ptr<ScopeDebugInfo>> ScopeInspector::getScopeChain(
    size_t currentPC,
    const BranchTrace& branchTrace
)
{
    std::vector<std::shared_ptr<ScopeDebugInfo>> chain;
    std::unordered_set<const ScopeDebugInfo*> visited;
    auto currentScope = getCurrentScope(currentPC, branchTrace);
    while (currentScope && visited.insert(currentScope.get()).second) {
        chain.push_back(currentScope);
        currentScope = currentScope->parent.lock();
    }
    return chain;
}

std::vector<VariableDebugInfo>
ScopeInspector::getVisibleVariables(size_t currentPC)
{
    return getVisibleVariables(currentPC, BranchTrace());
}

std::vector<VariableDebugInfo> ScopeInspector::getVisibleVariables(
    size_t currentPC,
    const BranchTrace& branchTrace
)
{
    std::vector<VariableDebugInfo> variables;
    const auto scopeChain = getScopeChain(currentPC, branchTrace);
    for (const auto& scope : scopeChain) {
        collectVariablesFromScope(scope, variables);
    }

    // V1 files may not duplicate parameters in the function scope. Retain that
    // fallback, but do not append all function locals: doing so leaks locals
    // belonging to an inactive branch.
    if (const auto* function = getCurrentFunction(currentPC)) {
        for (const auto& parameter : function->parameters) {
            if (!parameter.isAvailableAtPC(currentPC)) {
                continue;
            }
            const bool exists = std::any_of(
                variables.begin(),
                variables.end(),
                [&](const VariableDebugInfo& value) {
                    return value.name == parameter.name;
                }
            );
            if (!exists) {
                variables.push_back(parameter);
            }
        }

        if (scopeChain.empty()) {
            const auto location =
                m_debugInfo->getSourceLocation(currentPC, branchTrace);
            for (const auto& local : function->localVars) {
                if (!local.isAvailableAtPC(currentPC)) {
                    continue;
                }
                if (location.line != 0 && local.declLine >= location.line) {
                    continue;
                }
                const bool exists = std::any_of(
                    variables.begin(),
                    variables.end(),
                    [&](const VariableDebugInfo& value) {
                        return value.name == local.name;
                    }
                );
                if (!exists) {
                    variables.push_back(local);
                }
            }
        }
    }
    variables.erase(
        std::remove_if(
            variables.begin(),
            variables.end(),
            [&](const VariableDebugInfo& variable) {
                return !variable.isAvailableAtPC(currentPC);
            }
        ),
        variables.end()
    );
    return variables;
}

std::vector<VariableDebugInfo>
ScopeInspector::getLocalVariables(size_t currentPC)
{
    return getLocalVariables(currentPC, BranchTrace());
}

std::vector<VariableDebugInfo> ScopeInspector::getLocalVariables(
    size_t currentPC,
    const BranchTrace& branchTrace
)
{
    std::vector<VariableDebugInfo> variables;
    if (auto scope = getCurrentScope(currentPC, branchTrace)) {
        collectVariablesFromScope(scope, variables);
    }
    variables.erase(
        std::remove_if(
            variables.begin(),
            variables.end(),
            [&](const VariableDebugInfo& variable) {
                return !variable.isAvailableAtPC(currentPC);
            }
        ),
        variables.end()
    );
    return variables;
}

bool ScopeInspector::isVariableVisible(
    const std::string& varName,
    size_t currentPC
)
{
    return isVariableVisible(varName, currentPC, BranchTrace());
}

bool ScopeInspector::isVariableVisible(
    const std::string& varName,
    size_t currentPC,
    const BranchTrace& branchTrace
)
{
    const auto visibleVars = getVisibleVariables(currentPC, branchTrace);
    return std::any_of(
        visibleVars.begin(),
        visibleVars.end(),
        [&](const VariableDebugInfo& variable) {
            return variable.name == varName;
        }
    );
}

std::string ScopeInspector::getScopeDescription(size_t currentPC)
{
    return getScopeDescription(currentPC, BranchTrace());
}

std::string ScopeInspector::getScopeDescription(
    size_t currentPC,
    const BranchTrace& branchTrace
)
{
    std::ostringstream output;
    if (const auto* function = getCurrentFunction(currentPC)) {
        output << "函数: " << function->name << " ("
               << function->location.toString() << ")";
    } else {
        output << "全局作用域";
    }
    auto scope = getCurrentScope(currentPC, branchTrace);
    if (scope && !scope->name.empty()) {
        output << " / 作用域: " << scope->name << " ("
               << scopeTypeToString(scope->type) << ", id="
               << scope->scopeId << ")";
    }
    return output.str();
}

std::shared_ptr<ScopeDebugInfo> ScopeInspector::findScopeAtPC(size_t pc)
{
    return findScopeAtPC(pc, BranchTrace());
}

std::shared_ptr<ScopeDebugInfo> ScopeInspector::findScopeAtPC(
    size_t pc,
    const BranchTrace& branchTrace
)
{
    return m_debugInfo ? m_debugInfo->getScopeAtPC(pc, branchTrace) : nullptr;
}

const FunctionDebugInfo* ScopeInspector::findFunctionAtPC(size_t pc)
{
    return m_debugInfo ? m_debugInfo->getFunctionAtPC(pc) : nullptr;
}

void ScopeInspector::collectVariablesFromScope(
    std::shared_ptr<ScopeDebugInfo> scope,
    std::vector<VariableDebugInfo>& variables
)
{
    if (!scope) {
        return;
    }
    for (const auto& variable : scope->variables) {
        const bool exists = std::any_of(
            variables.begin(),
            variables.end(),
            [&](const VariableDebugInfo& value) {
                return value.name == variable.name;
            }
        );
        if (!exists) {
            variables.push_back(variable);
        }
    }
}

} // namespace apc_debug
