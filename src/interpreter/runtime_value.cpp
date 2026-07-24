#include "runtime_value.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#include "runtime_codec.h"
#include "runtime_error.h"

namespace apc_interpreter
{
bool RuntimeValue::BuiltinObject::operator==(const BuiltinObject& other) const
{
    return name == other.name && fields == other.fields;
}

RuntimeValue::RuntimeValue()
    : m_type(RuntimeType::Void), m_storage(std::monostate{}), m_declaredType("void")
{}

RuntimeValue::RuntimeValue(
    RuntimeType type,
    Storage storage,
    std::string declaredType
)
    : m_type(type),
      m_storage(std::move(storage)),
      m_declaredType(std::move(declaredType))
{}

RuntimeValue RuntimeValue::voidValue()
{
    return RuntimeValue();
}

RuntimeValue RuntimeValue::fromInt(int64_t value, std::string declaredType)
{
    return RuntimeValue(RuntimeType::Int, value, std::move(declaredType));
}

RuntimeValue RuntimeValue::fromBool(bool value)
{
    return RuntimeValue(RuntimeType::Bool, value, "bool");
}

RuntimeValue RuntimeValue::fromBytes(Bytes bytes, std::string declaredType)
{
    return RuntimeValue(
        RuntimeType::Bytes,
        std::move(bytes),
        std::move(declaredType)
    );
}

RuntimeValue RuntimeValue::fromHexString(
    const std::string& hex,
    std::string declaredType
)
{
    return fromBytes(runtime_codec::parseHex(hex), std::move(declaredType));
}

RuntimeValue RuntimeValue::fromString(std::string value)
{
    return RuntimeValue(RuntimeType::String, std::move(value), "string");
}

RuntimeValue RuntimeValue::fromAddress(std::string value)
{
    return RuntimeValue(RuntimeType::Address, std::move(value), "address");
}

RuntimeValue RuntimeValue::fromArray(Array values, std::string declaredType)
{
    return RuntimeValue(
        RuntimeType::Array,
        std::move(values),
        std::move(declaredType)
    );
}

RuntimeValue RuntimeValue::fromStruct(Struct fields, std::string declaredType)
{
    return RuntimeValue(
        RuntimeType::Struct,
        std::move(fields),
        std::move(declaredType)
    );
}

RuntimeValue RuntimeValue::fromBuiltinObject(std::string name, Struct fields)
{
    BuiltinObject object{std::move(name), std::move(fields)};
    return RuntimeValue(RuntimeType::BuiltinObject, std::move(object), "builtin");
}

void RuntimeValue::setDeclaredType(std::string declaredType)
{
    m_declaredType = std::move(declaredType);
}

int64_t RuntimeValue::toScriptNum() const
{
    switch (m_type) {
        case RuntimeType::Int:
            return std::get<int64_t>(m_storage);
        case RuntimeType::Bool:
            return std::get<bool>(m_storage) ? 1 : 0;
        case RuntimeType::Bytes:
            return runtime_codec::deserializeScriptNum(std::get<Bytes>(m_storage));
        case RuntimeType::Void:
            return 0;
        default:
            throw RuntimeError(
                RuntimeErrorKind::TypeMismatch,
                "runtime value cannot be converted to script number"
            );
    }
}

bool RuntimeValue::truthy() const
{
    switch (m_type) {
        case RuntimeType::Void:
            return false;
        case RuntimeType::Int:
            return std::get<int64_t>(m_storage) != 0;
        case RuntimeType::Bool:
            return std::get<bool>(m_storage);
        case RuntimeType::Bytes: {
            const auto& data = std::get<Bytes>(m_storage);
            return std::any_of(data.begin(), data.end(), [](uint8_t b) {
                return b != 0;
            });
        }
        case RuntimeType::String:
        case RuntimeType::Address:
            return !std::get<std::string>(m_storage).empty();
        case RuntimeType::Array:
            return !std::get<Array>(m_storage).empty();
        case RuntimeType::Struct:
            return !std::get<Struct>(m_storage).empty();
        case RuntimeType::BuiltinObject:
            return true;
    }
    return false;
}

RuntimeValue::Bytes RuntimeValue::toScriptBytes() const
{
    switch (m_type) {
        case RuntimeType::Void:
            return {};
        case RuntimeType::Int:
            return runtime_codec::serializeScriptNum(std::get<int64_t>(m_storage));
        case RuntimeType::Bool:
            return std::get<bool>(m_storage) ? Bytes{0x01} : Bytes{};
        case RuntimeType::Bytes:
            return std::get<Bytes>(m_storage);
        case RuntimeType::String:
        case RuntimeType::Address: {
            const auto& value = std::get<std::string>(m_storage);
            return Bytes(value.begin(), value.end());
        }
        case RuntimeType::Array: {
            Bytes out;
            for (const auto& item : std::get<Array>(m_storage)) {
                auto bytes = item.toScriptBytes();
                out.insert(out.end(), bytes.begin(), bytes.end());
            }
            return out;
        }
        case RuntimeType::Struct: {
            Bytes out;
            for (const auto& [_, field] : std::get<Struct>(m_storage)) {
                auto bytes = field.toScriptBytes();
                out.insert(out.end(), bytes.begin(), bytes.end());
            }
            return out;
        }
        case RuntimeType::BuiltinObject:
            throw RuntimeError(
                RuntimeErrorKind::TypeMismatch,
                "builtin object cannot be converted to script bytes"
            );
    }
    return {};
}

std::string RuntimeValue::toHexString(bool withPrefix) const
{
    return runtime_codec::bytesToHex(toScriptBytes(), withPrefix);
}

std::string RuntimeValue::toDisplayString() const
{
    switch (m_type) {
        case RuntimeType::Void:
            return "<void>";
        case RuntimeType::Int:
            return std::to_string(std::get<int64_t>(m_storage));
        case RuntimeType::Bool:
            return std::get<bool>(m_storage) ? "true" : "false";
        case RuntimeType::Bytes:
            return toHexString(true);
        case RuntimeType::String:
        case RuntimeType::Address:
            return std::get<std::string>(m_storage);
        case RuntimeType::Array:
            return "<array:" + std::to_string(std::get<Array>(m_storage).size()) +
                   ">";
        case RuntimeType::Struct:
            return "<struct:" + m_declaredType + ">";
        case RuntimeType::BuiltinObject:
            return "<builtin:" + std::get<BuiltinObject>(m_storage).name + ">";
    }
    return "<unknown>";
}

const RuntimeValue::Bytes& RuntimeValue::bytes() const
{
    if (m_type != RuntimeType::Bytes) {
        throw RuntimeError(RuntimeErrorKind::TypeMismatch, "value is not bytes");
    }
    return std::get<Bytes>(m_storage);
}

const std::string& RuntimeValue::stringValue() const
{
    if (m_type != RuntimeType::String && m_type != RuntimeType::Address) {
        throw RuntimeError(RuntimeErrorKind::TypeMismatch, "value is not string");
    }
    return std::get<std::string>(m_storage);
}

const RuntimeValue::Array& RuntimeValue::array() const
{
    if (m_type != RuntimeType::Array) {
        throw RuntimeError(RuntimeErrorKind::TypeMismatch, "value is not array");
    }
    return std::get<Array>(m_storage);
}

RuntimeValue::Array& RuntimeValue::array()
{
    if (m_type != RuntimeType::Array) {
        throw RuntimeError(RuntimeErrorKind::TypeMismatch, "value is not array");
    }
    return std::get<Array>(m_storage);
}

const RuntimeValue::Struct& RuntimeValue::structFields() const
{
    if (m_type != RuntimeType::Struct) {
        throw RuntimeError(RuntimeErrorKind::TypeMismatch, "value is not struct");
    }
    return std::get<Struct>(m_storage);
}

RuntimeValue::Struct& RuntimeValue::structFields()
{
    if (m_type != RuntimeType::Struct) {
        throw RuntimeError(RuntimeErrorKind::TypeMismatch, "value is not struct");
    }
    return std::get<Struct>(m_storage);
}

const RuntimeValue::BuiltinObject& RuntimeValue::builtinObject() const
{
    if (m_type != RuntimeType::BuiltinObject) {
        throw RuntimeError(
            RuntimeErrorKind::TypeMismatch,
            "value is not builtin object"
        );
    }
    return std::get<BuiltinObject>(m_storage);
}

bool RuntimeValue::operator==(const RuntimeValue& other) const
{
    if (m_type == RuntimeType::Int || m_type == RuntimeType::Bool ||
        m_type == RuntimeType::Bytes || m_type == RuntimeType::Void) {
        if (other.m_type == RuntimeType::Int || other.m_type == RuntimeType::Bool ||
            other.m_type == RuntimeType::Bytes || other.m_type == RuntimeType::Void) {
            return toScriptBytes() == other.toScriptBytes();
        }
    }

    if (m_type != other.m_type) {
        return false;
    }

    return m_storage == other.m_storage;
}

std::string RuntimeValue::typeName(RuntimeType type)
{
    switch (type) {
        case RuntimeType::Void:
            return "void";
        case RuntimeType::Int:
            return "int";
        case RuntimeType::Bool:
            return "bool";
        case RuntimeType::Bytes:
            return "bytes";
        case RuntimeType::String:
            return "string";
        case RuntimeType::Address:
            return "address";
        case RuntimeType::Array:
            return "array";
        case RuntimeType::Struct:
            return "struct";
        case RuntimeType::BuiltinObject:
            return "builtin";
    }
    return "unknown";
}

} // namespace apc_interpreter
