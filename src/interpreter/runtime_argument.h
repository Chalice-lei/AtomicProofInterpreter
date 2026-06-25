#ifndef RUNTIME_ARGUMENT_H
#define RUNTIME_ARGUMENT_H

#include <string>
#include <vector>

#include "../ast/ast.h"
#include "runtime_value.h"

namespace apc_interpreter
{

std::string normalizeRuntimeName(const std::string& name);
bool isRuntimeIntegerType(const std::string& typeName);
bool isRuntimeBoolType(const std::string& typeName);
bool isRuntimeHexType(const std::string& typeName);

StructDefNode* findStructDefinition(
    ContractNode& contract,
    const std::string& typeName
);
const StructDefNode* findStructDefinition(
    const ContractNode& contract,
    const std::string& typeName
);

bool matchesRuntimeFieldAlias(
    const std::string& candidateName,
    const std::string& expectedName
);
const RuntimeValue* findRuntimeFieldByContractName(
    const RuntimeValue::Struct& fields,
    const std::string& expectedName
);

RuntimeValue parseRuntimeArgument(
    const std::string& raw,
    const std::string& declaredType
);
RuntimeValue::Struct parseRuntimeFieldAssignments(
    const std::vector<std::string>& assignments,
    const std::string& optionName
);

RuntimeValue defaultRuntimeCompoundValue(
    const std::vector<CompoundFieldInfo>& fields
);
RuntimeValue defaultRuntimeValueForType(
    const std::string& typeName,
    ContractNode& contract
);
RuntimeValue defaultRuntimeValueForFieldType(
    const StructFieldType& fieldType,
    ContractNode& contract
);

RuntimeValue canonicalizeRuntimeValueForType(
    const RuntimeValue& value,
    const std::string& typeName,
    ContractNode& contract
);
RuntimeValue canonicalizeRuntimeValueForFieldType(
    const RuntimeValue& value,
    const StructFieldType& fieldType,
    ContractNode& contract
);

const RuntimeValue* findNamedRuntimeParameter(
    const RuntimeValue::Struct& namedValues,
    const ParameterInfo& parameter
);
RuntimeValue::Struct canonicalizeBvmFields(
    const RuntimeValue::Struct& fields
);

std::vector<RuntimeValue> buildRuntimeArgsForFunction(
    FunctionNode* function,
    const RuntimeValue::Struct& namedParamValues,
    const std::vector<std::string>& positionalArgs,
    bool allowPositionalArgs,
    ContractNode& contract
);

} // namespace apc_interpreter

#endif // RUNTIME_ARGUMENT_H
