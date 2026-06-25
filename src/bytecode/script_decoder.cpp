#include "script_decoder.h"

#include <cctype>
#include <iomanip>
#include <regex>
#include <sstream>

#include "../log/logger.h"

using namespace tbc;

std::string script_decoder::hex_to_asm(const std::string& hex_str)
{
    std::string processed_hex = hex_str;
    std::vector<std::pair<size_t, std::string>> placeholders;

    // 收集 <pub>/<sig> 等占位符
    std::regex placeholder_pattern(R"(<[^>]+>)");
    std::sregex_iterator iter(
        hex_str.begin(), hex_str.end(), placeholder_pattern
    );
    std::sregex_iterator end;

    for (auto it = iter; it != end; ++it) {
        placeholders.push_back({it->position(), it->str()});
    }

    // 从后往前删除, 避免位置偏移
    for (auto it = placeholders.rbegin(); it != placeholders.rend(); ++it) {
        processed_hex.erase(it->first, it->second.length());
    }

    if (placeholders.empty()) {
        auto bytes = hex_to_bytes(processed_hex);
        return bytes_to_asm(bytes);
    }

    return process_hex_with_placeholders(hex_str, placeholders, processed_hex);
}

std::string script_decoder::bytes_to_asm(
    const std::vector<uint8_t>& script_bytes
)
{
    std::ostringstream asm_output;
    bool first = true;

    size_t pos = 0;
    while (pos < script_bytes.size()) {
        if (!first) {
            asm_output << " ";
        }
        first = false;

        const uint8_t* remaining_data = script_bytes.data() + pos;
        size_t remaining_size = script_bytes.size() - pos;
        decode_result result =
            decode_instruction(remaining_data, remaining_size);

        if (!result.valid) {
            asm_output << "[error]";
            break;
        }

        if (result.instr.opcode <= BytOpcode::OP_PUSHDATA4 &&
            !result.instr.operand.empty()) {
            size_t data_length = result.instr.operand.size();

            // OP_PUSHDATA1/2/4: 先输出操作码名再输出长度 (与 hex 中一致)
            if (result.instr.opcode >= BytOpcode::OP_PUSHDATA1) {
                asm_output << opcode_to_name(result.instr.opcode);
                if (result.instr.opcode == BytOpcode::OP_PUSHDATA1) {
                    asm_output << std::hex << std::setfill('0') << std::setw(2)
                               << static_cast<int>(data_length);
                } else if (result.instr.opcode == BytOpcode::OP_PUSHDATA2) {
                    // 2 字节小端
                    uint16_t length_le = static_cast<uint16_t>(data_length);
                    asm_output << std::hex << std::setfill('0') << std::setw(2)
                               << static_cast<int>(length_le & 0xFF);
                    asm_output << std::hex << std::setfill('0') << std::setw(2)
                               << static_cast<int>((length_le >> 8) & 0xFF);
                } else if (result.instr.opcode == BytOpcode::OP_PUSHDATA4) {
                    // 4 字节小端
                    uint32_t length_le = static_cast<uint32_t>(data_length);
                    asm_output << std::hex << std::setfill('0') << std::setw(2)
                               << static_cast<int>(length_le & 0xFF);
                    asm_output << std::hex << std::setfill('0') << std::setw(2)
                               << static_cast<int>((length_le >> 8) & 0xFF);
                    asm_output << std::hex << std::setfill('0') << std::setw(2)
                               << static_cast<int>((length_le >> 16) & 0xFF);
                    asm_output << std::hex << std::setfill('0') << std::setw(2)
                               << static_cast<int>((length_le >> 24) & 0xFF);
                }
                asm_output << std::dec;
            } else {
                // 直接 PUSH (1-75): 操作码值即长度
                asm_output << std::hex << std::setfill('0') << std::setw(2)
                           << static_cast<int>(data_length);
                asm_output << std::dec;
            }

            auto hexFunRes = data_to_hex(
                result.instr.operand.data(), result.instr.operand.size()
            );
            asm_output << hexFunRes;
        } else {
            asm_output << opcode_to_name(result.instr.opcode);
        }

        pos += result.bytes_consumed;
    }

    return asm_output.str();
}

