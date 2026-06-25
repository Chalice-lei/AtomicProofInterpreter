#include "function_selection.h"

namespace apc_interpreter::function_selection
{

const nlohmann::json* findFunctionJson(
    const nlohmann::json& jsonData,
    const std::string& name
)
{
    if (!jsonData.contains("functions") || !jsonData["functions"].is_array()) {
        return nullptr;
    }

    for (const auto& function : jsonData["functions"]) {
        if (function.is_object() && function.value("name", "") == name) {
            return &function;
        }
    }

    return nullptr;
}

const nlohmann::json* findFirstPublicFunctionJson(
    const nlohmann::json& jsonData
)
{
    if (!jsonData.contains("functions") || !jsonData["functions"].is_array()) {
        return nullptr;
    }

    for (const auto& function : jsonData["functions"]) {
        if (function.is_object() && function.value("type", "") == "public") {
            return &function;
        }
    }

    return nullptr;
}

FunctionNode* findFunction(ContractNode& contract, const std::string& name)
{
    for (const auto& member : contract.members) {
        if (auto* function = dynamic_cast<FunctionNode*>(member.get())) {
            if (function->name == name) {
                return function;
            }
        }
    }
    return nullptr;
}

const FunctionNode* findFunction(
    const ContractNode& contract,
    const std::string& name
)
{
    for (const auto& member : contract.members) {
        if (const auto* function =
                dynamic_cast<const FunctionNode*>(member.get())) {
            if (function->name == name) {
                return function;
            }
        }
    }
    return nullptr;
}

std::string chooseASTFunctionName(
    const ContractNode& contract,
    const std::string& requestedName
)
{
    if (!requestedName.empty()) {
        return requestedName;
    }

    for (const auto& member : contract.members) {
        if (const auto* function = dynamic_cast<FunctionNode*>(member.get())) {
            if (!function->name.empty() && function->name[0] != '_' &&
                !function->fromLibrary) {
                return function->name;
            }
        }
    }

    return "";
}

#ifdef ENABLE_DEBUGGER
std::optional<std::string> chooseFunctionName(
    const std::shared_ptr<apc_debug::DebugInfo>& debugInfo,
    const nlohmann::json& jsonData,
    const std::string& requestedName
)
{
    if (!requestedName.empty()) {
        return requestedName;
    }

    if (const nlohmann::json* firstPublic =
            findFirstPublicFunctionJson(jsonData)) {
        const std::string name = firstPublic->value("name", "");
        if (!name.empty()) {
            return name;
        }
    }

    if (debugInfo) {
        for (const auto& [name, function] : debugInfo->functions) {
            if (function.isPublic) {
                return name;
            }
        }
    }

    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> debugInfoParams(
    const apc_debug::FunctionDebugInfo& function
)
{
    std::vector<std::pair<std::string, std::string>> params;
    for (const auto& param : function.parameters) {
        params.push_back({param.name, param.type});
    }
    return params;
}
#endif

} // namespace apc_interpreter::function_selection
