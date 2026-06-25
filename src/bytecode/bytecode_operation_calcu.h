#ifndef BYTECODE_OPERATION_CALC_H
#define BYTECODE_OPERATION_CALC_H

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "byt_defs.h"
#include "bytecode_opcodes.h"

SPACE_TBC_START

static const std::unordered_map<std::string, std::function<std::string()>>
    g_operatorMap = {
        {"+",
         []() -> std::string { return opcodeToHex(tbc::BytOpcode::OP_ADD); }},
        {"-",
         []() -> std::string { return opcodeToHex(tbc::BytOpcode::OP_SUB); }},
        {"*",
         []() -> std::string { return opcodeToHex(tbc::BytOpcode::OP_MUL); }},
        {"/",
         []() -> std::string { return opcodeToHex(tbc::BytOpcode::OP_DIV); }},
        {"==",
         []() -> std::string { return opcodeToHex(tbc::BytOpcode::OP_EQUAL); }},
        {"!=",
         []() -> std::string {
             auto result = opcodeToHex(tbc::BytOpcode::OP_EQUAL);
             return result + opcodeToHex(tbc::BytOpcode::OP_NOT);
         }},
        {"<",
         []() -> std::string {
             return opcodeToHex(tbc::BytOpcode::OP_LESSTHAN);
         }},
        {">",
         []() -> std::string {
             return opcodeToHex(tbc::BytOpcode::OP_GREATERTHAN);
         }},
        {"<=",
         []() -> std::string {
             return opcodeToHex(tbc::BytOpcode::OP_LESSTHANOREQUAL);
         }},
        {">=",
         []() -> std::string {
             return opcodeToHex(tbc::BytOpcode::OP_GREATERTHANOREQUAL);
         }},
        {"&&",
         []() -> std::string {
             return opcodeToHex(tbc::BytOpcode::OP_BOOLAND);
         }},
        {"||", []() -> std::string {
             return opcodeToHex(tbc::BytOpcode::OP_BOOLOR);
         }}};

SPACE_TBC_END
#endif // BYTECODE_OPERATION_CALC_H
