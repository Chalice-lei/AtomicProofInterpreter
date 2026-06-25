#ifndef SCRIPT_DECODER_H
#define SCRIPT_DECODER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "byt_defs.h"
#include "bytecode_opcodes.h"

SPACE_TBC_START
struct instruction
{
    BytOpcode opcode;
    std::vector<uint8_t> operand;

    instruction() : opcode(BytOpcode::OP_INVALIDOPCODE)
    {}
    instruction(BytOpcode op, const std::vector<uint8_t>& data)
        : opcode(op), operand(data)
    {}
};

struct decode_result
{
    bool valid;
    instruction instr;
    size_t bytes_consumed;

    decode_result() : valid(false), bytes_consumed(0)
    {}
    decode_result(bool v, const instruction& i, size_t consumed)
        : valid(v), instr(i), bytes_consumed(consumed)
    {}
};

// Bitcoin Script 反汇编器
class script_decoder
{
public:
    static std::string hex_to_asm(const std::string& hex_str);
    static std::string bytes_to_asm(const std::vector<uint8_t>& script_bytes);

    // 解析单条指令
    static decode_result decode_instruction(const uint8_t* data, size_t size);

    static std::string opcode_to_name(BytOpcode opcode);
    static std::string data_to_hex(const uint8_t* data, size_t size);
    static std::vector<uint8_t> hex_to_bytes(const std::string& hex);

private:
    static uint16_t read_le16(const uint8_t* data);
    static uint32_t read_le32(const uint8_t* data);

    static std::string process_hex_with_placeholders(
        const std::string& original_hex,
        const std::vector<std::pair<size_t, std::string>>& placeholders,
        const std::string& processed_hex);
};
SPACE_TBC_END

#endif // SCRIPT_DECODER_H
