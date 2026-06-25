#ifndef BYTECODE_HELPER_FUN_H
#define BYTECODE_HELPER_FUN_H

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

#include "../log/logger.h"
#include "../util/byt_fun.h"
#include "../util/op_stack.h"
#include "bytecode_opcodes.h"

SPACE_TBC_START

inline std::string encodePushData(const valtype& data)
{
    std::string result;

    auto byteToHex = [](uint8_t byte) -> std::string {
        static const char hex_digits[] = "0123456789abcdef";
        return std::string{hex_digits[byte >> 4], hex_digits[byte & 0x0f]};
    };

    auto appendData = [&result, &byteToHex](const auto& data) {
        for (uint8_t byte : data) {
            result += byteToHex(byte);
        }
    };

    if (data.size() <= 75) {
        result += opcodeToHex(static_cast<BytOpcode>(data.size()));
        appendData(data);
    } else if (data.size() <= 255) {
        result += opcodeToHex(BytOpcode::OP_PUSHDATA1);
        result += byteToHex(static_cast<uint8_t>(data.size()));
        appendData(data);
    } else if (data.size() <= 65535) {
        result += opcodeToHex(BytOpcode::OP_PUSHDATA2);
        uint16_t size = static_cast<uint16_t>(data.size());
        result += byteToHex(static_cast<uint8_t>(size & 0xff));
        result += byteToHex(static_cast<uint8_t>((size >> 8) & 0xff));
        appendData(data);
    } else if (data.size() <= 4294967295UL) {
        result += opcodeToHex(BytOpcode::OP_PUSHDATA4);
        uint32_t size = static_cast<uint32_t>(data.size());
        for (int i = 0; i < 4; ++i) {
            result += byteToHex(static_cast<uint8_t>((size >> (8 * i)) & 0xff));
        }
        appendData(data);
    } else {
        result += opcodeToHex(BytOpcode::OP_INVALIDOPCODE);
    }

    return result;
}

// 数值 -> Bitcoin Script hex
inline std::string numberToScriptHex(int64_t value)
{
    std::string result;

    if (value == 0) {
        result += opcodeToHex(BytOpcode::OP_0);
    } else if (value == -1) {
        result += opcodeToHex(BytOpcode::OP_1NEGATE);
    } else if (value >= 1 && value <= 16) {
        uint8_t opcode = 0x50 + static_cast<uint8_t>(value);
        result += opcodeToHex(static_cast<BytOpcode>(opcode));
    } else {
        // 其余 (含负数): 标准编码; StackElement 用 CScriptNum 处理负数
        StackElement element(value);
        const auto& data = element.valueData();
        result += encodePushData(data);
    }

    return "0x" + result;
}

/**
 * 生成"将位置 pos 的元素移至栈顶"的最短字节码序列：
 *   pos==0 → 无操作（已在栈顶）
 *   pos==1 → OP_SWAP   (1 byte，替代 OP_1 OP_ROLL)
 *   pos==2 → OP_ROT    (1 byte，替代 OP_2 OP_ROLL)
 *   pos>=3 → pos OP_ROLL (2+ bytes)
 */
inline std::string rollToTopHex(int64_t pos)
{
    if (pos == 0) {
        return "";
    }
    if (pos == 1) {
        return opcodeToHex(BytOpcode::OP_SWAP);
    }
    if (pos == 2) {
        return opcodeToHex(BytOpcode::OP_ROT);
    }
    return numberToScriptHex(pos) + opcodeToHex(BytOpcode::OP_ROLL);
}

/**
 * 生成"移除位置 pos 的元素（不保留）"的最短字节码序列：
 *   pos==0 → OP_DROP         (1 byte)
 *   pos==1 → OP_NIP          (1 byte，替代 OP_1 OP_ROLL OP_DROP)
 *   pos==2 → OP_ROT OP_DROP  (2 bytes，替代 OP_2 OP_ROLL OP_DROP)
 *   pos>=3 → pos OP_ROLL OP_DROP (3+ bytes)
 */
