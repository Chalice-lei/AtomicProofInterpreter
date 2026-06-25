#ifndef COMPILER_RESULT_JSON_H
#define COMPILER_RESULT_JSON_H

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace apc
{

struct CompilerResultJsonSections
{
    nlohmann::ordered_json metadata;
    nlohmann::ordered_json structs;
    nlohmann::ordered_json abi;
    nlohmann::ordered_json unlock;
    nlohmann::ordered_json constructorParams;
    nlohmann::ordered_json functions;
};

struct CompilerLockJson
{
    std::string hex;
    std::optional<std::string> asmCode;
};

inline void setJsonSectionIfPresent(
    nlohmann::ordered_json& dst,
    const std::string& key,
    const nlohmann::ordered_json& value
)
{
    if (!value.is_null() && !value.empty()) {
        dst[key] = value;
    }
}

inline nlohmann::ordered_json buildCompilerResultJson(
    const CompilerResultJsonSections& sections,
    const CompilerLockJson& lock
)
{
    nlohmann::ordered_json result = nlohmann::ordered_json::object();

    setJsonSectionIfPresent(result, "metadata", sections.metadata);
    setJsonSectionIfPresent(result, "structs", sections.structs);
    setJsonSectionIfPresent(result, "abi", sections.abi);

    nlohmann::ordered_json lockData = nlohmann::ordered_json::object();
    if (lock.asmCode.has_value()) {
        lockData["asm"] = *lock.asmCode;
    }
    lockData["hex"] = lock.hex;
    result["lock"] = lockData;

    setJsonSectionIfPresent(result, "unlock", sections.unlock);
    setJsonSectionIfPresent(
        result, "constructorParams", sections.constructorParams
    );
    setJsonSectionIfPresent(result, "functions", sections.functions);

    return result;
}

} // namespace apc

#endif // COMPILER_RESULT_JSON_H
