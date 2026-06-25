#ifndef BYTECODE_PEEPHOLE_PASS_H
#define BYTECODE_PEEPHOLE_PASS_H

#include <limits>
#include <memory>
#include <string>
#include <utility>
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

// 字节码 peephole 优化: 仅对相邻两条纯 hex 指令做窗口重写,
// 占位符 / 注释行 / 控制流操作码会阻断配对.
class BytecodePeepholePass : public Pass
{
    DECLARE_PASS(BytecodePeepholePass)
public:
    void execute(PassContext& data) override
    {
        LOG_DEBUG("BytecodePeepholePass::execute - Start Execution");

        if (!data.contains<apc_pipeline::BytecodeOutput>(
                apc_pipeline::key::kBytecode
            )) {
            LOG_DEBUG("BytecodePeepholePass::execute - no bytcode in context");
            return;
        }
        auto bytPtr = data.get<apc_pipeline::BytecodeOutput>(
            apc_pipeline::key::kBytecode
        );
        if (!bytPtr || bytPtr->first.empty()) {
            return;
        }

        auto& instrs = bytPtr->first;
        const size_t before = countBytes(instrs);

#ifdef ENABLE_DEBUGGER
        auto debugInfo =
            data.tryGet<apc_debug::DebugInfo>(apc_pipeline::key::kDebugInfo);
#endif

        // 不动点迭代直到无变化, 上限仅作防御.
        const int kMaxRounds = 16;
        int round = 0;
        bool changed = true;
        while (changed && round < kMaxRounds) {
            std::vector<size_t> pcRemap;
#ifdef ENABLE_DEBUGGER
            changed = runOnePass(instrs, debugInfo ? &pcRemap : nullptr);
            if (debugInfo) {
                debugInfo->remapPCs(pcRemap, instrs.size());
                debugInfo->syncInstructionOpcodes(instrs);
            }
#else
            changed = runOnePass(instrs);
#endif
            ++round;
        }

#ifdef ENABLE_DEBUGGER
        apc_debug::saveDebugInfoIfRequested(data, debugInfo);
#endif

        const size_t after = countBytes(instrs);
        LOG_INFO(
            "BytecodePeepholePass: bytes ",
            before,
            " -> ",
            after,
            " (rounds=",
            round,
            ")"
        );
    }

    std::vector<std::string> getDependencies() const override
    {
        return {DependTypeToString(DependType::ASTToBytecodePass)};
    }

private:
    static bool isPureHex(const std::string& s)
    {
        return tbc::bytecode_instruction::isPureHexStrictEven(s);
    }

    // 首字节值, 非 hex 返回 -1.
    static int firstByte(const std::string& s)
    {
        if (!isPureHex(s) || s.size() < 2) {
            return -1;
        }
        return std::stoi(s.substr(0, 2), nullptr, 16);
    }

    // 纯常量压栈指令 (无副作用).
    static bool isPushConst(const std::string& s)
    {
        int op = firstByte(s);
        if (op < 0) {
            return false;
        }
        if (op == 0x00) {
            return true; // OP_0
        }
        if (op == 0x4f) {
            return true; // OP_1NEGATE
        }
        if (op >= 0x51 && op <= 0x60) {
            return true; // OP_1..OP_16
        }
        if (op >= 0x01 && op <= 0x4b) {
            return true; // 直接长度推送
        }
        if (op == 0x4c || op == 0x4d || op == 0x4e) {
            return true; // OP_PUSHDATA1/2/4
        }
        return false;
    }

    static bool isSingleOp(const std::string& s, uint8_t op)
    {
        return s.size() == 2 && firstByte(s) == static_cast<int>(op);
    }

    static bool isOpDrop(const std::string& s)
    {
        return isSingleOp(s, static_cast<uint8_t>(tbc::BytOpcode::OP_DROP));
    }
    static bool isOpDup(const std::string& s)
    {
        return isSingleOp(s, static_cast<uint8_t>(tbc::BytOpcode::OP_DUP));
    }
    static bool isOpSwap(const std::string& s)
    {
        return isSingleOp(s, static_cast<uint8_t>(tbc::BytOpcode::OP_SWAP));
    }
    static bool isOpNot(const std::string& s)
    {
        return isSingleOp(s, static_cast<uint8_t>(tbc::BytOpcode::OP_NOT));
    }
    static bool isOpZero(const std::string& s)
    {
        return isSingleOp(s, static_cast<uint8_t>(tbc::BytOpcode::OP_0));
    }
    static bool isOpEqual(const std::string& s)
    {
        return isSingleOp(s, static_cast<uint8_t>(tbc::BytOpcode::OP_EQUAL));
    }
    static bool isOpNumEqual(const std::string& s)
    {
        return isSingleOp(
            s, static_cast<uint8_t>(tbc::BytOpcode::OP_NUMEQUAL)
        );
    }

