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

void StaticInfoVisitor::generateAllFunctionInfo(FunctionNode& node, bool isPrivate)
{
    nlohmann::ordered_json funcInfo;
    funcInfo["name"] = node.name;
    funcInfo["type"] = isPrivate ? "private" : "public";

    nlohmann::ordered_json paramsArray = nlohmann::ordered_json::array();

    for (const auto& param : node.parameters) {
        const std::string& paramName = param.name;
        const std::string& paramType = param.type;

        nlohmann::ordered_json paramObj;
        paramObj["name"] = paramName;
        paramObj["type"] = paramType;
        paramsArray.push_back(paramObj);

        LOG_DEBUG(
            "Collecting function parameter: ", paramName, " (type: ", paramType, ")"
        );
    }

    funcInfo["params"] = paramsArray;
    m_allFunctionsJson.push_back(funcInfo);
}

void StaticInfoVisitor::generateFunction(FunctionNode& node)
{
    nlohmann::ordered_json functionAbi;
    functionAbi["type"] = "function";
    functionAbi["name"] = node.name;
    functionAbi["index"] = m_functionIndex++;

    nlohmann::ordered_json paramsArray = nlohmann::ordered_json::array();

    for (const auto& param : node.parameters) {
        const std::string& paramName = param.name;
        const std::string& paramType = param.type;

        nlohmann::ordered_json paramObj;
        paramObj["name"] = paramName;
        paramObj["type"] = paramType;
        paramsArray.push_back(paramObj);

        LOG_DEBUG(
            "Processing parameter: ", paramName, " (type: ", paramType, ")"
        );
    }

    functionAbi["params"] = paramsArray;
    m_abiJson.push_back(functionAbi);
}

void StaticInfoVisitor::generateConstructor(ConstructorNode& node)
{
    LOG_INFO("Generating constructor ABI information");

    for (const auto& param : node.parameters) {
        const std::string& paramName = param.name;
        const std::string& paramType = param.type;

        nlohmann::ordered_json paramObj;
        paramObj["name"] = paramName;
        paramObj["type"] = paramType;
        m_constructorParamsJson.push_back(paramObj);

        LOG_DEBUG(
            "Processing constructor parameter: ",
            paramName,
            " (type: ",
            paramType,
            ")"
        );
    }

    LOG_INFO("Constructor ABI generation completed");
}
