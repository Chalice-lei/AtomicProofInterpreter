#ifndef FUNCTION_SELECTION_H
#define FUNCTION_SELECTION_H

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../ast/ast.h"

#ifdef ENABLE_DEBUGGER
#include "../debugger/info/debug_info.h"
#endif

namespace apc_interpreter::function_selection
{

const nlohmann::json* findFunctionJson(
    const nlohmann::json& jsonData,
    const std::string& name
);
const nlohmann::json* findFirstPublicFunctionJson(
    const nlohmann::json& jsonData
);

FunctionNode* findFunction(ContractNode& contract, const std::string& name);
const FunctionNode* findFunction(
    const ContractNode& contract,
    const std::string& name
);
std::string chooseASTFunctionName(
    const ContractNode& contract,
    const std::string& requestedName
);

#ifdef ENABLE_DEBUGGER
std::optional<std::string> chooseFunctionName(
    const std::shared_ptr<apc_debug::DebugInfo>& debugInfo,
    const nlohmann::json& jsonData,
    const std::string& requestedName
);
std::vector<std::pair<std::string, std::string>> debugInfoParams(
    const apc_debug::FunctionDebugInfo& function
);
#endif

} // namespace apc_interpreter::function_selection

#endif // FUNCTION_SELECTION_H
