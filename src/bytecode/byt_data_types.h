#ifndef BYT_DATA_TYPES_H
#define BYT_DATA_TYPES_H

#include "byt_defs.h"

SPACE_TBC_START

// 底层字节码操作类型 - Bitcoin Script 基础类型
enum class OpType {
    BYTES,
    INTEGER,
    BOOLEAN
};

// 高级语言类型 - 用户代码中可见的类型
enum class BytecodeType {
    Number,
    String,
    Boolean,
    Hex,

    FixedArray,

    // 加密相关类型 (映射到 BYTES)
    PubKey,
    Sig,
    Ripemd160,
    PubKeyHash,
    Sha1,
    Sha256,

    // Bitcoin 特定类型
    SigHashType,
    SigHashPreimage,
    OpCodeType,
    Addr,
    PrivKey,
};

// 高级类型 -> 底层 OpType 映射
class TypeMapper
{
public:
    static OpType mapToOpType(BytecodeType highLevelType)
    {
        switch (highLevelType) {
            case BytecodeType::Number:
            case BytecodeType::SigHashType:
            case BytecodeType::OpCodeType:
                return OpType::INTEGER;

            case BytecodeType::Boolean:
                return OpType::BOOLEAN;

            case BytecodeType::String:
            case BytecodeType::Hex:
            case BytecodeType::FixedArray:
            case BytecodeType::PubKey:
            case BytecodeType::Sig:
            case BytecodeType::Ripemd160:
            case BytecodeType::PubKeyHash:
            case BytecodeType::Sha1:
            case BytecodeType::Sha256:
            case BytecodeType::SigHashPreimage:
            case BytecodeType::Addr:
            case BytecodeType::PrivKey:
                return OpType::BYTES;

            default:
                return OpType::BYTES;
        }
    }

    static bool canMapTo(BytecodeType highLevelType, OpType opType)
    {
        return mapToOpType(highLevelType) == opType;
    }

    static std::string toString(BytecodeType type)
    {
        switch (type) {
            case BytecodeType::Number:
                return "number";
            case BytecodeType::String:
                return "string";
            case BytecodeType::Boolean:
                return "boolean";
            case BytecodeType::Hex:
                return "hex";
            case BytecodeType::FixedArray:
                return "array";
            case BytecodeType::PubKey:
                return "pubkey";
            case BytecodeType::Sig:
                return "signature";
            case BytecodeType::Ripemd160:
                return "ripemd160";
            case BytecodeType::PubKeyHash:
                return "pubkeyhash";
            case BytecodeType::Sha1:
                return "sha1";
            case BytecodeType::Sha256:
                return "sha256";
            case BytecodeType::SigHashType:
                return "sighashtype";
            case BytecodeType::SigHashPreimage:
                return "sighashpreimage";
            case BytecodeType::OpCodeType:
                return "opcodetype";
            case BytecodeType::Addr:
                return "address";
            case BytecodeType::PrivKey:
                return "privkey";
            default:
                return "unknown";
        }
    }

    static std::string toString(OpType type)
    {
        switch (type) {
            case OpType::BYTES:
                return "bytes";
            case OpType::INTEGER:
                return "integer";
            case OpType::BOOLEAN:
                return "boolean";
            default:
                return "unknown";
        }
    }
};

// 复合类型字段信息（多个模块共享）
struct CompoundFieldInfo
{
    std::string name;
    std::string type; // 例如 "hex20" 或 "uint64[3]"
    size_t byteSize;
    bool isArray;
    size_t arraySize;

    CompoundFieldInfo() = default;
    CompoundFieldInfo(
        const std::string& fieldName,
        const std::string& fieldType,
        size_t bytes,
        bool array = false,
        size_t arrSize = 0
    )
        : name(fieldName), type(fieldType), byteSize(bytes), isArray(array),
          arraySize(arrSize)
    {}
};

SPACE_TBC_END

#endif // BYT_DATA_TYPES_H