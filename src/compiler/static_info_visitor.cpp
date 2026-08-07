#include "static_info_visitor.h"

#include "../log/logger.h"

using namespace tbc;

void StaticInfoVisitor::visit(ContractNode& node)
{
    for (auto& member : node.members) {
        member->accept(*this);
    }
}

void StaticInfoVisitor::visit(FunctionNode& node)
{
    LOG_INFO("Generating function information for: " + node.name);

    // 库函数视为私有: 不进 ABI, 但登记到 all_functions 供调试器使用.
    bool isPrivate = (!node.name.empty() && node.name[0] == '_') ||
                     node.fromLibrary;

    if (isPrivate) {
        LOG_DEBUG("Private function detected: " + node.name);
    }

    generateAllFunctionInfo(node, isPrivate);

    if (!isPrivate) {
        generateFunction(node);
        LOG_INFO("Public function ABI generation completed for: " + node.name);
    } else {
        LOG_INFO("Private function info collected for: " + node.name);
    }
}

void StaticInfoVisitor::visit(ConstructorNode& node)
{
    generateConstructor(node);
}

void StaticInfoVisitor::visit(StructDefNode& node)
{
    LOG_INFO("Generating struct definition information for: ", node.name);
    generateStruct(node);
    LOG_INFO("Struct definition generation completed for: ", node.name);
}

void StaticInfoVisitor::generateStruct(StructDefNode& node)
{
    nlohmann::ordered_json structDef;
    structDef["name"] = node.name;
    structDef["type"] = "struct";

    nlohmann::ordered_json fieldsArray = nlohmann::ordered_json::array();

    for (const auto& field : node.fields) {
        const std::string& fieldName = field.first;
        const std::string& fieldType = field.second.getTypeString();

        nlohmann::ordered_json fieldObj;
        fieldObj["name"] = fieldName;
        fieldObj["type"] = fieldType;
        fieldsArray.push_back(fieldObj);

        LOG_DEBUG(
            "Processing struct field: ", fieldName, " (type: ", fieldType, ")"
        );
    }

    structDef["fields"] = fieldsArray;
    m_structJson.push_back(structDef);
}

nlohmann::ordered_json StaticInfoVisitor::makeParamsJson(
    const std::vector<ParameterInfo>& parameters,
    const std::string& logPrefix
)
{
    nlohmann::ordered_json paramsArray = nlohmann::ordered_json::array();

    for (const auto& param : parameters) {
        nlohmann::ordered_json paramObj;
        paramObj["name"] = param.name;
        paramObj["type"] = param.type;
        paramsArray.push_back(paramObj);

        LOG_DEBUG(logPrefix, param.name, " (type: ", param.type, ")");
    }

    return paramsArray;
}

std::string StaticInfoVisitor::makeUnlockScriptTemplate(
    const std::vector<ParameterInfo>& parameters
)
{
    std::string unlockingScriptTemplate;
    for (const auto& param : parameters) {
        unlockingScriptTemplate += "<" + param.name + ">";
    }
    return unlockingScriptTemplate;
}

void StaticInfoVisitor::generateAllFunctionInfo(FunctionNode& node, bool isPrivate)
{
    nlohmann::ordered_json funcInfo;
    funcInfo["name"] = node.name;
    funcInfo["type"] = isPrivate ? "private" : "public";

    funcInfo["params"] =
        makeParamsJson(node.parameters, "Collecting function parameter: ");
    m_allFunctionsJson.push_back(funcInfo);
}

void StaticInfoVisitor::generateFunction(FunctionNode& node)
{
    nlohmann::ordered_json functionAbi;
    functionAbi["type"] = "function";
    functionAbi["name"] = node.name;
    functionAbi["index"] = m_functionIndex++;

    functionAbi["params"] =
        makeParamsJson(node.parameters, "Processing parameter: ");
    m_abiJson.push_back(functionAbi);

    // unlock: 函数名 -> unlocking script.
    m_unlockJson[node.name] = makeUnlockScriptTemplate(node.parameters);
}

void StaticInfoVisitor::generateConstructor(ConstructorNode& node)
{
    LOG_INFO("Generating constructor ABI information");

    auto params =
        makeParamsJson(node.parameters, "Processing constructor parameter: ");
    for (const auto& paramObj : params) {
        m_constructorParamsJson.push_back(paramObj);
    }

    LOG_INFO("Constructor ABI generation completed");
}
