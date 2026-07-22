#include "variable_inspector.h"

#include <algorithm>

#include "../../util/type_utils.h"

namespace apc_debug
{
namespace
{
bool isLikelyText(const std::string& s)
{
    if (s.empty()) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 32 && c <= 126);
    });
}

VariableValue makeUnavailableValue(
    const std::string& varName,
    const std::string& varType
)
{
    VariableValue result;
    result.name = varName;
    result.type = varType;
    result.value = "<unavailable>";
    result.stackOffset = -1;
    result.isValid = false;
    return result;
}

const VariableDebugInfo*
findFunctionVariable(const FunctionDebugInfo& func, const std::string& varName)
{
    for (const auto& local : func.localVars) {
        if (local.name == varName) {
            return &local;
        }
    }
    for (const auto& param : func.parameters) {
        if (param.name == varName) {
            return &param;
        }
    }
    return nullptr;
}

std::vector<VariableDebugInfo> collectProjectedVariables(
    const FunctionDebugInfo& func,
    size_t currentLine,
    size_t currentPC
)
{
    size_t firstLocalLine = 0;
    std::vector<VariableDebugInfo> orderedVars;

    for (const auto& localVar : func.localVars) {
        if (firstLocalLine == 0 || localVar.declLine < firstLocalLine) {
            firstLocalLine = localVar.declLine;
        }
        if (currentLine == 0 || localVar.declLine < currentLine) {
            orderedVars.push_back(localVar);
        }
    }

    if (orderedVars.empty() && currentLine > 0 &&
        (firstLocalLine == 0 || currentLine <= firstLocalLine) &&
        currentPC == func.startPC) {
        orderedVars = func.parameters;
    }

    return orderedVars;
}
} // namespace


VariableInspector::VariableInspector(std::shared_ptr<DebugInfo> debugInfo)
    : m_debugInfo(debugInfo)
{}

std::string VariableInspector::formatValue(
    const std::string& rawValue,
    const std::string& type
)
{
    if (type == "int" || type == "number" || type == "Number") {
        if (rawValue.empty()) {
            return "0";
        }
        return rawValue;
    } else if (type == "bool" || type == "boolean" || type == "Boolean") {
        return (rawValue == "1" || rawValue == "true") ? "true" : "false";
    } else if (type == "string" || type == "String") {
        return "\"" + rawValue + "\"";
    } else if (type == "address" || type == "Address") {
        // 优先可读文本，否则按 pubkeyhash 语义
        if (isLikelyText(rawValue)) {
            return "\"" + rawValue + "\"";
        }
        if (rawValue.rfind("pubkeyhash:0x", 0) == 0) {
            return rawValue;
        }
        return "\"" + rawValue + "\"";
    } else if (type == "hex" || type == "Hex") {
        return "0x" + rawValue;
    } else if (type.find("hex") == 0) {
        // hexN
        return "0x" + rawValue;
    } else {
        return rawValue;
    }
}

std::optional<VariableValue> VariableInspector::readVariable(
    const std::string& varName,
    const StackState& stack,
    size_t currentPC
)
{
    if (!m_debugInfo) {
        return std::nullopt;
    }

    const auto* func = m_debugInfo->getFunctionAtPC(currentPC);
    if (func) {
        SourceLocation loc = m_debugInfo->getSourceLocation(currentPC);
        std::vector<VariableDebugInfo> orderedVars =
            collectProjectedVariables(*func, loc.line, currentPC);

        if (!orderedVars.empty() && orderedVars.size() <= stack.size()) {
            size_t firstStackIndex = stack.size() - orderedVars.size();
            for (size_t i = 0; i < orderedVars.size(); ++i) {
                if (orderedVars[i].name != varName) {
                    continue;
                }
                size_t bottomIndex = firstStackIndex + i;
                size_t depth = stack.size() - 1 - bottomIndex;
                return readStackValue(
                    static_cast<int>(depth),
                    orderedVars[i].name,
                    orderedVars[i].type,
                    stack
                );
            }
        }

        const auto* funcVar = findFunctionVariable(*func, varName);
        if (funcVar) {
            return makeUnavailableValue(varName, funcVar->type);
        }
    }

    const auto* varInfo = m_debugInfo->getVariableInfo(varName);
    if (!varInfo) {
        return std::nullopt;
    }

    if (func && varInfo->isParameter) {
        SourceLocation loc = m_debugInfo->getSourceLocation(currentPC);
        size_t firstLocalLine = 0;
        for (const auto& localVar : func->localVars) {
            if (firstLocalLine == 0 || localVar.declLine < firstLocalLine) {
                firstLocalLine = localVar.declLine;
            }
        }
        if (firstLocalLine > 0 && loc.line > firstLocalLine) {
            return makeUnavailableValue(varName, varInfo->type);
        }
    }

    if (varInfo->isStackVar && varInfo->stackOffset >= 0) {
        return readStackValue(
            varInfo->stackOffset, varName, varInfo->type, stack
        );
    }

    // 非栈变量暂返回无效值
    return makeUnavailableValue(varName, varInfo->type);
}

