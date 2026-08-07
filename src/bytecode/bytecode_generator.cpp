#include "bytecode_generator.h"

#include <sstream>
#include <utility>

#include "bytecode_helper_fun.h"
#include "legacy_bytecode_adapter.h"

using namespace tbc;

void BytecodeGenerator::emit(tbc::BytOpcode opcode)
{
    BytecodeInstruction instruction;
    instruction.id = m_nextInstructionId++;
    instruction.body = OpcodeInstruction{opcode};
    instruction.region = ScriptRegion::Executable;
    instruction.origins.push_back(instruction.id);
    instruction.legacyEncoding = opcodeToHex(opcode);
    appendInstruction(std::move(instruction));
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

    auto parsed = LegacyBytecodeAdapter::splitScriptFragment(
        processed_operand, m_nextInstructionId
    );
    if (!parsed.ok()) {
        // Keep LegacyV1 byte serialization stable, but make malformed or mixed
        // input an explicit optimizer barrier instead of guessing push
        // boundaries. CanonicalV2 materialization rejects this node.
        BytecodeInstruction barrier;
        barrier.id = m_nextInstructionId++;
        barrier.body = LegacyBarrierInstruction{processed_operand};
        barrier.region = ScriptRegion::Executable;
        barrier.origins.push_back(barrier.id);
        barrier.legacyEncoding = processed_operand;
        appendInstruction(std::move(barrier));
        return;
    }

    for (auto& instruction : parsed.instructions) {
        m_nextInstructionId = std::max(
            m_nextInstructionId, instruction.id + 1
        );
        appendInstruction(std::move(instruction));
    }
}

void BytecodeGenerator::appendInstruction(BytecodeInstruction instruction)
{
    m_subInstruct.push_back(std::move(instruction));

#ifdef ENABLE_DEBUGGER
    if (m_debugInfoCallback) {
        const size_t pc = m_instruct.size() + m_subInstruct.size() - 1;
        const std::string encoded = serialize(m_subInstruct.back());
        std::string opcodeStr = encoded;
        std::string operandStr;
        const size_t spacePos = encoded.find(' ');
        if (spacePos != std::string::npos) {
            opcodeStr = encoded.substr(0, spacePos);
            operandStr = encoded.substr(spacePos + 1);
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
}

std::string BytecodeGenerator::serialize(
    const BytecodeInstruction& instruction
) const
{
    return LegacyBytecodeAdapter::serializeInstructionPreserving(instruction);
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
    return LegacyBytecodeAdapter::exportPreserving(artifact());
}

BytecodeArtifact BytecodeGenerator::artifact() const
{
    BytecodeArtifact result;
    result.format = ArtifactFormat::LegacyV1;
    result.lockingScript = m_instruct;
    result.unlockingScripts = m_uninstruct;

    // Bytes after the final OP_RETURN are immutable state/suffix data.  Mark
    // this only when exposing the complete artifact, since an earlier Return
    // may be followed by another public function during generation.
    std::optional<size_t> finalReturn;
    for (size_t index = 0; index < result.lockingScript.size(); ++index) {
        const auto* opcode = std::get_if<OpcodeInstruction>(
            &result.lockingScript[index].body
        );
        if (opcode && opcode->opcode == BytOpcode::OP_RETURN) {
            finalReturn = index;
        }
    }
    if (finalReturn.has_value()) {
        for (size_t index = *finalReturn + 1;
             index < result.lockingScript.size(); ++index) {
            result.lockingScript[index].region =
                ScriptRegion::ImmutableSuffix;
        }
    }
    return result;
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
    m_nextInstructionId = 0;
    m_subUninstruct.first.clear();
    m_subUninstruct.second.clear();
    m_uninstruct.clear();
}

std::string BytecodeGenerator::subStr() const
{
    std::ostringstream oss;
    for (const auto& instr : m_subInstruct) {
        oss << serialize(instr) << " ";
    }
    return oss.str();
}

std::string BytecodeGenerator::str() const
{
    std::ostringstream oss;
    for (const auto& instr : m_instruct) {
        oss << serialize(instr) << " ";
    }
    return oss.str();
}
