#include "bytecode_opcodes.h"

SPACE_TBC_START

const std::unordered_map<BytOpcode, std::string>
    OpcodeMapper::g_opcodeToString = {
        // 常量
        {BytOpcode::OP_0, "OP_0"},
        {BytOpcode::OP_FALSE, "OP_FALSE"},
        {BytOpcode::OP_PUSHDATA1, "OP_PUSHDATA1"},
        {BytOpcode::OP_PUSHDATA2, "OP_PUSHDATA2"},
        {BytOpcode::OP_PUSHDATA4, "OP_PUSHDATA4"},
        {BytOpcode::OP_1NEGATE, "OP_1NEGATE"},
        {BytOpcode::OP_1, "OP_1"},
        {BytOpcode::OP_TRUE, "OP_TRUE"},
        {BytOpcode::OP_2, "OP_2"},
        {BytOpcode::OP_3, "OP_3"},
        {BytOpcode::OP_4, "OP_4"},
        {BytOpcode::OP_5, "OP_5"},
        {BytOpcode::OP_6, "OP_6"},
        {BytOpcode::OP_7, "OP_7"},
        {BytOpcode::OP_8, "OP_8"},
        {BytOpcode::OP_9, "OP_9"},
        {BytOpcode::OP_10, "OP_10"},
        {BytOpcode::OP_11, "OP_11"},
        {BytOpcode::OP_12, "OP_12"},
        {BytOpcode::OP_13, "OP_13"},
        {BytOpcode::OP_14, "OP_14"},
        {BytOpcode::OP_15, "OP_15"},
        {BytOpcode::OP_16, "OP_16"},

        // 控制流
        {BytOpcode::OP_NOP, "OP_NOP"},
        {BytOpcode::OP_IF, "OP_IF"},
        {BytOpcode::OP_NOTIF, "OP_NOTIF"},
        {BytOpcode::OP_ELSE, "OP_ELSE"},
        {BytOpcode::OP_ENDIF, "OP_ENDIF"},
        {BytOpcode::OP_VERIFY, "OP_VERIFY"},
        {BytOpcode::OP_RETURN, "OP_RETURN"},

        // 栈操作
        {BytOpcode::OP_TOALTSTACK, "OP_TOALTSTACK"},
        {BytOpcode::OP_FROMALTSTACK, "OP_FROMALTSTACK"},
        {BytOpcode::OP_2DROP, "OP_2DROP"},
        {BytOpcode::OP_2DUP, "OP_2DUP"},
        {BytOpcode::OP_3DUP, "OP_3DUP"},
        {BytOpcode::OP_2OVER, "OP_2OVER"},
        {BytOpcode::OP_2ROT, "OP_2ROT"},
        {BytOpcode::OP_2SWAP, "OP_2SWAP"},
        {BytOpcode::OP_IFDUP, "OP_IFDUP"},
        {BytOpcode::OP_DEPTH, "OP_DEPTH"},
        {BytOpcode::OP_DROP, "OP_DROP"},
        {BytOpcode::OP_DUP, "OP_DUP"},
        {BytOpcode::OP_NIP, "OP_NIP"},
        {BytOpcode::OP_OVER, "OP_OVER"},
        {BytOpcode::OP_PICK, "OP_PICK"},
        {BytOpcode::OP_ROLL, "OP_ROLL"},
        {BytOpcode::OP_ROT, "OP_ROT"},
        {BytOpcode::OP_SWAP, "OP_SWAP"},
        {BytOpcode::OP_TUCK, "OP_TUCK"},

        // 数据操作
        {BytOpcode::OP_CAT, "OP_CAT"},
        {BytOpcode::OP_SPLIT, "OP_SPLIT"},
        {BytOpcode::OP_NUM2BIN, "OP_NUM2BIN"},
        {BytOpcode::OP_BIN2NUM, "OP_BIN2NUM"},
        {BytOpcode::OP_SIZE, "OP_SIZE"},

        // 位运算
        {BytOpcode::OP_INVERT, "OP_INVERT"},
        {BytOpcode::OP_AND, "OP_AND"},
        {BytOpcode::OP_OR, "OP_OR"},
        {BytOpcode::OP_XOR, "OP_XOR"},

        // 比较操作符
        {BytOpcode::OP_EQUAL, "OP_EQUAL"},
        {BytOpcode::OP_EQUALVERIFY, "OP_EQUALVERIFY"},

        // 算术运算
        {BytOpcode::OP_1ADD, "OP_1ADD"},
        {BytOpcode::OP_1SUB, "OP_1SUB"},
        {BytOpcode::OP_NEGATE, "OP_NEGATE"},
        {BytOpcode::OP_ABS, "OP_ABS"},
        {BytOpcode::OP_NOT, "OP_NOT"},
        {BytOpcode::OP_0NOTEQUAL, "OP_0NOTEQUAL"},
        {BytOpcode::OP_ADD, "OP_ADD"},
        {BytOpcode::OP_SUB, "OP_SUB"},
        {BytOpcode::OP_MUL, "OP_MUL"},
        {BytOpcode::OP_DIV, "OP_DIV"},
        {BytOpcode::OP_MOD, "OP_MOD"},
        {BytOpcode::OP_LSHIFT, "OP_LSHIFT"},
        {BytOpcode::OP_RSHIFT, "OP_RSHIFT"},

        // 逻辑
        {BytOpcode::OP_BOOLAND, "OP_BOOLAND"},
        {BytOpcode::OP_BOOLOR, "OP_BOOLOR"},

        // 数值比较
        {BytOpcode::OP_NUMEQUAL, "OP_NUMEQUAL"},
        {BytOpcode::OP_NUMEQUALVERIFY, "OP_NUMEQUALVERIFY"},
        {BytOpcode::OP_NUMNOTEQUAL, "OP_NUMNOTEQUAL"},
        {BytOpcode::OP_LESSTHAN, "OP_LESSTHAN"},
        {BytOpcode::OP_GREATERTHAN, "OP_GREATERTHAN"},
        {BytOpcode::OP_LESSTHANOREQUAL, "OP_LESSTHANOREQUAL"},
        {BytOpcode::OP_GREATERTHANOREQUAL, "OP_GREATERTHANOREQUAL"},
        {BytOpcode::OP_MIN, "OP_MIN"},
        {BytOpcode::OP_MAX, "OP_MAX"},
        {BytOpcode::OP_WITHIN, "OP_WITHIN"},

        // 加密
        {BytOpcode::OP_RIPEMD160, "OP_RIPEMD160"},
        {BytOpcode::OP_SHA1, "OP_SHA1"},
        {BytOpcode::OP_SHA256, "OP_SHA256"},
        {BytOpcode::OP_HASH160, "OP_HASH160"},
        {BytOpcode::OP_HASH256, "OP_HASH256"},
        {BytOpcode::OP_CODESEPARATOR, "OP_CODESEPARATOR"},
        {BytOpcode::OP_CHECKSIG, "OP_CHECKSIG"},
        {BytOpcode::OP_CHECKSIGVERIFY, "OP_CHECKSIGVERIFY"},
        {BytOpcode::OP_CHECKMULTISIG, "OP_CHECKMULTISIG"},
        {BytOpcode::OP_CHECKMULTISIGVERIFY, "OP_CHECKMULTISIGVERIFY"},

        // 时间锁
        {BytOpcode::OP_CHECKLOCKTIMEVERIFY, "OP_CHECKLOCKTIMEVERIFY"},
        {BytOpcode::OP_CHECKSEQUENCEVERIFY, "OP_CHECKSEQUENCEVERIFY"},

        // 扩展操作码
        {BytOpcode::OP_PUSH_META, "OP_PUSH_META"},
        {BytOpcode::OP_PARTIAL_HASH, "OP_PARTIAL_HASH"},

        {BytOpcode::OP_INVALIDOPCODE, "OP_INVALIDOPCODE"},
};

