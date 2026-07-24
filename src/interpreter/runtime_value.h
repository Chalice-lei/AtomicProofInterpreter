#ifndef RUNTIME_VALUE_H
#define RUNTIME_VALUE_H

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace apc_interpreter
{

enum class RuntimeType {
    Void,
    Int,
    Bool,
    Bytes,
    String,
    Address,
    Array,
    Struct,
    BuiltinObject
};

class RuntimeValue
{
public:
    using Bytes = std::vector<uint8_t>;
    using Array = std::vector<RuntimeValue>;
    using Struct = std::map<std::string, RuntimeValue>;

    struct BuiltinObject
    {
        std::string name;
        Struct fields;

        bool operator==(const BuiltinObject& other) const;
    };

    RuntimeValue();

    static RuntimeValue voidValue();
    static RuntimeValue fromInt(int64_t value, std::string declaredType = "int");
    static RuntimeValue fromBool(bool value);
    static RuntimeValue fromBytes(
        Bytes bytes,
        std::string declaredType = "bytes"
    );
    static RuntimeValue fromHexString(
        const std::string& hex,
        std::string declaredType = "hex"
    );
    static RuntimeValue fromString(std::string value);
    static RuntimeValue fromAddress(std::string value);
    static RuntimeValue fromArray(
        Array values,
        std::string declaredType = "array"
    );
    static RuntimeValue fromStruct(
        Struct fields,
        std::string declaredType
    );
    static RuntimeValue fromBuiltinObject(
        std::string name,
        Struct fields = {}
    );

    RuntimeType type() const
    {
        return m_type;
    }

    const std::string& declaredType() const
    {
        return m_declaredType;
    }

    void setDeclaredType(std::string declaredType);

    int64_t toScriptNum() const;
    int64_t toInt() const
    {
        return toScriptNum();
    }
    bool truthy() const;
    Bytes toScriptBytes() const;
    std::string toHexString(bool withPrefix = true) const;
    std::string toDisplayString() const;

    const Bytes& bytes() const;
    const std::string& stringValue() const;
    const Array& array() const;
    Array& array();
    const Struct& structFields() const;
    Struct& structFields();
    const BuiltinObject& builtinObject() const;

    bool isVoid() const
    {
        return m_type == RuntimeType::Void;
    }

    bool operator==(const RuntimeValue& other) const;
    bool operator!=(const RuntimeValue& other) const
    {
        return !(*this == other);
    }

    static std::string typeName(RuntimeType type);

private:
    using Storage = std::variant<
        std::monostate,
        int64_t,
        bool,
        Bytes,
        std::string,
        Array,
        Struct,
        BuiltinObject>;

    RuntimeValue(RuntimeType type, Storage storage, std::string declaredType);

    RuntimeType m_type;
    Storage m_storage;
    std::string m_declaredType;
};

} // namespace apc_interpreter

#endif // RUNTIME_VALUE_H
