#ifndef BYTECODE_INSTRUCTION_UTILS_H
#define BYTECODE_INSTRUCTION_UTILS_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bytecode_opcodes.h"

namespace tbc::bytecode_instruction
{

inline bool isHexChar(char c)
{
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

inline bool isPureHexStrictEven(const std::string& value)
{
    if (value.empty() || (value.size() % 2) != 0) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), isHexChar);
}

inline std::string normalizeHex(std::string value)
{
    if (value.size() >= 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value = value.substr(2);
    }
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

inline bool isPureHexNormalizedEven(const std::string& value)
{
    const std::string hex = normalizeHex(value);
    if (hex.empty() || (hex.size() % 2) != 0) {
        return false;
    }
    return std::all_of(hex.begin(), hex.end(), isHexChar);
}

inline bool isIgnoredLine(const std::string& value)
{
    return value.empty() || value[0] == '#';
}

inline bool isSingleOpcodeNormalized(
    const std::string& value,
    tbc::BytOpcode opcode
)
{
    return isPureHexNormalizedEven(value) &&
           normalizeHex(value) == tbc::opcodeToHex(opcode);
}

inline std::optional<size_t> instructionBytesNormalized(
    const std::string& value
)
{
    if (isIgnoredLine(value)) {
        return 0;
    }
    if (!isPureHexNormalizedEven(value)) {
        return std::nullopt;
    }
    return normalizeHex(value).size() / 2;
}

inline std::optional<size_t> countBytesNormalized(
    const std::vector<std::string>& instrs,
    size_t start,
    size_t end
)
{
    size_t total = 0;
    for (size_t i = start; i < end; ++i) {
        const auto bytesOpt = instructionBytesNormalized(instrs[i]);
        if (!bytesOpt.has_value()) {
            return std::nullopt;
        }
        total += bytesOpt.value();
    }
    return total;
}

inline size_t countBestEffortBytesNormalized(
    const std::vector<std::string>& instrs,
    size_t start,
    size_t end
)
{
    size_t total = 0;
    for (size_t i = start; i < end; ++i) {
        const auto bytesOpt = instructionBytesNormalized(instrs[i]);
        if (bytesOpt.has_value()) {
            total += bytesOpt.value();
        }
    }
    return total;
}

inline size_t countStrictHexBytes(const std::vector<std::string>& instrs)
{
    size_t total = 0;
    for (const auto& instr : instrs) {
        if (!isIgnoredLine(instr) && isPureHexStrictEven(instr)) {
            total += instr.size() / 2;
        }
    }
    return total;
}

inline size_t bytesToAlign(size_t length, size_t alignment)
{
    const size_t remainder = length % alignment;
    return remainder == 0 ? 0 : alignment - remainder;
}

inline uint8_t readHexByte(const std::string& hex, size_t byteIndex)
{
    return static_cast<uint8_t>(
        std::stoul(hex.substr(byteIndex * 2, 2), nullptr, 16)
    );
}

inline std::vector<std::string> splitHexScriptIntoInstructions(
    const std::string& hex
)
{
    std::vector<std::string> instructions;
    if (!isPureHexStrictEven(hex)) {
        return instructions;
    }

    const size_t totalBytes = hex.size() / 2;
    size_t i = 0;
    while (i < totalBytes) {
        const uint8_t op = readHexByte(hex, i);

        if (op >= 0x01 && op <= 0x4b) {
            const size_t dataLen = static_cast<size_t>(op);
            const size_t instrBytes = 1 + dataLen;
            if (i + instrBytes <= totalBytes) {
                instructions.push_back(hex.substr(i * 2, instrBytes * 2));
                i += instrBytes;
                continue;
            }
        }

        if (op == 0x4c || op == 0x4d || op == 0x4e) {
            const size_t lenBytes = (op == 0x4c) ? 1 : (op == 0x4d ? 2 : 4);
            if (i + 1 + lenBytes <= totalBytes) {
                uint32_t dataLen = 0;
                if (lenBytes == 1) {
                    dataLen = readHexByte(hex, i + 1);
                } else if (lenBytes == 2) {
                    dataLen = static_cast<uint32_t>(readHexByte(hex, i + 1)) |
                              (static_cast<uint32_t>(readHexByte(hex, i + 2))
                               << 8);
                } else {
                    dataLen = static_cast<uint32_t>(readHexByte(hex, i + 1)) |
                              (static_cast<uint32_t>(readHexByte(hex, i + 2))
                               << 8) |
                              (static_cast<uint32_t>(readHexByte(hex, i + 3))
                               << 16) |
                              (static_cast<uint32_t>(readHexByte(hex, i + 4))
                               << 24);
                }

                const size_t instrBytes =
                    1 + lenBytes + static_cast<size_t>(dataLen);
                if (i + instrBytes <= totalBytes) {
                    instructions.push_back(hex.substr(i * 2, instrBytes * 2));
                    i += instrBytes;
                    continue;
                }
            }
        }

        instructions.push_back(hex.substr(i * 2, 2));
        ++i;
    }

    return instructions;
}

} // namespace tbc::bytecode_instruction

#endif // BYTECODE_INSTRUCTION_UTILS_H