decode_result
script_decoder::decode_instruction(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return decode_result();
    }

    BytOpcode opcode = static_cast<BytOpcode>(data[0]);
    std::vector<uint8_t> operand;
    size_t bytes_consumed = 1;

    // PUSH 数据指令
    if (opcode <= BytOpcode::OP_PUSHDATA4) {
        size_t data_length = 0;
        size_t length_bytes = 0;

        if (opcode < BytOpcode::OP_PUSHDATA1) {
            // 直接 PUSH (1-75 字节)
            data_length = static_cast<size_t>(opcode);
            length_bytes = 0;
        } else if (opcode == BytOpcode::OP_PUSHDATA1) {
            if (size < 2)
                return decode_result();
            data_length = data[1];
            length_bytes = 1;
        } else if (opcode == BytOpcode::OP_PUSHDATA2) {
            if (size < 3)
                return decode_result();
            data_length = read_le16(&data[1]);
            length_bytes = 2;
        } else if (opcode == BytOpcode::OP_PUSHDATA4) {
            if (size < 5)
                return decode_result();
            data_length = read_le32(&data[1]);
            length_bytes = 4;
        }

        if (size < 1 + length_bytes + data_length) {
            return decode_result();
        }

        if (data_length > 0) {
            size_t data_start = 1 + length_bytes;
            operand.assign(data + data_start, data + data_start + data_length);
        }

        bytes_consumed = 1 + length_bytes + data_length;
    }

    return decode_result(true, instruction(opcode, operand), bytes_consumed);
}

std::string script_decoder::opcode_to_name(BytOpcode opcode)
{
    auto it = OpcodeMapper::g_opcodeToString.find(opcode);
    if (it != OpcodeMapper::g_opcodeToString.end()) {
        return it->second;
    }

    // 未知操作码: 输出十六进制表示
    std::ostringstream oss;
    oss << "OP_UNKNOWN(0x" << std::hex << static_cast<int>(opcode) << ")";
    return oss.str();
}

std::string script_decoder::data_to_hex(const uint8_t* data, size_t size)
{
    std::ostringstream oss;
    for (size_t i = 0; i < size; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::vector<uint8_t> script_decoder::hex_to_bytes(const std::string& hex)
{
    std::vector<uint8_t> bytes;

    // 仅保留十六进制字符
    std::string clean_hex;
    for (char c : hex) {
        if (std::isxdigit(c)) {
            clean_hex += std::tolower(c);
        }
    }

    // 长度需为偶数
    if (clean_hex.length() % 2 != 0) {
        LOG_ERROR("The length is not an even number, data may be missing!");
        clean_hex = "0" + clean_hex;
    }

    for (size_t i = 0; i < clean_hex.length(); i += 2) {
        std::string byte_str = clean_hex.substr(i, 2);
        uint8_t byte_val = static_cast<uint8_t>(
            std::stoul(byte_str, nullptr, 16)
        );
        bytes.push_back(byte_val);
    }

    return bytes;
}

uint16_t script_decoder::read_le16(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t script_decoder::read_le32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

std::string script_decoder::process_hex_with_placeholders(
    const std::string& original_hex,
    const std::vector<std::pair<size_t, std::string>>& placeholders,
    const std::string& /*processed_hex*/
)
{
    std::ostringstream result;
    size_t current_hex_pos = 0;
    [[maybe_unused]] size_t processed_pos = 0;
    bool first = true;

    for (const auto& placeholder : placeholders) {
        // 占位符前的 hex 段
        if (placeholder.first > current_hex_pos) {
            size_t hex_segment_len = placeholder.first - current_hex_pos;
            std::string hex_segment =
                original_hex.substr(current_hex_pos, hex_segment_len);

            if (!hex_segment.empty()) {
                auto bytes = script_decoder::hex_to_bytes(hex_segment);
                if (!bytes.empty()) {
                    std::string asm_segment = script_decoder::bytes_to_asm(bytes
                    );
                    if (!first && !asm_segment.empty()) {
                        result << " ";
                    }
                    result << asm_segment;
                    first = false;
                }
            }
        }

        if (!first) {
            result << " ";
        }
        result << placeholder.second;
        first = false;

        current_hex_pos = placeholder.first + placeholder.second.length();
    }

    // 最后一个占位符之后的 hex 段
    if (current_hex_pos < original_hex.length()) {
        std::string remaining_hex = original_hex.substr(current_hex_pos);
        if (!remaining_hex.empty()) {
            auto bytes = script_decoder::hex_to_bytes(remaining_hex);
            if (!bytes.empty()) {
                std::string asm_segment = script_decoder::bytes_to_asm(bytes);
                if (!first && !asm_segment.empty()) {
                    result << " ";
                }
                result << asm_segment;
            }
        }
    }

    return result.str();
}
