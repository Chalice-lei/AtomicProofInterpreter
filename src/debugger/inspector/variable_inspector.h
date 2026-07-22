#ifndef VARIABLE_INSPECTOR_H
#define VARIABLE_INSPECTOR_H

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <map>

#include "../info/debug_info.h"
#include "../vm/stack_state.h"

namespace apc_debug {

class BVMSimulator;

struct VariableValue {
    std::string name;
    std::string type;
    std::string value;          // 字符串表示
    std::string rawValue;       // 原始十六进制
    int stackOffset;            // -1 表示不在栈上
    bool isValid;

    std::vector<VariableValue> fields;   // 结构体字段
    std::vector<VariableValue> elements; // 数组元素

    VariableValue()
        : stackOffset(-1), isValid(false) {}
    
    VariableValue(const std::string& n, const std::string& t,
                  const std::string& v, int offset = -1)
        : name(n), type(t), value(v), stackOffset(offset), isValid(true) {}

    std::string toString(int indent = 0) const;
    std::string toJSON() const;
};

/**
 * @brief 从栈状态读取变量值，解析基本/数组/结构体类型并格式化。
 */
class VariableInspector {
public:
    explicit VariableInspector(std::shared_ptr<DebugInfo> debugInfo);
    ~VariableInspector() = default;

    std::optional<VariableValue> readVariable(
        const std::string& varName,
        const StackState& stack,
        size_t currentPC
    );

    std::optional<VariableValue> readStackValue(
        int stackOffset,
        const std::string& varName,
        const std::string& varType,
        const StackState& stack
    );

    std::vector<VariableValue> getAllVariables(
        const StackState& stack,
        size_t currentPC
    );

    std::vector<VariableValue> getLocalVariables(
        const StackState& stack,
        size_t currentPC
    );

    std::vector<VariableValue> getGlobalVariables(
        const StackState& stack,
        size_t currentPC
    );

    std::optional<VariableValue> parseCompoundType(
        const std::string& varName,
        const std::string& varType,
        const StackElement& element
    );

    std::optional<VariableValue> parseArrayType(
        const std::string& varName,
        const std::string& varType,
        const StackElement& element
    );

    std::string formatValue(
        const std::string& rawValue,
        const std::string& type
    );

    std::shared_ptr<DebugInfo> getDebugInfo() const { return m_debugInfo; }

private:
    std::shared_ptr<DebugInfo> m_debugInfo;

    std::string parseBasicType(const std::string& rawValue, const std::string& type);
    std::string extractRawValue(const StackElement& element, const std::string& varType);

    bool isBasicType(const std::string& type);
    bool isArrayType(const std::string& type);
    bool isCompoundType(const std::string& type);

    // "type[size]"
    struct ArrayTypeInfo {
        std::string elementType;
        size_t size;
    };
    std::optional<ArrayTypeInfo> parseArrayTypeName(const std::string& type);
};

} // namespace apc_debug

#endif // VARIABLE_INSPECTOR_H


