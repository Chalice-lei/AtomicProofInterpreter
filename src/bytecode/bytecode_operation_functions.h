#ifndef BYTECODE_OPERATION_FUNCTIONS_H
#define BYTECODE_OPERATION_FUNCTIONS_H

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "byt_data_types.h"
#include "byt_defs.h"
#include "bytecode_base_function.h"
#include "bytecode_opcodes.h"

SPACE_TBC_START

// 单字节码操作的函数基类
class OpFunction : public BytecodeFunction
{
public:
    virtual ~OpFunction() = default;

    virtual tbc::BytOpcode getOpcodeEnum() const = 0;

    virtual std::string getOpcodeHex() const
    {
        return opcodeToHex(getOpcodeEnum());
    }

    virtual bool isArgOrderSensitive() const = 0;
};

template <tbc::BytOpcode opCode,
          size_t argCount,
          bool isSensitive = false,
          size_t returnCount = 0>
class OpFunctionTemplate : public OpFunction
{
private:
    std::vector<tbc::OpType> returnTypes;
    std::vector<tbc::OpType> inputTypes;

public:
    OpFunctionTemplate(const std::vector<tbc::OpType>& retTypes,
                       const std::vector<tbc::OpType>& inTypes)
        : returnTypes(retTypes), inputTypes(inTypes)
    {}

    tbc::BytOpcode getOpcodeEnum() const override
    {
        return opCode;
    }

    size_t getExpectedArgCount() const override
    {
        return argCount;
    }

    bool isArgOrderSensitive() const override
    {
        return isSensitive;
    }

    size_t getReturnCount() const override
    {
        return returnCount;
    }

    std::vector<tbc::OpType> getReturnTypes() const override
    {
        return returnTypes;
    }

    std::vector<tbc::OpType> getInputTypes() const override
    {
        return inputTypes;
    }
};

// 注意: OP_*VERIFY 系列 (CHECKSIGVERIFY/CHECKMULTISIGVERIFY/EQUALVERIFY/
// NUMEQUALVERIFY) 不向栈返回值
static const std::unordered_map<std::string,
                                std::shared_ptr<OpFunction> (*)(size_t)>
    opCreators = {
        // 加密哈希: BYTES -> BYTES
        {"Rmd160",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_RIPEMD160, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES});
         }},
        {"Sha1",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_SHA1, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES});
         }},
        {"Sha256",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_SHA256, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES});
         }},
        {"Hash160",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_HASH160, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES});
         }},
        {"Hash256",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_HASH256, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES});
         }},
        {"PartialHash",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (3 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_PARTIAL_HASH, 3, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES,
                                          tbc::OpType::BYTES,
                                          tbc::OpType::BYTES});
         }},
        // 签名验证: (sig, pubkey)
        {"CheckSig",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_CHECKSIG, 2, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES,
                                          tbc::OpType::BYTES});
         }},
        {"CheckSigVerify",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_CHECKSIGVERIFY,
                                    2,
                                    true,
                                    0>>(
                 std::vector<tbc::OpType>{},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES,
                                          tbc::OpType::BYTES});
         }},
        // 多签验证: (sigs, pubkeys)
        {"MultiSig",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_CHECKMULTISIG,
                                    2,
                                    true,
                                    1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES,
                                          tbc::OpType::BYTES});
         }},
        {"MultiSigVerify",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_CHECKMULTISIGVERIFY,
                                    2,
                                    true,
                                    0>>(
                 std::vector<tbc::OpType>{},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES,
                                          tbc::OpType::BYTES});
         }},
        // 一元算术: INTEGER -> INTEGER
        {"Inc",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_1ADD, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER});
         }},
        {"Dec",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_1SUB, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER});
         }},
        {"Neg",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_NEGATE, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER});
         }},
        {"Abs",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_ABS, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER});
         }},
        {"Not",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_NOT, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER});
         }},
        {"ZeroNotEqual",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_0NOTEQUAL, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER});
         }},
        // 二元算术: (INTEGER, INTEGER) -> INTEGER
        {"Add",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_ADD, 2, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"Sub",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_SUB, 2, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"Mul",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_MUL, 2, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"Div",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_DIV, 2, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"Mod",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_MOD, 2, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        // 位移
        {"Lshift",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_LSHIFT, 2, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"Rshift",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_RSHIFT, 2, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        // 等值: (BYTES, BYTES) -> BOOLEAN / 无
        {"Equal",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_EQUAL, 2, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES,
                                          tbc::OpType::BYTES});
         }},
        {"EqualVerify",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_EQUALVERIFY,
                                    2,
                                    false,
                                    0>>(
                 std::vector<tbc::OpType>{},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES,
                                          tbc::OpType::BYTES});
         }},
        // 数值比较: (INTEGER, INTEGER) -> BOOLEAN / 无
        {"NumEqual",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_NUMEQUAL, 2, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"NumEqualVerify",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_NUMEQUALVERIFY,
                                    2,
                                    false,
                                    0>>(
                 std::vector<tbc::OpType>{},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"NumNotEqual",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_NUMNOTEQUAL,
                                    2,
                                    false,
                                    1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"LessThan",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_LESSTHAN, 2, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"GreaterThan",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_GREATERTHAN,
                                    2,
                                    true,
                                    1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"LessOrEqual",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_LESSTHANOREQUAL,
                                    2,
                                    true,
                                    1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"GreaterOrEqual",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_GREATERTHANOREQUAL,
                                    2,
                                    true,
                                    1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        // 最值
        {"Min",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_MIN, 2, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        {"Max",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_MAX, 2, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        // 范围检查: (x, min, max) -> BOOLEAN
        {"Within",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (3 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_WITHIN, 3, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        // 字节串拼接 / 分割
        {"Cat",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_CAT, 2, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES,
                                          tbc::OpType::BYTES});
         }},
        {"Split",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_SPLIT, 2, true, 2>>(
                 std::vector<tbc::OpType>{tbc::OpType::BYTES,
                                          tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES,
                                          tbc::OpType::INTEGER});
         }},
        // 数字 -> 字节数组: (int, size) -> BYTES
        {"NumToBin",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_NUM2BIN, 2, true, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER,
                                          tbc::OpType::INTEGER});
         }},
        // 字节数组 -> 整数
        {"BinToNum",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_BIN2NUM, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES});
         }},
        {"Size",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (1 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_SIZE, 1, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::INTEGER},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES});
         }},
        // 逻辑: BOOLEAN -> BOOLEAN
        {"And",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_BOOLAND, 2, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN,
                                          tbc::OpType::BOOLEAN});
         }},
        {"Or",
         [](size_t size) -> std::shared_ptr<OpFunction> {
             if (2 != size) {
                 return nullptr;
             }
             return std::make_shared<
                 OpFunctionTemplate<tbc::BytOpcode::OP_BOOLOR, 2, false, 1>>(
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN},
                 std::vector<tbc::OpType>{tbc::OpType::BOOLEAN,
                                          tbc::OpType::BOOLEAN});
         }},
};

