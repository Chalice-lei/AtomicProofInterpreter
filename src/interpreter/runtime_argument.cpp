#include "runtime_argument.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

#include "runtime_codec.h"

namespace apc_interpreter
{
namespace
{

RuntimeValue defaultRuntimeStructValue(
    StructDefNode& structDef,
    ContractNode& contract
);

} // namespace

std::string normalizeRuntimeName(const std::string& name)
{
    std::string normalized;
    normalized.reserve(name.size());
    for (unsigned char ch : name) {
        if (std::isalnum(ch) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return normalized;
}

bool isRuntimeIntegerType(const std::string& typeName)
{
    const std::string lower = normalizeRuntimeName(typeName);
    return lower == "int" || lower == "number" || lower == "uint64" ||
           lower == "uint32" || lower == "uint16" || lower == "uint8";
}

bool isRuntimeBoolType(const std::string& typeName)
{
    const std::string lower = normalizeRuntimeName(typeName);
    return lower == "bool" || lower == "boolean";
}

bool isRuntimeHexType(const std::string& typeName)
{
    const std::string lower = normalizeRuntimeName(typeName);
    return lower == "hex" || lower.rfind("hex", 0) == 0 ||
           lower == "bytes" || lower == "pubkey" || lower == "sig";
}

StructDefNode* findStructDefinition(
    ContractNode& contract,
    const std::string& typeName
)
{
    for (auto& member : contract.members) {
        if (auto* structDef = dynamic_cast<StructDefNode*>(member.get())) {
            if (structDef->name == typeName) {
                return structDef;
            }
        }
    }
    return nullptr;
}

const StructDefNode* findStructDefinition(
    const ContractNode& contract,
    const std::string& typeName
)
{
    for (const auto& member : contract.members) {
        if (const auto* structDef =
                dynamic_cast<const StructDefNode*>(member.get())) {
            if (structDef->name == typeName) {
                return structDef;
            }
        }
    }
    return nullptr;
}

bool matchesRuntimeFieldAlias(
    const std::string& candidateName,
    const std::string& expectedName
)
{
    const std::string candidate = normalizeRuntimeName(candidateName);
    const std::string expected = normalizeRuntimeName(expectedName);
    if (candidate == expected) {
        return true;
    }

    if (expected == "lockingscript") {
        return candidate == "script" || candidate == "lockscript";
    }
    if (expected == "suffixdata") {
        return candidate == "suffix" || candidate == "scriptdata";
    }
    if (expected == "partialhash") {
        return candidate == "scriptpartialhash" || candidate == "scripthash";
    }
    if (expected == "size") {
        return candidate == "length" || candidate == "len";
    }
    if (expected == "value") {
        return candidate == "amount" || candidate == "satoshis";
    }
    if (expected == "unlockingscripthash") {
        return candidate == "unlockinghash" || candidate == "scriptunlockinghash";
    }

    return false;
}

const RuntimeValue* findRuntimeFieldByContractName(
    const RuntimeValue::Struct& fields,
    const std::string& expectedName
)
{
    auto exact = fields.find(expectedName);
    if (exact != fields.end()) {
        return &exact->second;
    }

    for (const auto& [candidateName, value] : fields) {
        if (normalizeRuntimeName(candidateName) ==
            normalizeRuntimeName(expectedName)) {
            return &value;
        }
    }

    for (const auto& [candidateName, value] : fields) {
        if (matchesRuntimeFieldAlias(candidateName, expectedName)) {
            return &value;
        }
    }

    return nullptr;
}

RuntimeValue parseRuntimeArgument(
    const std::string& raw,
    const std::string& declaredType
)
{
    const std::string value = runtime_codec::trim(raw);
    if (value.empty()) {
        if (isRuntimeBoolType(declaredType)) {
            return RuntimeValue::fromBool(false);
        }
        if (isRuntimeHexType(declaredType)) {
            return RuntimeValue::fromHexString("0x", declaredType);
        }
        if (normalizeRuntimeName(declaredType) == "string") {
            return RuntimeValue::fromString("");
        }
        return RuntimeValue::fromInt(
            0,
            declaredType.empty() ? "int" : declaredType
        );
    }

    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return RuntimeValue::fromString(value.substr(1, value.size() - 2));
    }

    const std::string lower = runtime_codec::toLower(value);
    if (lower == "true") {
        return RuntimeValue::fromBool(true);
    }
    if (lower == "false") {
        return RuntimeValue::fromBool(false);
    }

    if (value.size() >= 2 &&
        (value.substr(0, 2) == "0x" || value.substr(0, 2) == "0X")) {
        return RuntimeValue::fromHexString(
            value,
            declaredType.empty() ? "hex" : declaredType
        );
    }

    if (isRuntimeHexType(declaredType)) {
        return RuntimeValue::fromHexString(value, declaredType);
    }

    try {
        return RuntimeValue::fromInt(
            std::stoll(value),
            declaredType.empty() ? "int" : declaredType
        );
    } catch (...) {
        return RuntimeValue::fromString(value);
    }
}

RuntimeValue::Struct parseRuntimeFieldAssignments(
    const std::vector<std::string>& assignments,
    const std::string& optionName
)
{
    struct PathSegment
    {
        std::string name;
        bool hasIndex = false;
        size_t index = 0;
    };

    auto parseSegment = [&](const std::string& raw) {
        PathSegment segment;
        const size_t bracket = raw.find('[');
        if (bracket == std::string::npos) {
            segment.name = raw;
            return segment;
        }

        const size_t close = raw.find(']', bracket);
        if (close == std::string::npos || close != raw.size() - 1 ||
            bracket == 0) {
            throw std::runtime_error(
                optionName + " has an invalid indexed path segment '" + raw + "'"
            );
        }

        const std::string indexText =
            raw.substr(bracket + 1, close - bracket - 1);
        if (indexText.empty() ||
            !std::all_of(indexText.begin(), indexText.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            throw std::runtime_error(
                optionName + " has an invalid array index in '" + raw + "'"
            );
        }

        segment.name = raw.substr(0, bracket);
        segment.hasIndex = true;
        segment.index = static_cast<size_t>(std::stoull(indexText));
        return segment;
    };

    auto splitPath = [](const std::string& path) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= path.size()) {
            const size_t dot = path.find('.', start);
            const size_t end = dot == std::string::npos ? path.size() : dot;
            parts.push_back(path.substr(start, end - start));
            if (dot == std::string::npos) {
                break;
            }
            start = dot + 1;
        }
        return parts;
    };

    auto insertField =
        [](auto&& self,
           RuntimeValue::Struct& fields,
           const std::vector<PathSegment>& path,
           size_t index,
           RuntimeValue value) -> void {
        const PathSegment& segment = path[index];
        if (index + 1 == path.size()) {
            if (!segment.hasIndex) {
                fields[segment.name] = std::move(value);
                return;
            }

            auto it = fields.find(segment.name);
            if (it == fields.end()) {
                it = fields
                         .emplace(
                             segment.name,
                             RuntimeValue::fromArray({}, segment.name + "[]")
                         )
                         .first;
            } else if (it->second.type() != RuntimeType::Array) {
                it->second =
                    RuntimeValue::fromArray({}, segment.name + "[]");
            }
            auto& array = it->second.array();
            if (array.size() <= segment.index) {
                array.resize(segment.index + 1);
            }
            array[segment.index] = std::move(value);
            return;
        }

        if (segment.hasIndex) {
            auto it = fields.find(segment.name);
            if (it == fields.end()) {
                it = fields
                         .emplace(
                             segment.name,
                             RuntimeValue::fromArray({}, segment.name + "[]")
                         )
                         .first;
            } else if (it->second.type() != RuntimeType::Array) {
                it->second =
                    RuntimeValue::fromArray({}, segment.name + "[]");
            }
            auto& array = it->second.array();
            if (array.size() <= segment.index) {
                array.resize(segment.index + 1);
            }
            if (array[segment.index].type() != RuntimeType::Struct) {
                array[segment.index] =
                    RuntimeValue::fromStruct({}, segment.name);
            }

            self(
                self,
                array[segment.index].structFields(),
                path,
                index + 1,
                std::move(value)
            );
            return;
        }

        auto it = fields.find(segment.name);
        if (it == fields.end()) {
            it = fields
                     .emplace(
                         segment.name,
                         RuntimeValue::fromStruct({}, segment.name)
                     )
                     .first;
        } else if (it->second.type() != RuntimeType::Struct) {
            it->second = RuntimeValue::fromStruct({}, segment.name);
        }

        self(
            self,
            it->second.structFields(),
            path,
            index + 1,
            std::move(value)
        );
    };

    RuntimeValue::Struct fields;
    for (const std::string& assignment : assignments) {
        const size_t eq = assignment.find('=');
        if (eq == std::string::npos || eq == 0) {
            throw std::runtime_error(
                optionName + " expects field=value, got '" + assignment + "'"
            );
        }

        const std::string name = assignment.substr(0, eq);
        const std::string value = assignment.substr(eq + 1);
        std::vector<std::string> rawPath = splitPath(name);
        std::vector<PathSegment> path;
        path.reserve(rawPath.size());
        for (const auto& rawPart : rawPath) {
            path.push_back(parseSegment(rawPart));
        }
        if (path.empty() ||
            std::any_of(path.begin(), path.end(), [](const PathSegment& part) {
                return part.name.empty();
            })) {
            throw std::runtime_error(
                optionName + " has an invalid field path '" + name + "'"
            );
        }
        insertField(insertField, fields, path, 0, parseRuntimeArgument(value, ""));
    }
    return fields;
}

RuntimeValue defaultRuntimeCompoundValue(
    const std::vector<CompoundFieldInfo>& fields
)
{
    RuntimeValue::Struct result;
    for (const auto& field : fields) {
        result[field.name] =
            RuntimeValue::fromBytes(
                std::vector<uint8_t>(field.byteSize, 0x00),
                field.type
            );
    }
    return RuntimeValue::fromStruct(std::move(result), "__compound__");
}

RuntimeValue defaultRuntimeValueForType(
    const std::string& typeName,
    ContractNode& contract
)
{
    if (StructDefNode* structDef = findStructDefinition(contract, typeName)) {
        return defaultRuntimeStructValue(*structDef, contract);
    }
    if (isRuntimeBoolType(typeName)) {
        return RuntimeValue::fromBool(false);
    }
    if (isRuntimeHexType(typeName)) {
        return RuntimeValue::fromHexString("0x", typeName);
    }
    if (normalizeRuntimeName(typeName) == "string") {
        return RuntimeValue::fromString("");
    }
    if (normalizeRuntimeName(typeName) == "address") {
        return RuntimeValue::fromAddress("");
    }
    if (isRuntimeIntegerType(typeName) || typeName.empty()) {
        return RuntimeValue::fromInt(0, typeName.empty() ? "int" : typeName);
    }
    return RuntimeValue::fromStruct({}, typeName);
}

RuntimeValue defaultRuntimeValueForFieldType(
    const StructFieldType& fieldType,
    ContractNode& contract
)
{
    if (fieldType.isCompoundType) {
        return defaultRuntimeCompoundValue(fieldType.compoundFields);
    }

    if (fieldType.isArray) {
        RuntimeValue::Array values;
        values.reserve(fieldType.arraySize);
        for (size_t i = 0; i < fieldType.arraySize; ++i) {
            values.push_back(defaultRuntimeValueForType(fieldType.baseType, contract));
        }
        return RuntimeValue::fromArray(
            std::move(values),
            fieldType.baseType + "[]"
        );
    }

    return defaultRuntimeValueForType(fieldType.baseType, contract);
}

RuntimeValue canonicalizeRuntimeValueForFieldType(
    const RuntimeValue& value,
    const StructFieldType& fieldType,
    ContractNode& contract
)
{
    if (value.isVoid()) {
        return defaultRuntimeValueForFieldType(fieldType, contract);
    }

    if (fieldType.isArray) {
        if (value.type() != RuntimeType::Array) {
            return defaultRuntimeValueForFieldType(fieldType, contract);
        }

        RuntimeValue::Array array;
        array.reserve(std::max(value.array().size(), fieldType.arraySize));
        for (const auto& item : value.array()) {
            if (item.isVoid()) {
                array.push_back(
                    defaultRuntimeValueForType(fieldType.baseType, contract)
                );
            } else {
                array.push_back(canonicalizeRuntimeValueForType(
                    item,
                    fieldType.baseType,
                    contract
                ));
            }
        }
        while (array.size() < fieldType.arraySize) {
            array.push_back(
                defaultRuntimeValueForType(fieldType.baseType, contract)
            );
        }
        return RuntimeValue::fromArray(
            std::move(array),
            fieldType.baseType + "[]"
        );
    }

    return canonicalizeRuntimeValueForType(value, fieldType.baseType, contract);
}

RuntimeValue canonicalizeRuntimeValueForType(
    const RuntimeValue& value,
    const std::string& typeName,
    ContractNode& contract
)
{
    StructDefNode* structDef = findStructDefinition(contract, typeName);
    if (structDef && value.isVoid()) {
        return defaultRuntimeStructValue(*structDef, contract);
    }
    if (!structDef || value.type() != RuntimeType::Struct) {
        return value;
    }

    RuntimeValue::Struct canonicalFields;
    const auto& inputFields = value.structFields();
    for (const auto& [fieldName, fieldType] : structDef->fields) {
        const auto* source =
            findRuntimeFieldByContractName(inputFields, fieldName);
        if (!source) {
            canonicalFields[fieldName] =
                defaultRuntimeValueForFieldType(fieldType, contract);
            continue;
        }
        canonicalFields[fieldName] = canonicalizeRuntimeValueForFieldType(
            *source,
            fieldType,
            contract
        );
    }

    return RuntimeValue::fromStruct(std::move(canonicalFields), structDef->name);
}

const RuntimeValue* findNamedRuntimeParameter(
    const RuntimeValue::Struct& namedValues,
    const ParameterInfo& parameter
)
{
    if (const auto* byName =
            findRuntimeFieldByContractName(namedValues, parameter.name)) {
        return byName;
    }

    if (!parameter.type.empty()) {
        if (const auto* byType =
                findRuntimeFieldByContractName(namedValues, parameter.type)) {
            return byType;
        }

        const std::string normalizedType = normalizeRuntimeName(parameter.type);
        if (normalizedType == "currenttx") {
            if (const auto* current =
                    findRuntimeFieldByContractName(namedValues, "currenttx")) {
                return current;
            }
            if (const auto* ctx =
                    findRuntimeFieldByContractName(namedValues, "ctx")) {
                return ctx;
            }
        }
        if (normalizedType == "pretx") {
            if (const auto* pretx =
                    findRuntimeFieldByContractName(namedValues, "pretx")) {
                return pretx;
            }
            if (const auto* previous =
                    findRuntimeFieldByContractName(namedValues, "previousTx")) {
                return previous;
            }
        }
    }

    return nullptr;
}

RuntimeValue::Struct canonicalizeBvmFields(
    const RuntimeValue::Struct& fields
)
{
    RuntimeValue::Struct canonical = fields;
    const std::vector<std::string> knownFields = {
        "version",
        "locktime",
        "inputCount",
        "outputCount",
        "inputsHash",
        "unlockingInput",
        "outputsHash",
    };

    for (const auto& fieldName : knownFields) {
        if (const auto* value =
                findRuntimeFieldByContractName(fields, fieldName)) {
            canonical[fieldName] = *value;
        }
    }

    return canonical;
}

std::vector<RuntimeValue> buildRuntimeArgsForFunction(
    FunctionNode* function,
    const RuntimeValue::Struct& namedParamValues,
    const std::vector<std::string>& positionalArgs,
    bool allowPositionalArgs,
    ContractNode& contract
)
{
    std::vector<RuntimeValue> runtimeArgs;
    if (!function) {
        return runtimeArgs;
    }

    if (!namedParamValues.empty()) {
        runtimeArgs.reserve(function->parameters.size());
        for (size_t i = 0; i < function->parameters.size(); ++i) {
            const auto& param = function->parameters[i];
            const auto* namedValue =
                findNamedRuntimeParameter(namedParamValues, param);
            if (allowPositionalArgs && i < positionalArgs.size()) {
                runtimeArgs.push_back(
                    parseRuntimeArgument(positionalArgs[i], param.type)
                );
            } else if (namedValue != nullptr) {
                runtimeArgs.push_back(canonicalizeRuntimeValueForType(
                    *namedValue,
                    param.type,
                    contract
                ));
            } else {
                runtimeArgs.push_back(
                    defaultRuntimeValueForType(param.type, contract)
                );
            }
        }

        if (allowPositionalArgs) {
            for (size_t i = function->parameters.size();
                 i < positionalArgs.size();
                 ++i) {
                runtimeArgs.push_back(parseRuntimeArgument(positionalArgs[i], ""));
            }
        }
        return runtimeArgs;
    }

    if (!allowPositionalArgs) {
        runtimeArgs.reserve(function->parameters.size());
        for (const auto& param : function->parameters) {
            runtimeArgs.push_back(
                defaultRuntimeValueForType(param.type, contract)
            );
        }
        return runtimeArgs;
    }

    runtimeArgs.reserve(positionalArgs.size());
    for (size_t i = 0; i < positionalArgs.size(); ++i) {
        const std::string declaredType =
            i < function->parameters.size() ? function->parameters[i].type : "";
        runtimeArgs.push_back(parseRuntimeArgument(positionalArgs[i], declaredType));
    }
    return runtimeArgs;
}

namespace
{

RuntimeValue defaultRuntimeStructValue(
    StructDefNode& structDef,
    ContractNode& contract
)
{
    RuntimeValue::Struct fields;
    for (const auto& [fieldName, fieldType] : structDef.fields) {
        fields[fieldName] = defaultRuntimeValueForFieldType(fieldType, contract);
    }
    return RuntimeValue::fromStruct(std::move(fields), structDef.name);
}

} // namespace
} // namespace apc_interpreter
