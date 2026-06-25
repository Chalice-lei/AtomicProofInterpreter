#ifndef BYTECODE_OPCODES_H
#define BYTECODE_OPCODES_H

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <string>
#include <unordered_map>

#include "byt_defs.h"

SPACE_TBC_START

// Bitcoin Script 操作码 (基于标准 Bitcoin Script)
enum class BytOpcode : uint16_t {
    // 常量
    OP_0 = 0x00,
    OP_FALSE = OP_0,
    OP_PUSHDATA1 = 0x4c,
    OP_PUSHDATA2 = 0x4d,
    OP_PUSHDATA4 = 0x4e,
    OP_1NEGATE = 0x4f,
    OP_1 = 0x51,
    OP_TRUE = OP_1,
    OP_2 = 0x52,
    OP_3 = 0x53,
    OP_4 = 0x54,
    OP_5 = 0x55,
    OP_6 = 0x56,
    OP_7 = 0x57,
    OP_8 = 0x58,
    OP_9 = 0x59,
    OP_10 = 0x5a,
    OP_11 = 0x5b,
    OP_12 = 0x5c,
    OP_13 = 0x5d,
    OP_14 = 0x5e,
    OP_15 = 0x5f,
    OP_16 = 0x60,

    // 控制流
    OP_NOP = 0x61,
    OP_IF = 0x63,
    OP_NOTIF = 0x64,
    OP_ELSE = 0x67,
    OP_ENDIF = 0x68,
    OP_VERIFY = 0x69,
    OP_RETURN = 0x6a,

    // 栈操作
    OP_TOALTSTACK = 0x6b,
    OP_FROMALTSTACK = 0x6c,
    OP_2DROP = 0x6d,
    OP_2DUP = 0x6e,
    OP_3DUP = 0x6f,
    OP_2OVER = 0x70,
    OP_2ROT = 0x71,
    OP_2SWAP = 0x72,
    OP_IFDUP = 0x73,
    OP_DEPTH = 0x74,
    OP_DROP = 0x75,
    OP_DUP = 0x76,
    OP_NIP = 0x77,
    OP_OVER = 0x78,
    OP_PICK = 0x79,
    OP_ROLL = 0x7a,
    OP_ROT = 0x7b,
    OP_SWAP = 0x7c,
    OP_TUCK = 0x7d,

    // 数据操作
    OP_CAT = 0x7e,
    OP_SPLIT = 0x7f,
    OP_NUM2BIN = 0x80,
    OP_BIN2NUM = 0x81,
    OP_SIZE = 0x82,

    // 位运算
    OP_INVERT = 0x83,
    OP_AND = 0x84,
    OP_OR = 0x85,
    OP_XOR = 0x86,

    // 比较操作符
    OP_EQUAL = 0x87,
    OP_EQUALVERIFY = 0x88,

    // 算术运算
    OP_1ADD = 0x8b,
    OP_1SUB = 0x8c,
    OP_NEGATE = 0x8f,
    OP_ABS = 0x90,
    OP_NOT = 0x91,
    OP_0NOTEQUAL = 0x92,
    OP_ADD = 0x93,
    OP_SUB = 0x94,
    OP_MUL = 0x95,
    OP_DIV = 0x96,
    OP_MOD = 0x97,
    OP_LSHIFT = 0x98,
    OP_RSHIFT = 0x99,

    // 逻辑
    OP_BOOLAND = 0x9a,
    OP_BOOLOR = 0x9b,

    // 数值比较
    OP_NUMEQUAL = 0x9c,
    OP_NUMEQUALVERIFY = 0x9d,
    OP_NUMNOTEQUAL = 0x9e,
    OP_LESSTHAN = 0x9f,
    OP_GREATERTHAN = 0xa0,
    OP_LESSTHANOREQUAL = 0xa1,
    OP_GREATERTHANOREQUAL = 0xa2,
    OP_MIN = 0xa3,
    OP_MAX = 0xa4,
    OP_WITHIN = 0xa5,

    // 加密
    OP_RIPEMD160 = 0xa6,
    OP_SHA1 = 0xa7,
    OP_SHA256 = 0xa8,
    OP_HASH160 = 0xa9,
    OP_HASH256 = 0xaa,
    OP_CODESEPARATOR = 0xab,
    OP_CHECKSIG = 0xac,
    OP_CHECKSIGVERIFY = 0xad,
    OP_CHECKMULTISIG = 0xae,
    OP_CHECKMULTISIGVERIFY = 0xaf,

    // 时间锁
    OP_CHECKLOCKTIMEVERIFY = 0xb1,
    OP_CHECKSEQUENCEVERIFY = 0xb2,

    // 扩展操作码
    OP_PUSH_META = 0xba,
    OP_PARTIAL_HASH = 0xbb,

    OP_INVALIDOPCODE = 0xff,
};

// 操作码 ↔ 字符串映射
class OpcodeMapper
{
public:
    static const std::unordered_map<BytOpcode, std::string> g_opcodeToString;
    static const std::unordered_map<std::string, BytOpcode> g_stringToOpcode;

    static std::string toString(BytOpcode opcode)
    {
        auto it = g_opcodeToString.find(opcode);
        if (it != g_opcodeToString.end()) {
            return it->second;
        }

        return "UNKNOWN_OPCODE";
    }

    static BytOpcode fromString(const std::string& opcodeStr)
    {
        auto it = g_stringToOpcode.find(opcodeStr);
        if (it != g_stringToOpcode.end()) {
            return it->second;
        }

        return BytOpcode::OP_FALSE; // 默认
    }
};

[[maybe_unused]] static std::string opcodeToHex(const BytOpcode bytOpcode)
{
    uint16_t opcodeValue = static_cast<uint16_t>(bytOpcode);
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(2) << opcodeValue;
    return ss.str();
}

[[maybe_unused]] static bool isScript(const std::string& str)
{
    if (str.empty()) {
        return false;
    }

    // <...> 包裹的占位符
    if (2 < str.size() && '<' == str.front() && '>' == str.back()) {
        return true;
    }

    // 0x 开头的十六进制
    if (str.length() >= 2 &&
        (str.substr(0, 2) == "0x" || str.substr(0, 2) == "0X")) {
        std::string hexData = str.substr(2);
        if (!hexData.empty() &&
            std::all_of(hexData.begin(), hexData.end(), [](char c) {
                return std::isxdigit(c);
            })) {
            return true;
        }
    }

    return false;
}

SPACE_TBC_END
#endif // BYTECODE_OPCODES_H