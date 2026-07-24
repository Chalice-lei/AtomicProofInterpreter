#ifndef BYTECODE_FINALIZE_PASS_H
#define BYTECODE_FINALIZE_PASS_H

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bytecode/bytecode_instruction_utils.h"
#include "bytecode/bytecode_opcodes.h"
#include "include/pass_type.h"
#include "log/logger.h"
#include "pass/pass.h"
#include "pass/pass_context.h"
#include "pass/pass_context_keys.h"
#include "pass/pass_macros.h"

#ifdef ENABLE_DEBUGGER
#include "debugger/info/debug_info.h"
#include "debugger/info/debug_info_save.h"
#endif

// Final bytecode layout pass.
//
// The compiler may append immutable suffix/state data after OP_RETURN. Padding
// must therefore be inserted between the executable region and that suffix,
// after any OP_ENDIF instructions which structurally close the final Return's
// branch and after peephole optimization has settled the code length.
class BytecodeFinalizePass : public Pass
{
    DECLARE_PASS(BytecodeFinalizePass)

public:
    void execute(PassContext& data) override
    {
        LOG_DEBUG("BytecodeFinalizePass::execute - Start Execution");

        if (!data.contains<apc_pipeline::BytecodeOutput>(
                apc_pipeline::key::kBytecode
            )) {
            LOG_DEBUG("BytecodeFinalizePass::execute - no bytcode in context");
            return;
        }

        auto bytPtr = data.get<apc_pipeline::BytecodeOutput>(
            apc_pipeline::key::kBytecode
        );
        if (!bytPtr || bytPtr->first.empty()) {
            return;
        }

        auto& instrs = bytPtr->first;

#ifdef ENABLE_DEBUGGER
        auto debugInfo =
            data.tryGet<apc_debug::DebugInfo>(apc_pipeline::key::kDebugInfo);
#endif

        const auto returnIt = std::find_if(
            instrs.rbegin(), instrs.rend(), [](const std::string& instr) {
                return isSingleOpcode(instr, tbc::BytOpcode::OP_RETURN);
            }
        );

        if (returnIt == instrs.rend()) {
            LOG_DEBUG(
                "BytecodeFinalizePass: no OP_RETURN found, skipping padding"
            );
#ifdef ENABLE_DEBUGGER
            apc_debug::saveDebugInfoIfRequested(data, debugInfo);
#endif
            return;
        }

        const size_t returnIndex =
            static_cast<size_t>(std::distance(returnIt, instrs.rend()) - 1);

        // 最后一个 Return 可能位于嵌套 if 的分支中，后面仍有语法必需的
        // ENDIF。旧 padding 可能已经位于 Return 与这些闭合操作码之间；
        // 先剥离旧 padding，再把闭合操作码纳入 executable code。
        size_t structuralStart = returnIndex + 1;
        while (structuralStart < instrs.size() &&
               isSingleOpcode(instrs[structuralStart],
                              tbc::BytOpcode::OP_INVALIDOPCODE)) {
            ++structuralStart;
        }
        size_t structuralEnd = structuralStart;
        while (structuralEnd < instrs.size() &&
               isSingleOpcode(instrs[structuralEnd],
                              tbc::BytOpcode::OP_ENDIF)) {
            ++structuralEnd;
        }
        size_t suffixStart = structuralEnd;
        while (suffixStart < instrs.size() &&
               isSingleOpcode(instrs[suffixStart],
                              tbc::BytOpcode::OP_INVALIDOPCODE)) {
            ++suffixStart;
        }

        const auto prefixBytesOpt = countBytes(instrs, 0, returnIndex + 1);
        const auto structuralBytesOpt =
            countBytes(instrs, structuralStart, structuralEnd);
        if (!prefixBytesOpt.has_value() ||
            !structuralBytesOpt.has_value()) {
            LOG_WARNING(
                "BytecodeFinalizePass: non-hex instruction before OP_RETURN; "
                "cannot determine final padding"
            );
#ifdef ENABLE_DEBUGGER
            apc_debug::saveDebugInfoIfRequested(data, debugInfo);
#endif
            return;
        }

        const size_t codeBytes =
            prefixBytesOpt.value() + structuralBytesOpt.value();
        const size_t paddingBytes = bytesToAlign(codeBytes, 64);
        const size_t oldPaddingBytes =
            (structuralStart - (returnIndex + 1)) +
            (suffixStart - structuralEnd);
        const size_t suffixBytes =
            countBestEffortBytes(instrs, suffixStart, instrs.size());
        const size_t beforeBytes =
            countBestEffortBytes(instrs, 0, instrs.size());

#ifdef ENABLE_DEBUGGER
        std::vector<size_t> pcRemap(
            instrs.size(), std::numeric_limits<size_t>::max()
        );
        for (size_t i = 0; i <= returnIndex; ++i) {
            pcRemap[i] = i;
        }
        for (size_t i = structuralStart; i < structuralEnd; ++i) {
            pcRemap[i] = returnIndex + 1 + (i - structuralStart);
        }
        // immutable suffix 位于 OP_INVALIDOPCODE padding 之后，不可执行，
        // 因此不保留源码/指令映射；函数与块范围也会收缩到真实代码区。
#endif

        std::vector<std::string> finalized;
        finalized.reserve(
            returnIndex + 1 + (structuralEnd - structuralStart) +
            paddingBytes + (instrs.size() - suffixStart)
        );
        finalized.insert(finalized.end(), instrs.begin(),
                         instrs.begin() + returnIndex + 1);
        finalized.insert(finalized.end(),
                         instrs.begin() + structuralStart,
                         instrs.begin() + structuralEnd);
        for (size_t i = 0; i < paddingBytes; ++i) {
            finalized.push_back(
                tbc::opcodeToHex(tbc::BytOpcode::OP_INVALIDOPCODE)
            );
        }
        finalized.insert(finalized.end(),
                         instrs.begin() + suffixStart,
                         instrs.end());

        instrs = std::move(finalized);

#ifdef ENABLE_DEBUGGER
        if (debugInfo) {
            debugInfo->remapPCs(pcRemap, instrs.size());
            debugInfo->syncInstructionOpcodes(instrs);
        }
        apc_debug::saveDebugInfoIfRequested(data, debugInfo);
#endif

        const size_t afterBytes =
            countBestEffortBytes(instrs, 0, instrs.size());
        LOG_INFO(
            "BytecodeFinalizePass: code bytes ",
            codeBytes,
            ", padding ",
            oldPaddingBytes,
            " -> ",
            paddingBytes,
            ", suffix bytes ",
            suffixBytes,
            ", total bytes ",
            beforeBytes,
            " -> ",
            afterBytes
        );
    }