inline std::string rollDropHex(int64_t pos)
{
    if (pos == 0) {
        return opcodeToHex(BytOpcode::OP_DROP);
    }
    if (pos == 1) {
        return opcodeToHex(BytOpcode::OP_NIP);
    }
    if (pos == 2) {
        return opcodeToHex(BytOpcode::OP_ROT) + opcodeToHex(BytOpcode::OP_DROP);
    }
    return numberToScriptHex(pos) + opcodeToHex(BytOpcode::OP_ROLL) +
           opcodeToHex(BytOpcode::OP_DROP);
}

// 字符串 -> Bitcoin Script hex
inline std::string stringToScriptHex(const std::string& str)
{
    try {
        if (str.empty()) {
            return "0x00"; // OP_0
        }

        std::string processedStr = str;
        escapeString(processedStr);

        std::ostringstream oss;
        for (unsigned char c : processedStr) {
            oss << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<unsigned int>(c);
        }
        std::string stringHex = oss.str();

        if (stringHex.empty()) {
            return "0x00";
        }

        int dataLength = stringHex.length() / 2;

        if (dataLength <= 0) {
            return "0x00";
        }

        std::string lengthOpcode = bytEncodeLengthOpcode(dataLength);
        if (lengthOpcode.empty()) {
            return "0x00";
        }

        return "0x" + lengthOpcode + stringHex;

    } catch (const std::exception& e) {
        return "0x00";
    } catch (...) {
        return "0x00";
    }
}

// 去掉 0x 前缀并校验有效性, 失败返回空串
inline std::string hexData(const std::string& hex)
{
    if (hex.empty()) {
        return "";
    }

    std::string hexData = hex;

    // 0x 前缀大小写不敏感
    if (hexData.length() >= 2 &&
        (hexData.substr(0, 2) == "0x" || hexData.substr(0, 2) == "0X")) {
        hexData = hexData.substr(2);
    }

    if (!hexData.empty() &&
        !std::all_of(hexData.begin(), hexData.end(), [](char c) {
            return std::isxdigit(c);
        })) {
        return "";
    }

    return hexData;
}

// Bitcoin Script hex -> 数值 (含负数, 与 CScriptNum 一致)
inline int64_t scriptHexToNumber(const std::string& hex)
{
    if (hex.empty()) {
        return 0;
    }

    std::string cleanHex = hexData(hex);
    if (cleanHex.empty()) {
        return 0;
    }

    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < cleanHex.length(); i += 2) {
        std::string byteStr = cleanHex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
        bytes.push_back(byte);
    }

    if (bytes.empty()) {
        return 0;
    }

    // numberToScriptHex 的特殊操作码
    if (bytes.size() == 1 && bytes[0] == 0x00) {
        return 0; // OP_0
    }
    if (bytes.size() == 1 && bytes[0] == 0x4f) {
        return -1; // OP_1NEGATE
    }
    if (bytes.size() == 1 && bytes[0] >= 0x51 && bytes[0] <= 0x60) {
        return static_cast<int64_t>(bytes[0] - 0x50); // OP_1 ~ OP_16
    }

    // 按 encodePushData 的编码取真实数据段
    size_t dataStart = 0;
    size_t dataLen = 0;

    uint8_t first = bytes[0];

    if (first >= 1 && first <= 75) {
        dataLen = first;
        dataStart = 1;
        if (bytes.size() < dataStart + dataLen) {
            return 0;
        }
    } else if (first == 0x4c) { // OP_PUSHDATA1
        if (bytes.size() < 2) {
            return 0;
        }
        dataLen = bytes[1];
        dataStart = 2;
        if (bytes.size() < dataStart + dataLen) {
            return 0;
        }
    } else if (first == 0x4d) { // OP_PUSHDATA2
        if (bytes.size() < 3) {
            return 0;
        }
        dataLen = static_cast<size_t>(bytes[1]) |
                  (static_cast<size_t>(bytes[2]) << 8);
        dataStart = 3;
        if (bytes.size() < dataStart + dataLen) {
            return 0;
        }
    } else if (first == 0x4e) { // OP_PUSHDATA4
        if (bytes.size() < 5) {
            return 0;
        }
        dataLen = static_cast<size_t>(bytes[1]) |
                  (static_cast<size_t>(bytes[2]) << 8) |
                  (static_cast<size_t>(bytes[3]) << 16) |
                  (static_cast<size_t>(bytes[4]) << 24);
        dataStart = 5;
        if (bytes.size() < dataStart + dataLen) {
            return 0;
        }
    } else {
        // 非 pushdata 编码: 整段字节当作数值 (兼容旧逻辑)
        dataStart = 0;
        dataLen = bytes.size();
    }

    if (dataLen == 0) {
        return 0;
    }

    std::vector<uint8_t> numBytes(
        bytes.begin() + static_cast<long>(dataStart),
        bytes.begin() + static_cast<long>(dataStart + dataLen)
    );

    int64_t value = 0;
    for (size_t i = 0; i < numBytes.size(); ++i) {
        value |= static_cast<int64_t>(numBytes[i]) << (8 * i);
    }

    // 最高位为符号位
    if (!numBytes.empty() && (numBytes.back() & 0x80)) {
        value &= ~(0x80ULL << (8 * (numBytes.size() - 1)));
        value = -value;
    }

    return value;
}

