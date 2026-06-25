#include "scope_inspector.h"
#include <sstream>

namespace apc_debug {

ScopeInspector::ScopeInspector(std::shared_ptr<DebugInfo> debugInfo)
    : m_debugInfo(debugInfo) {
}

std::shared_ptr<ScopeDebugInfo> ScopeInspector::getCurrentScope(size_t currentPC) {
    return findScopeAtPC(currentPC);
}

const FunctionDebugInfo* ScopeInspector::getCurrentFunction(size_t currentPC) {
    return findFunctionAtPC(currentPC);
}

std::vector<std::shared_ptr<ScopeDebugInfo>> ScopeInspector::getScopeChain(
    size_t currentPC
) {
    std::vector<std::shared_ptr<ScopeDebugInfo>> chain;
    
    auto currentScope = getCurrentScope(currentPC);
    while (currentScope) {
        chain.push_back(currentScope);
        currentScope = currentScope->parent;
    }
    
    return chain;
}

std::vector<VariableDebugInfo> ScopeInspector::getVisibleVariables(
    size_t currentPC
) {
    std::vector<VariableDebugInfo> variables;

    auto scopeChain = getScopeChain(currentPC);

    // 从内到外，内层先 push（已加入则不覆盖）
    for (auto& scope : scopeChain) {
        collectVariablesFromScope(scope, variables);
    }

    auto func = getCurrentFunction(currentPC);
    if (func) {
        for (const auto& param : func->parameters) {
            bool exists = false;
            for (const auto& v : variables) {
                if (v.name == param.name) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                variables.push_back(param);
            }
        }

        for (const auto& localVar : func->localVars) {
            bool exists = false;
            for (const auto& v : variables) {
                if (v.name == localVar.name) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                variables.push_back(localVar);
            }
        }
    }
    
    return variables;
}

std::vector<VariableDebugInfo> ScopeInspector::getLocalVariables(
    size_t currentPC
) {
    std::vector<VariableDebugInfo> variables;
    
    auto currentScope = getCurrentScope(currentPC);
    if (currentScope) {
        collectVariablesFromScope(currentScope, variables);
    }
    
    return variables;
}

bool ScopeInspector::isVariableVisible(
    const std::string& varName,
    size_t currentPC
) {
    auto visibleVars = getVisibleVariables(currentPC);
    
    for (const auto& var : visibleVars) {
        if (var.name == varName) {
            return true;
        }
    }
    
    return false;
}

std::string ScopeInspector::getScopeDescription(size_t currentPC) {
    std::ostringstream oss;
    
    auto func = getCurrentFunction(currentPC);
    if (func) {
        oss << "函数: " << func->name;
        oss << " (" << func->location.toString() << ")";
    } else {
        oss << "全局作用域";
    }
    
    auto scope = getCurrentScope(currentPC);
    if (scope && scope->name != "") {
        oss << " / 作用域: " << scope->name;
        oss << " (" << scopeTypeToString(scope->type) << ")";
    }
    
    return oss.str();
}

std::shared_ptr<ScopeDebugInfo> ScopeInspector::findScopeAtPC(size_t pc) {
    // 取覆盖 PC 的最内层作用域
    std::shared_ptr<ScopeDebugInfo> result = nullptr;
    size_t minRange = SIZE_MAX;
    
    for (auto& scope : m_debugInfo->scopes) {
        if (pc >= scope->startPC && pc <= scope->endPC) {
            size_t range = scope->endPC - scope->startPC;
            if (range < minRange) {
                minRange = range;
                result = scope;
            }
        }
    }
    
    return result;
}

const FunctionDebugInfo* ScopeInspector::findFunctionAtPC(size_t pc) {
    return m_debugInfo->getFunctionAtPC(pc);
}

void ScopeInspector::collectVariablesFromScope(
    std::shared_ptr<ScopeDebugInfo> scope,
    std::vector<VariableDebugInfo>& variables
) {
    if (!scope) {
        return;
    }
    
    for (const auto& var : scope->variables) {
        bool exists = false;
        for (const auto& v : variables) {
            if (v.name == var.name) {
                exists = true;
                break;
            }
        }
        
        if (!exists) {
            variables.push_back(var);
        }
    }
}

} // namespace apc_debug