// 函数别名
static const std::unordered_map<std::string, std::string> opFunctionAliases = {
    {"Ripemd160", "Rmd160"},
    {"Sha256hash", "Sha256"},
    {"Increment", "Inc"},
    {"Decrement", "Dec"},
    {"Negate", "Neg"},
    {"Absolute", "Abs"},
    {"Addition", "Add"},
    {"Subtract", "Sub"},
    {"Multiply", "Mul"},
    {"Divide", "Div"},
    {"Modulo", "Mod"},
    {"Leftshift", "Lshift"},
    {"Rightshift", "Rshift"},
    {"Require", "EqualVerify"},
};

// 操作码函数工厂
class OpFunctionFactory : public BytecodeFunctionFactory
{
public:
    static std::shared_ptr<OpFunction> createFunction(const std::string& name,
                                                      const size_t argCount)
    {
        auto it = opCreators.find(name);
        if (it != opCreators.end()) {
            return it->second(argCount);
        }

        // 别名 fallback
        auto aliasIt = opFunctionAliases.find(name);
        if (aliasIt != opFunctionAliases.end()) {
            auto originalIt = opCreators.find(aliasIt->second);
            if (originalIt != opCreators.end()) {
                return originalIt->second(argCount);
            }
        }

        return nullptr;
    }

    static bool isOpFunction(const std::string& name)
    {
        if (opCreators.find(name) != opCreators.end() ||
            opFunctionAliases.find(name) != opFunctionAliases.end()) {
            return true;
        }
        return false;
    }

    // 别名 -> 原始函数名
    static std::string getOriginalName(const std::string& name)
    {
        auto aliasIt = opFunctionAliases.find(name);
        return (aliasIt != opFunctionAliases.end()) ? aliasIt->second : name;
    }

    // 非内置则返回 OP_INVALIDOPCODE
    static std::string getOpcodeHex(const std::string& name,
                                    const size_t argCount)
    {
        auto func = createFunction(name, argCount);

        return func ? func->getOpcodeHex()
                    : opcodeToHex(tbc::BytOpcode::OP_INVALIDOPCODE);
    }

    static bool validateFunctionArgs(const std::string& name, size_t argCount)
    {
        auto func = createFunction(name, argCount);
        if (func) {
            return true;
        }
        return false;
    }

    static std::vector<tbc::OpType> getFunctionReturnTypes(
        const std::string& name,
        size_t argCount)
    {
        auto func = createFunction(name, argCount);
        return func ? func->getReturnTypes() : std::vector<tbc::OpType>{};
    }

    static std::vector<std::string> getFunctionReturnTypeStrings(
        const std::string& name,
        size_t argCount)
    {
        return getOpTypeStrings(getFunctionReturnTypes(name, argCount));
    }

    static std::vector<tbc::OpType> getFunctionInputTypes(
        const std::string& name,
        size_t argCount)
    {
        auto func = createFunction(name, argCount);
        return func ? func->getInputTypes() : std::vector<tbc::OpType>{};
    }

    static std::vector<std::string> getFunctionInputTypeStrings(
        const std::string& name,
        size_t argCount)
    {
        return getOpTypeStrings(getFunctionInputTypes(name, argCount));
    }
};

SPACE_TBC_END

#endif // BYTECODE_OPERATION_FUNCTIONS_H