    std::vector<std::string> getDependencies() const override
    {
        return {DependTypeToString(DependType::BytecodePeepholePass)};
    }

private:
    static std::string normalizeHex(std::string value)
    {
        return tbc::bytecode_instruction::normalizeHex(std::move(value));
    }

    static bool isPureHex(const std::string& value)
    {
        return tbc::bytecode_instruction::isPureHexNormalizedEven(value);
    }

    static bool isIgnored(const std::string& value)
    {
        return tbc::bytecode_instruction::isIgnoredLine(value);
    }

    static bool isSingleOpcode(const std::string& value, tbc::BytOpcode opcode)
    {
        return tbc::bytecode_instruction::isSingleOpcodeNormalized(
            value, opcode
        );
    }

    static std::optional<size_t> instructionBytes(const std::string& value)
    {
        return tbc::bytecode_instruction::instructionBytesNormalized(value);
    }

    static std::optional<size_t> countBytes(
        const std::vector<std::string>& instrs,
        size_t start,
        size_t end
    )
    {
        return tbc::bytecode_instruction::countBytesNormalized(
            instrs, start, end
        );
    }

    static size_t countBestEffortBytes(
        const std::vector<std::string>& instrs,
        size_t start,
        size_t end
    )
    {
        return tbc::bytecode_instruction::countBestEffortBytesNormalized(
            instrs, start, end
        );
    }

    static size_t bytesToAlign(size_t length, size_t alignment)
    {
        return tbc::bytecode_instruction::bytesToAlign(length, alignment);
    }
};

#endif // BYTECODE_FINALIZE_PASS_H