std::optional<VariableValue> VariableInspector::readStackValue(
    int stackOffset,
    const std::string& varName,
    const std::string& varType,
    const StackState& stack
)
{
    if (stackOffset < 0 || stackOffset >= static_cast<int>(stack.size())) {
        return std::nullopt;
    }

    StackElement element = stack.peek(static_cast<size_t>(stackOffset));

    VariableValue result;
    result.name = varName;
    result.type = varType;
    result.rawValue = extractRawValue(element, varType);
    result.value = formatValue(result.rawValue, varType);
    result.stackOffset = stackOffset;
    result.isValid = true;

    if (isArrayType(varType)) {
        auto arrayValue = parseArrayType(varName, varType, element);
        if (arrayValue) {
            return arrayValue;
        }
    } else if (isCompoundType(varType)) {
        auto compoundValue = parseCompoundType(varName, varType, element);
        if (compoundValue) {
            return compoundValue;
        }
    }

    return result;
}

std::vector<VariableValue>
VariableInspector::getAllVariables(const StackState& stack, size_t currentPC)
{
    std::vector<VariableValue> result;

    if (!m_debugInfo) {
        return result;
    }

    auto scopedVars = m_debugInfo->getVariablesInScope(currentPC);
    if (!scopedVars.empty()) {
        for (const auto& varInfo : scopedVars) {
            auto varValue = readVariable(varInfo.name, stack, currentPC);
            if (varValue) {
                result.push_back(*varValue);
            }
        }
        return result;
    }

    // 作用域查询失败：退化遍历全局变量表
    for (const auto& [name, varInfo] : m_debugInfo->variables) {
        (void)varInfo;
        auto varValue = readVariable(name, stack, currentPC);
        if (varValue) {
            result.push_back(*varValue);
        }
    }

    return result;
}

std::vector<VariableValue>
VariableInspector::getLocalVariables(const StackState& stack, size_t currentPC)
{
    std::vector<VariableValue> result;

    if (!m_debugInfo) {
        return result;
    }

    const auto* func = m_debugInfo->getFunctionAtPC(currentPC);
    if (func) {
        SourceLocation loc = m_debugInfo->getSourceLocation(currentPC);
        std::vector<VariableDebugInfo> orderedVars =
            collectProjectedVariables(*func, loc.line, currentPC);

        if (!orderedVars.empty() && orderedVars.size() <= stack.size()) {
            size_t firstStackIndex = stack.size() - orderedVars.size();
            for (size_t i = 0; i < orderedVars.size(); ++i) {
                const auto& varInfo = orderedVars[i];
                size_t bottomIndex = firstStackIndex + i;
                size_t depth = stack.size() - 1 - bottomIndex;
                auto varValue = readStackValue(
                    static_cast<int>(depth),
                    varInfo.name,
                    varInfo.type,
                    stack
                );
                if (varValue) {
                    result.push_back(*varValue);
                }
            }
            return result;
        }

        for (const auto& varInfo : orderedVars) {
            result.push_back(makeUnavailableValue(varInfo.name, varInfo.type));
        }
        if (!result.empty()) {
            return result;
        }
    }

    auto scope = m_debugInfo->getScopeAtPC(currentPC);
    if (!scope) {
        return result;
    }

    for (const auto& varInfo : scope->variables) {
        auto varValue = readVariable(varInfo.name, stack, currentPC);
        if (varValue) {
            result.push_back(*varValue);
        }
    }

    return result;
}