    // 控制流指令: 阻断 peephole 跨边界配对.
    static bool isControlFlow(const std::string& s)
    {
        int op = firstByte(s);
        if (op < 0) {
            return false;
        }
        switch (op) {
            case static_cast<int>(tbc::BytOpcode::OP_IF):
            case static_cast<int>(tbc::BytOpcode::OP_NOTIF):
            case static_cast<int>(tbc::BytOpcode::OP_ELSE):
            case static_cast<int>(tbc::BytOpcode::OP_ENDIF):
            case static_cast<int>(tbc::BytOpcode::OP_VERIFY):
            case static_cast<int>(tbc::BytOpcode::OP_RETURN):
            case static_cast<int>(tbc::BytOpcode::OP_CODESEPARATOR):
                return true;
            default:
                return false;
        }
    }

    // 注释行 / 非 hex 占位符: 不可折叠.
    static bool isInert(const std::string& s)
    {
        if (s.empty()) {
            return true;
        }
        if (s[0] == '#') {
            return true;
        }
        return !isPureHex(s);
    }

    static std::string opHex(tbc::BytOpcode op)
    {
        return tbc::opcodeToHex(op);
    }

    static void keepInstruction(
        const std::vector<std::string>& v,
        std::vector<std::string>& out,
        std::vector<size_t>* oldToNew,
        size_t oldPC
    )
    {
        if (oldToNew) {
            (*oldToNew)[oldPC] = out.size();
        }
        out.push_back(v[oldPC]);
    }

    static void replacePairWithOne(
        std::vector<std::string>& out,
        std::vector<size_t>* oldToNew,
        size_t oldPC,
        const std::string& replacement
    )
    {
        if (oldToNew) {
            (*oldToNew)[oldPC] = out.size();
        }
        out.push_back(replacement);
    }

    // 一轮扫描, 返回是否发生替换.
    static bool runOnePass(
        std::vector<std::string>& v,
        std::vector<size_t>* oldToNew = nullptr
    )
    {
        if (oldToNew) {
            oldToNew->assign(v.size(), std::numeric_limits<size_t>::max());
        }

        std::vector<std::string> out;
        out.reserve(v.size());
        bool changed = false;
        size_t i = 0;
        while (i < v.size()) {
            const std::string& cur = v[i];

            if (isInert(cur)) {
                keepInstruction(v, out, oldToNew, i);
                ++i;
                continue;
            }

            if (isControlFlow(cur)) {
                keepInstruction(v, out, oldToNew, i);
                ++i;
                continue;
            }

            if (i + 1 < v.size()) {
                const std::string& nxt = v[i + 1];
                if (!isInert(nxt) && !isControlFlow(nxt)) {
                    // 规则 1: push_const + OP_DROP -> ∅
                    if (isPushConst(cur) && isOpDrop(nxt)) {
                        i += 2;
                        changed = true;
                        continue;
                    }
                    // 规则 2: OP_DUP + OP_DROP -> ∅
                    if (isOpDup(cur) && isOpDrop(nxt)) {
                        i += 2;
                        changed = true;
                        continue;
                    }
                    // 规则 3: OP_SWAP + OP_SWAP -> ∅
                    if (isOpSwap(cur) && isOpSwap(nxt)) {
                        i += 2;
                        changed = true;
                        continue;
                    }
                    // 规则 4: OP_NOT + OP_NOT -> OP_0NOTEQUAL (省 1 字节)
                    if (isOpNot(cur) && isOpNot(nxt)) {
                        replacePairWithOne(
                            out,
                            oldToNew,
                            i,
                            opHex(tbc::BytOpcode::OP_0NOTEQUAL)
                        );
                        i += 2;
                        changed = true;
                        continue;
                    }
                    // 规则 5: OP_DROP + OP_DROP -> OP_2DROP (省 1 字节)
                    if (isOpDrop(cur) && isOpDrop(nxt)) {
                        replacePairWithOne(
                            out,
                            oldToNew,
                            i,
                            opHex(tbc::BytOpcode::OP_2DROP)
                        );
                        i += 2;
                        changed = true;
                        continue;
                    }
                    // 规则 6: OP_0 + OP_EQUAL -> OP_NOT (x==0 的更短写法)
                    if (isOpZero(cur) && isOpEqual(nxt)) {
                        replacePairWithOne(
                            out,
                            oldToNew,
                            i,
                            opHex(tbc::BytOpcode::OP_NOT)
                        );
                        i += 2;
                        changed = true;
                        continue;
                    }
                    // 规则 7: OP_0 + OP_NUMEQUAL -> OP_NOT
                    if (isOpZero(cur) && isOpNumEqual(nxt)) {
                        replacePairWithOne(
                            out,
                            oldToNew,
                            i,
                            opHex(tbc::BytOpcode::OP_NOT)
                        );
                        i += 2;
                        changed = true;
                        continue;
                    }
                }
            }

            keepInstruction(v, out, oldToNew, i);
            ++i;
        }
        if (changed) {
            v = std::move(out);
        }
        return changed;
    }

    // 仅统计纯 hex 指令的字节数.
    static size_t countBytes(const std::vector<std::string>& v)
    {
        return tbc::bytecode_instruction::countStrictHexBytes(v);
    }
};

#endif // BYTECODE_PEEPHOLE_PASS_H
