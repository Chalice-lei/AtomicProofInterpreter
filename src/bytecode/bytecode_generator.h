#ifndef BYTECODE_GENERATOR_H
#define BYTECODE_GENERATOR_H

#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

#include "byt_defs.h"
#include "bytecode_ir.h"
#include "bytecode_opcodes.h"

#ifdef ENABLE_DEBUGGER
#include "../debugger/info/debug_info.h"
#endif

SPACE_TBC_START
class BytecodeGenerator
{
public:
    BytecodeGenerator() {};

    void emit(tbc::BytOpcode opcode);
    void emit(tbc::BytOpcode opcode, const std::string& operand);

    // 字符串形式: 兼容旧调用点
    void emit(const std::string& opcode);

    void emitUnlock(const std::string& unlock);
    void emitUnlockName(const std::string& unlockName);

    std::pair<std::vector<std::string>,
              std::unordered_map<std::string, std::string>>
    instructions() const;

    // Typed artifact is the generator's authoritative representation.  The
    // legacy pair returned by instructions() is now an explicit compatibility
    // serialization boundary for existing passes and external consumers.
    BytecodeArtifact artifact() const;

    void mergeSubOverall();
    void mergeSubUnoverall();

    void clear();
    std::string subStr() const;
    std::string str() const;

    // Total instruction atoms emitted into the committed and current
    // function buffers. Available in all builds so expansion budgets do not
    // depend on debugger support.
    size_t getCurrentPC() const {
        return m_instruct.size() + m_subInstruct.size();
    }

#ifdef ENABLE_DEBUGGER
    using DebugInfoCallback = std::function<void(size_t pc, const std::string& opcode, const std::string& operand, const apc_debug::SourceLocation& loc)>;
    void setDebugInfoCallback(DebugInfoCallback callback) {
        m_debugInfoCallback = callback;
    }

    void setCurrentLocation(const apc_debug::SourceLocation& loc) {
        m_currentLocation = loc;
    }
#endif

private:
    void appendInstruction(BytecodeInstruction instruction);
    std::string serialize(const BytecodeInstruction& instruction) const;

    std::vector<BytecodeInstruction> m_subInstruct;
    std::vector<BytecodeInstruction> m_instruct;
    InstructionId m_nextInstructionId{0};
    std::pair<std::string, std::string> m_subUninstruct;
    std::unordered_map<std::string, std::string> m_uninstruct;

#ifdef ENABLE_DEBUGGER
    DebugInfoCallback m_debugInfoCallback;
    apc_debug::SourceLocation m_currentLocation;
#endif
};
SPACE_TBC_END
#endif // BYTECODE_GENERATOR_H