const std::unordered_map<std::string, BytOpcode>
    OpcodeMapper::g_stringToOpcode = {
        // 常量
        {"OP_0", BytOpcode::OP_0},
        {"OP_FALSE", BytOpcode::OP_FALSE},
        {"OP_PUSHDATA1", BytOpcode::OP_PUSHDATA1},
        {"OP_PUSHDATA2", BytOpcode::OP_PUSHDATA2},
        {"OP_PUSHDATA4", BytOpcode::OP_PUSHDATA4},
        {"OP_1NEGATE", BytOpcode::OP_1NEGATE},
        {"OP_1", BytOpcode::OP_1},
        {"OP_TRUE", BytOpcode::OP_TRUE},
        {"OP_2", BytOpcode::OP_2},
        {"OP_3", BytOpcode::OP_3},
        {"OP_4", BytOpcode::OP_4},
        {"OP_5", BytOpcode::OP_5},
        {"OP_6", BytOpcode::OP_6},
        {"OP_7", BytOpcode::OP_7},
        {"OP_8", BytOpcode::OP_8},
        {"OP_9", BytOpcode::OP_9},
        {"OP_10", BytOpcode::OP_10},
        {"OP_11", BytOpcode::OP_11},
        {"OP_12", BytOpcode::OP_12},
        {"OP_13", BytOpcode::OP_13},
        {"OP_14", BytOpcode::OP_14},
        {"OP_15", BytOpcode::OP_15},
        {"OP_16", BytOpcode::OP_16},

        // 控制流
        {"OP_NOP", BytOpcode::OP_NOP},
        {"OP_IF", BytOpcode::OP_IF},
        {"OP_NOTIF", BytOpcode::OP_NOTIF},
        {"OP_ELSE", BytOpcode::OP_ELSE},
        {"OP_ENDIF", BytOpcode::OP_ENDIF},
        {"OP_VERIFY", BytOpcode::OP_VERIFY},
        {"OP_RETURN", BytOpcode::OP_RETURN},

        // 栈操作
        {"OP_TOALTSTACK", BytOpcode::OP_TOALTSTACK},
        {"OP_FROMALTSTACK", BytOpcode::OP_FROMALTSTACK},
        {"OP_2DROP", BytOpcode::OP_2DROP},
        {"OP_2DUP", BytOpcode::OP_2DUP},
        {"OP_3DUP", BytOpcode::OP_3DUP},
        {"OP_2OVER", BytOpcode::OP_2OVER},
        {"OP_2ROT", BytOpcode::OP_2ROT},
        {"OP_2SWAP", BytOpcode::OP_2SWAP},
        {"OP_IFDUP", BytOpcode::OP_IFDUP},
        {"OP_DEPTH", BytOpcode::OP_DEPTH},
        {"OP_DROP", BytOpcode::OP_DROP},
        {"OP_DUP", BytOpcode::OP_DUP},
        {"OP_NIP", BytOpcode::OP_NIP},
        {"OP_OVER", BytOpcode::OP_OVER},
        {"OP_PICK", BytOpcode::OP_PICK},
        {"OP_ROLL", BytOpcode::OP_ROLL},
        {"OP_ROT", BytOpcode::OP_ROT},
        {"OP_SWAP", BytOpcode::OP_SWAP},
        {"OP_TUCK", BytOpcode::OP_TUCK},

        // 数据操作
        {"OP_CAT", BytOpcode::OP_CAT},
        {"OP_SPLIT", BytOpcode::OP_SPLIT},
        {"OP_NUM2BIN", BytOpcode::OP_NUM2BIN},
        {"OP_BIN2NUM", BytOpcode::OP_BIN2NUM},
        {"OP_SIZE", BytOpcode::OP_SIZE},

        // 位运算
        {"OP_INVERT", BytOpcode::OP_INVERT},
        {"OP_AND", BytOpcode::OP_AND},
        {"OP_OR", BytOpcode::OP_OR},
        {"OP_XOR", BytOpcode::OP_XOR},

        // 比较操作符
        {"OP_EQUAL", BytOpcode::OP_EQUAL},
        {"OP_EQUALVERIFY", BytOpcode::OP_EQUALVERIFY},

        // 算术运算
        {"OP_1ADD", BytOpcode::OP_1ADD},
        {"OP_1SUB", BytOpcode::OP_1SUB},
        {"OP_NEGATE", BytOpcode::OP_NEGATE},
        {"OP_ABS", BytOpcode::OP_ABS},
        {"OP_NOT", BytOpcode::OP_NOT},
        {"OP_0NOTEQUAL", BytOpcode::OP_0NOTEQUAL},
        {"OP_ADD", BytOpcode::OP_ADD},
        {"OP_SUB", BytOpcode::OP_SUB},
        {"OP_MUL", BytOpcode::OP_MUL},
        {"OP_DIV", BytOpcode::OP_DIV},
        {"OP_MOD", BytOpcode::OP_MOD},
        {"OP_LSHIFT", BytOpcode::OP_LSHIFT},
        {"OP_RSHIFT", BytOpcode::OP_RSHIFT},

        // 逻辑
        {"OP_BOOLAND", BytOpcode::OP_BOOLAND},
        {"OP_BOOLOR", BytOpcode::OP_BOOLOR},

        // 数值比较
        {"OP_NUMEQUAL", BytOpcode::OP_NUMEQUAL},
        {"OP_NUMEQUALVERIFY", BytOpcode::OP_NUMEQUALVERIFY},
        {"OP_NUMNOTEQUAL", BytOpcode::OP_NUMNOTEQUAL},
        {"OP_LESSTHAN", BytOpcode::OP_LESSTHAN},
        {"OP_GREATERTHAN", BytOpcode::OP_GREATERTHAN},
        {"OP_LESSTHANOREQUAL", BytOpcode::OP_LESSTHANOREQUAL},
        {"OP_GREATERTHANOREQUAL", BytOpcode::OP_GREATERTHANOREQUAL},
        {"OP_MIN", BytOpcode::OP_MIN},
        {"OP_MAX", BytOpcode::OP_MAX},
        {"OP_WITHIN", BytOpcode::OP_WITHIN},

        // 加密
        {"OP_RIPEMD160", BytOpcode::OP_RIPEMD160},
        {"OP_SHA1", BytOpcode::OP_SHA1},
        {"OP_SHA256", BytOpcode::OP_SHA256},
        {"OP_HASH160", BytOpcode::OP_HASH160},
        {"OP_HASH256", BytOpcode::OP_HASH256},
        {"OP_CODESEPARATOR", BytOpcode::OP_CODESEPARATOR},
        {"OP_CHECKSIG", BytOpcode::OP_CHECKSIG},
        {"OP_CHECKSIGVERIFY", BytOpcode::OP_CHECKSIGVERIFY},
        {"OP_CHECKMULTISIG", BytOpcode::OP_CHECKMULTISIG},
        {"OP_CHECKMULTISIGVERIFY", BytOpcode::OP_CHECKMULTISIGVERIFY},

        // 时间锁
        {"OP_CHECKLOCKTIMEVERIFY", BytOpcode::OP_CHECKLOCKTIMEVERIFY},
        {"OP_CHECKSEQUENCEVERIFY", BytOpcode::OP_CHECKSEQUENCEVERIFY},

        // 扩展操作码
        {"OP_PUSH_META", BytOpcode::OP_PUSH_META},
        {"OP_PARTIAL_HASH", BytOpcode::OP_PARTIAL_HASH},

        {"OP_INVALIDOPCODE", BytOpcode::OP_INVALIDOPCODE},
};

SPACE_TBC_END
