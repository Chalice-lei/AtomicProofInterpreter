#include "bytecode_generator.h"

#include <sstream>

#include "bytecode_helper_fun.h"
#include "bytecode_instruction_utils.h"

using namespace tbc;

void BytecodeGenerator::emit(tbc::BytOpcode opcode)
{
    emit(opcodeToHex(opcode));
}

void BytecodeGenerator::emit(tbc::BytOpcode opcode, const std::string& operand)
{
    std::string processed_operand = tbc::hexData(operand);
    // hexData 返回空 (无效十六进制) 时回退到原始 operand
    if (processed_operand.empty() && !operand.empty()) {
        processed_operand = operand;
    }

    emit(opcodeToHex(opcode) + processed_operand);
}

void BytecodeGenerator::emit(const std::string& opcode)
{
    std::string processed_operand = tbc::hexData(opcode);
    if (processed_operand.empty() && !opcode.empty()) {
        processed_operand = opcode;
    }

    auto emitOne = [&](const std::string& instr) {
        size_t pc = m_instruct.size() + m_subInstruct.size();
        m_subInstruct.push_back(instr);

#ifdef ENABLE_DEBUGGER
        if (m_debugInfoCallback) {
            std::string opcodeStr = instr;
            std::string operandStr = "";
            size_t spacePos = instr.find(' ');
            if (spacePos != std::string::npos) {
                opcodeStr = instr.substr(0, spacePos);
                operandStr = instr.substr(spacePos + 1);
            }

            m_debugInfoCallback(pc, opcodeStr, operandStr, m_currentLocation);

            if (m_currentLocation.isValid()) {
                LOG_DEBUG(
                    "DEBUG_MAP:",
                    pc,
                    ":",
                    m_currentLocation.line,
                    ":",
                    m_currentLocation.column,
                    ":",
                    opcodeStr
                );
            }
        }
#endif
    };

    // 非纯十六进制：整段保留（如 <self.xxx> 占位）
    if (!tbc::bytecode_instruction::isPureHexStrictEven(processed_operand)) {
        emitOne(processed_operand);
        return;
    }

    for (const auto& instr :
         tbc::bytecode_instruction::splitHexScriptIntoInstructions(
             processed_operand
         )) {
        emitOne(instr);
    }
}

void BytecodeGenerator::emitUnlock(const std::string& unlock)
{
    m_subUninstruct.second = m_subUninstruct.second + unlock;
}

void BytecodeGenerator::emitUnlockName(const std::string& unlockName)
{
    m_subUninstruct.first = unlockName;
}

std::
    pair<std::vector<std::string>, std::unordered_map<std::string, std::string>>
    BytecodeGenerator::instructions() const
{
    return std::pair<
        std::vector<std::string>,
        std::unordered_map<std::string, std::string>>(m_instruct, m_uninstruct);
}

void BytecodeGenerator::mergeSubOverall()
{
    m_instruct
        .insert(m_instruct.end(), m_subInstruct.begin(), m_subInstruct.end());
    m_subInstruct.clear();
}

void BytecodeGenerator::mergeSubUnoverall()
{
    m_uninstruct.insert(m_subUninstruct);
    m_subUninstruct.first.clear();
    m_subUninstruct.second.clear();
}

void BytecodeGenerator::clear()
{
    m_subInstruct.clear();
    m_instruct.clear();
    m_subUninstruct.first.clear();
    m_subUninstruct.second.clear();
    m_uninstruct.clear();
}

std::string BytecodeGenerator::subStr() const
{
    std::ostringstream oss;
    for (const auto& instr : m_subInstruct) {
        oss << instr << " ";
    }
    return oss.str();
}

std::string BytecodeGenerator::str() const
{
    std::ostringstream oss;
    for (const auto& instr : m_instruct) {
        oss << instr << " ";
    }
    return oss.str();
}