// hex 串 -> Bitcoin Script hex
inline std::string hexToScriptHex(const std::string& hexStr)
{
    if (hexStr.empty()) {
        return "0x00";
    }

    std::string hexDataStr = hexData(hexStr);
    if (hexDataStr.empty()) {
        return "0x00";
    }

    std::string lengthOpcode = bytEncodeLengthOpcode(hexDataStr.length() / 2);
    return "0x" + lengthOpcode + hexDataStr;
}

// Base58 解码
inline std::vector<uint8_t> base58Decode(const std::string& encoded)
{
    static const std::string base58_chars =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

    // 前导 '1' 数 = 前导零字节数
    int zeroCount = 0;
    for (char c : encoded) {
        if (c == '1') {
            zeroCount++;
        } else {
            break;
        }
    }

    // base58 串 -> 大整数 (字节数组, 小端)
    std::vector<uint8_t> num;
    for (size_t i = zeroCount; i < encoded.length(); i++) {
        size_t pos = base58_chars.find(encoded[i]);
        if (pos == std::string::npos) {
            return {};
        }

        // num = num * 58 + digit
        int carry = static_cast<int>(pos);
        for (size_t j = 0; j < num.size(); j++) {
            carry += static_cast<int>(num[j]) * 58;
            num[j] = static_cast<uint8_t>(carry % 256);
            carry /= 256;
        }
        while (carry > 0) {
            num.push_back(static_cast<uint8_t>(carry % 256));
            carry /= 256;
        }
    }

    // 转回大端
    std::reverse(num.begin(), num.end());

    num.insert(num.begin(), zeroCount, 0);

    return num;
}

// 将 P2PKH 地址解码为 20 字节公钥哈希
inline std::string decodeP2PKHAddress(const std::string& address)
{
    std::vector<uint8_t> decoded = base58Decode(address);
    if (decoded.size() != 25) {
        LOG_ERROR("Invalid address length after Base58 decode");
        return "";
    }

    // 主网 P2PKH 版本字节 0x00
    if (decoded[0] != 0x00) {
        LOG_ERROR("Invalid version byte for P2PKH address");
        return "";
    }

    std::vector<uint8_t> pubkeyHash(decoded.begin() + 1, decoded.begin() + 21);

    // 跳过校验和: 需要 SHA256; 生产环境应验证完整性

    std::ostringstream oss;
    for (uint8_t byte : pubkeyHash) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<unsigned int>(byte);
    }
    return oss.str();
}

SPACE_TBC_END
#endif