std::vector<VariableValue>
VariableInspector::getGlobalVariables(const StackState& stack, size_t currentPC)
{
    (void)currentPC;
    std::vector<VariableValue> result;
    if (!m_debugInfo || !m_debugInfo->globalScope) {
        return result;
    }

    for (const auto& varInfo : m_debugInfo->globalScope->variables) {
        if (varInfo.isStackVar && varInfo.stackOffset >= 0) {
            auto value = readStackValue(
                varInfo.stackOffset,
                varInfo.name,
                varInfo.type,
                stack
            );
            result.push_back(
                value ? *value
                      : makeUnavailableValue(varInfo.name, varInfo.type)
            );
        } else {
            result.push_back(makeUnavailableValue(varInfo.name, varInfo.type));
        }
    }
    return result;
}

std::optional<VariableValue> VariableInspector::parseCompoundType(
    const std::string& varName,
    const std::string& varType,
    const StackElement& element
)
{
    // 复合类型可能跨多个栈元素；当前仅返回原始值
    VariableValue result;
    result.name = varName;
    result.type = varType;
    result.rawValue = extractRawValue(element, varType);
    result.value = formatValue(result.rawValue, varType);
    result.isValid = true;
    result.stackOffset = -1;

    // TODO: 从 DebugInfo 取结构体定义并解析字段

    return result;
}

std::optional<VariableValue> VariableInspector::parseArrayType(
    const std::string& varName,
    const std::string& varType,
    const StackElement& element
)
{
    auto arrayInfo = parseArrayTypeName(varType);
    if (!arrayInfo) {
        return std::nullopt;
    }

    VariableValue result;
    result.name = varName;
    result.type = varType;
    result.rawValue = extractRawValue(element, varType);
    result.value = formatValue(result.rawValue, varType);
    result.isValid = true;
    result.stackOffset = -1; // 数组可能跨多个栈元素

    // TODO: 解析数组元素

    return result;
}

std::string VariableInspector::parseBasicType(
    const std::string& rawValue,
    const std::string& type
)
{
    return formatValue(rawValue, type);
}

std::string VariableInspector::extractRawValue(
    const StackElement& element,
    const std::string& varType
)
{
    // 原始值语义按类型分发；hex/hexN 不带 0x（formatValue 会补）
    if (varType == "int" || varType == "number" || varType == "Number") {
        auto v = element.toInt();
        return v ? std::to_string(*v) : "0";
    }

    if (varType == "bool" || varType == "boolean" || varType == "Boolean") {
        auto v = element.toInt();
        return (v && *v != 0) ? "1" : "0";
    }

    if (varType == "string" || varType == "String") {
        return std::string(element.data.begin(), element.data.end());
    }

    if (varType == "address" || varType == "Address") {
        // 编译器地址字面量：0x14 + 20 字节 pubkeyhash
        if (element.data.size() == 21 && element.data[0] == 0x14) {
            StackElement hashElement(
                std::vector<uint8_t>(element.data.begin() + 1, element.data.end())
            );
            return "pubkeyhash:0x" + hashElement.toHexString(false);
        }
        // 兼容旧行为
        return std::string(element.data.begin(), element.data.end());
    }

    if (varType == "hex" || varType == "Hex" || varType.rfind("hex", 0) == 0) {
        return element.toHexString(false);
    }

    return element.toHexString(true);
}

bool VariableInspector::isBasicType(const std::string& type)
{
    static const std::vector<std::string> basicTypes =
        {"int",
         "number",
         "Number",
         "bool",
         "boolean",
         "Boolean",
         "string",
         "String",
         "address",
         "Address",
         "hex",
         "Hex"};

    if (std::find(basicTypes.begin(), basicTypes.end(), type) !=
        basicTypes.end()) {
        return true;
    }

    // hexN
    if (type.find("hex") == 0 && type.length() > 3) {
        return std::all_of(type.begin() + 3, type.end(), ::isdigit);
    }

    return false;
}

bool VariableInspector::isArrayType(const std::string& type)
{
    return apc::util::isFixedArrayType(type);
}

bool VariableInspector::isCompoundType(const std::string& type)
{
    return !isBasicType(type) && !isArrayType(type);
}

std::optional<VariableInspector::ArrayTypeInfo>
VariableInspector::parseArrayTypeName(const std::string& type)
{
    auto arrayType = apc::util::parseFixedArrayType(type);
    if (!arrayType) {
        return std::nullopt;
    }

    return ArrayTypeInfo{arrayType->elementType, arrayType->size};
}

} // namespace apc_debug
