#ifndef PARAMETER_SCHEMA_H
#define PARAMETER_SCHEMA_H

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../util/type_utils.h"

namespace apc_interpreter::parameter_schema
{

inline void parseArrayType(
    const std::string& typeName,
    std::string& outBaseType,
    size_t& outSize
)
{
    outBaseType = typeName;
    outSize = 0;

    if (auto arrayType = apc::util::parseFixedArrayType(typeName)) {
        outBaseType = arrayType->elementType;
        outSize = arrayType->size;
    }
}

inline const nlohmann::json* findStructByName(
    const nlohmann::json& structsArray,
    const std::string& name
)
{
    if (!structsArray.is_array()) {
        return nullptr;
    }

    for (const auto& item : structsArray) {
        if (item.is_object() && item.value("name", "") == name) {
            return &item;
        }
    }

    return nullptr;
}

inline void expandStructFields(
    const nlohmann::json& structsArray,
    const std::string& structName,
    const std::string& prefix,
    std::vector<std::pair<std::string, std::string>>& out
)
{
    const nlohmann::json* structJson = findStructByName(structsArray, structName);
    if (!structJson || !structJson->contains("fields") ||
        !(*structJson)["fields"].is_array()) {
        return;
    }

    for (const auto& field : (*structJson)["fields"]) {
        if (!field.is_object()) {
            continue;
        }

        const std::string fieldName = field.value("name", "");
        const std::string fieldType = field.value("type", "");
        if (fieldName.empty() || fieldType.empty()) {
            continue;
        }

        const std::string path =
            prefix.empty() ? fieldName : prefix + "." + fieldName;

        std::string baseType;
        size_t arraySize = 0;
        parseArrayType(fieldType, baseType, arraySize);

        if (arraySize > 0) {
            if (findStructByName(structsArray, baseType) != nullptr) {
                for (size_t i = 0; i < arraySize; ++i) {
                    expandStructFields(
                        structsArray,
                        baseType,
                        path + "[" + std::to_string(i) + "]",
                        out
                    );
                }
            } else {
                for (size_t i = 0; i < arraySize; ++i) {
                    out.push_back(
                        {path + "[" + std::to_string(i) + "]", baseType}
                    );
                }
            }
            continue;
        }

        if (findStructByName(structsArray, fieldType) != nullptr) {
            expandStructFields(structsArray, fieldType, path, out);
        } else {
            out.push_back({path, fieldType});
        }
    }
}

inline std::vector<std::pair<std::string, std::string>> expandFunctionParams(
    const nlohmann::json* functionJson,
    const nlohmann::json* structsArray
)
{
    std::vector<std::pair<std::string, std::string>> params;
    if (!functionJson || !functionJson->contains("params") ||
        !(*functionJson)["params"].is_array()) {
        return params;
    }

    for (const auto& param : (*functionJson)["params"]) {
        if (!param.is_object()) {
            continue;
        }

        const std::string name = param.value("name", "");
        const std::string type = param.value("type", "");
        if (name.empty() || type.empty()) {
            continue;
        }

        if (structsArray && findStructByName(*structsArray, type) != nullptr) {
            expandStructFields(*structsArray, type, name, params);
        } else {
            params.push_back({name, type});
        }
    }

    return params;
}

} // namespace apc_interpreter::parameter_schema

#endif // PARAMETER_SCHEMA_H
