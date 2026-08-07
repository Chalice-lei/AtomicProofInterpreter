#ifndef BYTECODE_PEEPHOLE_PASS_H
#define BYTECODE_PEEPHOLE_PASS_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bytecode/bytecode_opcodes.h"
#include "bytecode/bytecode_artifact_rewriter.h"
#include "bytecode/script_codec.h"
#include "bytecode/structured_if_tail_optimizer.h"
#include "include/pass_type.h"
#include "log/logger.h"
#include "pass/pass.h"
#include "pass/pass_context.h"
#include "pass/pass_context_keys.h"
#include "pass/pass_macros.h"

#ifdef ENABLE_DEBUGGER
#include "debugger/info/debug_info_save.h"
#endif

// 字节码 peephole 优化: 仅对相邻 2-3 条纯 hex 指令做窗口重写。
// 占位符 / 注释行 / 控制流操作码通常会阻断配对；仅允许规则中明确列出的
// IF/NOTIF/VERIFY 终止融合。
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

        auto& committedInstrs = bytPtr->first;
        auto artifact = data.tryGet<tbc::BytecodeArtifact>(
            apc_pipeline::key::kBytecodeArtifact
        );
        if (artifact) {
            const auto typedView =
                tbc::LegacyBytecodeAdapter::exportPreserving(*artifact);
            if (typedView.first != committedInstrs) {
                LOG_WARNING(
                    "BytecodePeepholePass: legacy view was stale; "
                    "restoring it from the typed artifact"
                );
                committedInstrs = typedView.first;
                bytPtr->second = typedView.second;
            }
        }

        const std::vector<std::string> originalInstrs = committedInstrs;
        std::vector<std::string> instrs = originalInstrs;
        const size_t originalExecutableBytes = countBytes(
            originalInstrs,
            0,
            optimizablePrefixLength(originalInstrs, artifact.get())
        );
        const size_t before = countBytes(instrs);

        std::optional<tbc::BytecodeArtifact> workingArtifact;
        if (artifact && artifact->lockingScript.size() == instrs.size()) {
            workingArtifact = *artifact;
        }

#ifdef ENABLE_DEBUGGER
        auto debugInfo = data.tryGet<apc_debug::DebugInfo>(
            apc_pipeline::key::kDebugInfo
        );
        const bool allowTrueVerifyElision = !debugInfo;
        std::shared_ptr<apc_debug::DebugInfo> workingDebugInfo;
        if (debugInfo) {
            workingDebugInfo = apc_debug::DebugInfo::fromJson(
                debugInfo->toJson()
            );
            if (!workingDebugInfo) {
                LOG_WARNING(
                    "BytecodePeepholePass: cannot clone DebugInfo; "
                    "leaving bytecode unchanged"
                );
                return;
            }
        }
#else
        const bool allowTrueVerifyElision = true;
#endif

        auto applyArtifactRewrite = [
            &workingArtifact
        ](
            const std::vector<std::string>& rewritten,
            const std::vector<std::vector<size_t>>& newToOld
        ) {
            if (!workingArtifact.has_value()) {
                return true;
            }
            std::string error;
            auto candidate = tbc::BytecodeArtifactRewriter::rewrite(
                *workingArtifact, rewritten, newToOld, &error
            );
            if (!candidate.has_value()) {
                LOG_WARNING(
                    "BytecodePeepholePass: typed artifact rewrite failed: ",
                    error
                );
                return false;
            }
            workingArtifact = std::move(candidate.value());
            return true;
        };

        LocalRewriteOptions localOptions;
        if (artifact &&
            artifact->format == tbc::ArtifactFormat::CanonicalV2) {
            localOptions.pushEncodingPolicy =
                tbc::PushEncodingPolicy::Canonical;
        }
        auto selfPlaceholderLengths = data.tryGet<
            std::unordered_map<std::string, size_t>>(
            apc_pipeline::key::kSelfPlaceholderLengths
        );
        if (selfPlaceholderLengths) {
            for (const auto& [label, byteLength] : *selfPlaceholderLengths) {
                localOptions.knownDataPlaceholders.emplace(label, byteLength);
                localOptions.knownDataPlaceholders.emplace(
                    label + std::to_string(byteLength), byteLength
                );
            }
        }

        // 不动点迭代直到无变化, 上限仅作防御.
        const int kMaxRounds = 16;
        int round = 0;
        bool changed = true;
        bool localMappingFailed = false;
        while (changed && round < kMaxRounds) {
            const std::vector<std::string> beforeRound = instrs;
            std::vector<size_t> pcRemap;
            changed = runOnePass(
                instrs,
                &pcRemap,
                allowTrueVerifyElision,
                localOptions,
                workingArtifact ? &*workingArtifact : nullptr
            );
            if (changed) {
                const auto newToOld =
                    tbc::BytecodeArtifactRewriter::reverseMapping(
                        pcRemap, instrs.size()
                    );
                if (newToOld.size() != instrs.size() ||
                    !applyArtifactRewrite(instrs, newToOld)) {
                    localMappingFailed = true;
                }
#ifdef ENABLE_DEBUGGER
                if (!localMappingFailed && workingDebugInfo) {
                    auto remapped = workingDebugInfo->remapped(
                        pcRemap, instrs.size()
                    );
                    if (!remapped) {
                        localMappingFailed = true;
                    } else {
                        remapped->syncInstructionOpcodes(instrs);
                        workingDebugInfo = std::move(remapped);
                    }
                }
#endif
                if (localMappingFailed) {
                    instrs = beforeRound;
                    break;
                }
            }
            ++round;
        }

        if (localMappingFailed) {
            LOG_WARNING(
                "BytecodePeepholePass: local provenance rewrite failed; "
                "leaving bytecode and metadata unchanged"
            );
            return;
        }

        // Common branch tails carry all old PCs plus their final-stream branch
        // predicates.  This permits the same transformation in Debug and
        // Release builds while retaining source-specific breakpoint behavior.
        size_t mergedIfCount = 0;
        {
            const size_t executableEnd = optimizablePrefixLength(
                instrs, workingArtifact ? &*workingArtifact : nullptr
            );
            const std::vector<std::string> executable(
                instrs.begin(), instrs.begin() + executableEnd
            );
            tbc::StructuredIfTailOptions structuredOptions;
            if (selfPlaceholderLengths) {
                for (const auto& [label, byteLength] :
                     *selfPlaceholderLengths) {
                    structuredOptions.knownDataPlaceholderLabels.insert(
                        label
                    );
                    structuredOptions.knownDataPlaceholderLabels.insert(
                        label + std::to_string(byteLength)
                    );
                }
            }
            auto structured = tbc::StructuredIfTailOptimizer::optimize(
                executable, structuredOptions
            );
            if (!structured.structurallyValid) {
                LOG_WARNING(
                    "BytecodePeepholePass: malformed IF structure; "
                    "skipping common-tail optimization"
                );
            } else if (structured.changed && structured.rewritePlanValid) {
                const auto beforeStructuredInstrs = instrs;
                const auto beforeStructuredArtifact = workingArtifact;
#ifdef ENABLE_DEBUGGER
                const auto beforeStructuredDebug = workingDebugInfo;
#endif

                std::vector<size_t> fullOldToNew(
                    instrs.size(), std::numeric_limits<size_t>::max()
                );
                std::copy(
                    structured.rewritePlan.oldToNew.begin(),
                    structured.rewritePlan.oldToNew.end(),
                    fullOldToNew.begin()
                );

                std::vector<std::vector<size_t>> artifactOrigins;
                artifactOrigins.reserve(
                    structured.instructions.size() +
                    (instrs.size() - executableEnd)
                );
#ifdef ENABLE_DEBUGGER
                std::vector<std::vector<apc_debug::OriginRewriteRef>>
                    debugOrigins;
                debugOrigins.reserve(
                    structured.instructions.size() +
                    (instrs.size() - executableEnd)
                );
#endif
                for (const auto& origins :
                     structured.rewritePlan.newToOld) {
                    std::vector<size_t> oldPCs;
                    oldPCs.reserve(origins.size());
#ifdef ENABLE_DEBUGGER
                    std::vector<apc_debug::OriginRewriteRef> debugRefs;
                    debugRefs.reserve(origins.size());
#endif
                    for (const auto& origin : origins) {
                        oldPCs.push_back(origin.oldPC);
#ifdef ENABLE_DEBUGGER
                        apc_debug::OriginRewriteRef converted;
                        converted.oldPC = origin.oldPC;
                        for (const auto& predicate : origin.path) {
                            converted.path.push_back(
                                {static_cast<apc_debug::ControlRegionId>(
                                     predicate.ifPC
                                 ),
                                 predicate.arm == tbc::BranchArm::THEN
                                     ? apc_debug::BranchArm::Then
                                     : apc_debug::BranchArm::Else}
                            );
                        }
                        debugRefs.push_back(std::move(converted));
#endif
                    }
                    artifactOrigins.push_back(std::move(oldPCs));
#ifdef ENABLE_DEBUGGER
                    debugOrigins.push_back(std::move(debugRefs));
#endif
                }

                const size_t newExecutableEnd =
                    structured.instructions.size();
                for (size_t oldPC = executableEnd;
                     oldPC < instrs.size(); ++oldPC) {
                    const size_t newPC = newExecutableEnd +
                                         (oldPC - executableEnd);
                    fullOldToNew[oldPC] = newPC;
                    artifactOrigins.push_back({oldPC});
#ifdef ENABLE_DEBUGGER
                    debugOrigins.push_back(
                        {{oldPC, {}}}
                    );
#endif
                }

                structured.instructions.insert(
                    structured.instructions.end(),
                    instrs.begin() + executableEnd,
                    instrs.end()
                );
                instrs = std::move(structured.instructions);

                bool structuredMappingValid =
                    applyArtifactRewrite(instrs, artifactOrigins);
#ifdef ENABLE_DEBUGGER
                if (structuredMappingValid && workingDebugInfo) {
                    auto remapped = workingDebugInfo->remapped(
                        fullOldToNew, instrs.size(), debugOrigins
                    );
                    if (!remapped) {
                        structuredMappingValid = false;
                    } else {
                        remapped->syncInstructionOpcodes(instrs);
                        workingDebugInfo = std::move(remapped);
                    }
                }
#endif
                if (!structuredMappingValid) {
                    LOG_WARNING(
                        "BytecodePeepholePass: structured provenance "
                        "validation failed; retaining local rewrites"
                    );
                    instrs = beforeStructuredInstrs;
                    workingArtifact = beforeStructuredArtifact;
#ifdef ENABLE_DEBUGGER
                    workingDebugInfo = beforeStructuredDebug;
#endif
                } else {
                    mergedIfCount = structured.mergedIfCount;
                }

                // Hoisting can expose a new local window at the parent
                // sequence. Normalize it before final padding is computed.
                bool cleanupChanged = structuredMappingValid;
                int cleanupRounds = 0;
                while (cleanupChanged && cleanupRounds < kMaxRounds) {
                    const auto beforeCleanupInstrs = instrs;
                    const auto beforeCleanupArtifact = workingArtifact;
#ifdef ENABLE_DEBUGGER
                    const auto beforeCleanupDebug = workingDebugInfo;
#endif
                    std::vector<size_t> cleanupRemap;
                    cleanupChanged = runOnePass(
                        instrs,
                        &cleanupRemap,
                        allowTrueVerifyElision,
                        localOptions,
                        workingArtifact ? &*workingArtifact : nullptr
                    );
                    if (cleanupChanged) {
                        const auto cleanupOrigins =
                            tbc::BytecodeArtifactRewriter::reverseMapping(
                                cleanupRemap, instrs.size()
                            );
                        bool cleanupValid =
                            cleanupOrigins.size() == instrs.size() &&
                            applyArtifactRewrite(instrs, cleanupOrigins);
#ifdef ENABLE_DEBUGGER
                        if (cleanupValid && workingDebugInfo) {
                            auto remapped = workingDebugInfo->remapped(
                                cleanupRemap, instrs.size()
                            );
                            if (!remapped) {
                                cleanupValid = false;
                            } else {
                                remapped->syncInstructionOpcodes(instrs);
                                workingDebugInfo = std::move(remapped);
                            }
                        }
#endif
                        if (!cleanupValid) {
                            instrs = beforeCleanupInstrs;
                            workingArtifact = beforeCleanupArtifact;
#ifdef ENABLE_DEBUGGER
                            workingDebugInfo = beforeCleanupDebug;
#endif
                            cleanupChanged = false;
                            LOG_WARNING(
                                "BytecodePeepholePass: cleanup provenance "
                                "validation failed; retaining structured "
                                "result before cleanup"
                            );
                        }
                    }
                    ++cleanupRounds;
                }
                round += cleanupRounds;
            }
        }

        // Removing a constant-true VERIFY is useful only when some executable
        // byte remains. Other pair deletions in the same fixed point may erase
        // that apparent remainder, so validate the committed result rather
        // than relying on the input instruction count of an individual round.
        // This rollback is intentionally narrow: it preserves the historical
        // behavior of scripts such as "push; DROP" while preventing the new
        // OP_1/VERIFY rule from turning a previously nonempty script empty.
        const size_t executableBytes = countBytes(
            instrs,
            0,
            optimizablePrefixLength(
                instrs, workingArtifact ? &*workingArtifact : nullptr
            )
        );
        if (allowTrueVerifyElision && originalExecutableBytes != 0 &&
            executableBytes == 0 &&
            containsTrueVerifyPair(originalInstrs, artifact.get())) {
            LOG_DEBUG(
                "BytecodePeepholePass: retaining OP_1 VERIFY to keep the "
                "executable script nonempty"
            );
            instrs = originalInstrs;
            if (artifact) {
                workingArtifact = *artifact;
            }
#ifdef ENABLE_DEBUGGER
            if (debugInfo) {
                workingDebugInfo = apc_debug::DebugInfo::fromJson(
                    debugInfo->toJson()
                );
            }
#endif
            mergedIfCount = 0;
        }

        committedInstrs = instrs;
        if (artifact && workingArtifact.has_value()) {
            *artifact = std::move(*workingArtifact);
        }

#ifdef ENABLE_DEBUGGER
        if (debugInfo && workingDebugInfo) {
            *debugInfo = std::move(*workingDebugInfo);
        }
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
            ", merged_if=",
            mergedIfCount,
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
        if (s.empty() || (s.size() % 2) != 0) {
            return false;
        }
        for (char c : s) {
            if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'f') ||
                  ('A' <= c && c <= 'F'))) {
                return false;
            }
        }
        return true;
    }

    // 首字节值, 非 hex 返回 -1.
    static int firstByte(const std::string& s)
    {
        if (!isPureHex(s) || s.size() < 2) {
            return -1;
        }
        return std::stoi(s.substr(0, 2), nullptr, 16);
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
    static bool isOpOver(const std::string& s)
    {
        return isSingleOp(s, static_cast<uint8_t>(tbc::BytOpcode::OP_OVER));
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
    static bool isOpNumNotEqual(const std::string& s)
    {
        return isSingleOp(
            s, static_cast<uint8_t>(tbc::BytOpcode::OP_NUMNOTEQUAL)
        );
    }

    static bool isOp(const std::string& s, tbc::BytOpcode op)
    {
        return isSingleOp(s, static_cast<uint8_t>(op));
    }

    // OP_RETURN may be followed by contract state/data. When typed metadata
    // exists it is authoritative: an immutable RawSuffix atom may itself be
    // the byte 6a and must never be mistaken for executable OP_RETURN. Only
    // the legacy-only fallback infers the boundary from string atoms.
    static size_t optimizablePrefixLength(
        const std::vector<std::string>& instructions,
        const tbc::BytecodeArtifact* artifact = nullptr
    )
    {
        if (artifact) {
            if (artifact->lockingScript.size() != instructions.size()) {
                return 0;
            }

            size_t executableEnd = 0;
            std::optional<size_t> finalReturnEnd;
            bool leftExecutableRegion = false;
            for (size_t i = 0; i < artifact->lockingScript.size(); ++i) {
                const auto& instruction = artifact->lockingScript[i];
                if (instruction.region != tbc::ScriptRegion::Executable) {
                    leftExecutableRegion = true;
                    continue;
                }
                if (leftExecutableRegion) {
                    // Malformed region ordering is not a reason to fall back
                    // to parsing compatibility strings. Fail closed instead.
                    return 0;
                }

                executableEnd = i + 1;
                const auto* opcode =
                    std::get_if<tbc::OpcodeInstruction>(&instruction.body);
                if (opcode && opcode->opcode == tbc::BytOpcode::OP_RETURN) {
                    finalReturnEnd = i + 1;
                }
            }
            return finalReturnEnd.value_or(executableEnd);
        }

        for (size_t i = instructions.size(); i > 0; --i) {
            if (isOp(instructions[i - 1], tbc::BytOpcode::OP_RETURN)) {
                return i;
            }
        }
        return instructions.size();
    }

    static bool containsTrueVerifyPair(
        const std::vector<std::string>& instructions,
        const tbc::BytecodeArtifact* artifact = nullptr
    )
    {
        const size_t end = optimizablePrefixLength(instructions, artifact);
        for (size_t i = 0; i + 1 < end; ++i) {
            if (isOp(instructions[i], tbc::BytOpcode::OP_1) &&
                isOp(instructions[i + 1], tbc::BytOpcode::OP_VERIFY)) {
                return true;
            }
        }
        return false;
    }

    // OP_NOT 与 OP_IF/OP_NOTIF 只在输入是规范布尔值时可无条件融合。
    // 对任意字节串，数值 OP_NOT 与控制流的 truthiness 转换可能具有不同
    // 的非法数值行为。因此仅在紧邻的前一条指令可证明产生 0/1 时融合。
    static bool producesCanonicalBool(const std::string& s)
    {
        static const tbc::BytOpcode kCanonicalBoolProducers[] = {
            tbc::BytOpcode::OP_0,
            tbc::BytOpcode::OP_1,
            tbc::BytOpcode::OP_NOT,
            tbc::BytOpcode::OP_0NOTEQUAL,
            tbc::BytOpcode::OP_EQUAL,
            tbc::BytOpcode::OP_NUMEQUAL,
            tbc::BytOpcode::OP_NUMNOTEQUAL,
            tbc::BytOpcode::OP_LESSTHAN,
            tbc::BytOpcode::OP_GREATERTHAN,
            tbc::BytOpcode::OP_LESSTHANOREQUAL,
            tbc::BytOpcode::OP_GREATERTHANOREQUAL,
            tbc::BytOpcode::OP_BOOLAND,
            tbc::BytOpcode::OP_BOOLOR,
            tbc::BytOpcode::OP_WITHIN,
            tbc::BytOpcode::OP_CHECKSIG,
            tbc::BytOpcode::OP_CHECKMULTISIG,
        };
        for (auto op : kCanonicalBoolProducers) {
            if (isOp(s, op)) {
                return true;
            }
        }
        return false;
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

    struct LocalRewriteOptions
    {
        // Placeholder label without angle brackets -> payload byte length.
        std::unordered_map<std::string, size_t> knownDataPlaceholders;
        // Profitability is measured in the artifact's eventual output
        // encoding. A non-minimal Legacy push may shrink during CanonicalV2
        // materialization even when the compatibility string is unchanged.
        tbc::PushEncodingPolicy pushEncodingPolicy{
            tbc::PushEncodingPolicy::Legacy};
    };

    static uint8_t hexByteAt(const std::string& encoding, size_t byteIndex)
    {
        return static_cast<uint8_t>(std::stoul(
            encoding.substr(byteIndex * 2, 2), nullptr, 16
        ));
    }

    // Return the complete serialized size only for a structurally valid push.
    // Every rewrite which removes or evaluates a push must use this stronger
    // parser: looking only at the leading opcode can turn a truncated push or
    // a multi-op atom into executable bytecode with different failure behavior.
    static std::optional<size_t> wellFormedPushSize(
        const std::string& encoding
    )
    {
        if (!isPureHex(encoding)) {
            return std::nullopt;
        }
        const size_t bytes = encoding.size() / 2;
        const uint8_t opcode = hexByteAt(encoding, 0);
        if (opcode == 0x00 || opcode == 0x4f ||
            (opcode >= 0x51 && opcode <= 0x60)) {
            return bytes == 1 ? std::optional<size_t>(1) : std::nullopt;
        }
        if (opcode >= 0x01 && opcode <= 0x4b) {
            return bytes == static_cast<size_t>(opcode) + 1
                       ? std::optional<size_t>(bytes)
                       : std::nullopt;
        }

        size_t prefixBytes = 0;
        uint64_t payloadBytes = 0;
        if (opcode == 0x4c) {
            if (bytes < 2) {
                return std::nullopt;
            }
            prefixBytes = 2;
            payloadBytes = hexByteAt(encoding, 1);
        } else if (opcode == 0x4d) {
            if (bytes < 3) {
                return std::nullopt;
            }
            prefixBytes = 3;
            payloadBytes = static_cast<uint64_t>(hexByteAt(encoding, 1)) |
                           (static_cast<uint64_t>(hexByteAt(encoding, 2))
                            << 8);
        } else if (opcode == 0x4e) {
            if (bytes < 5) {
                return std::nullopt;
            }
            prefixBytes = 5;
            for (size_t index = 0; index < 4; ++index) {
                payloadBytes |=
                    static_cast<uint64_t>(hexByteAt(encoding, index + 1))
                    << (8 * index);
            }
        } else {
            return std::nullopt;
        }

        return payloadBytes == bytes - prefixBytes
                   ? std::optional<size_t>(bytes)
                   : std::nullopt;
    }

    static std::optional<size_t> knownPlaceholderPushSize(
        const std::string& encoding,
        const LocalRewriteOptions& options
    )
    {
        if (encoding.size() < 3 || encoding.front() != '<' ||
            encoding.back() != '>' ||
            encoding.find('<', 1) != std::string::npos ||
            encoding.find('>') != encoding.size() - 1) {
            return std::nullopt;
        }
        const std::string label = encoding.substr(1, encoding.size() - 2);
        auto found = options.knownDataPlaceholders.find(label);
        if (found == options.knownDataPlaceholders.end()) {
            return std::nullopt;
        }
        const size_t payload = found->second;
        if (payload > std::numeric_limits<uint32_t>::max()) {
            return std::nullopt;
        }
        const size_t prefix = payload <= 75     ? 1
                              : payload <= 255  ? 2
                              : payload <= 65535 ? 3
                                                 : 5;
        if (payload > std::numeric_limits<size_t>::max() - prefix) {
            return std::nullopt;
        }
        return payload + prefix;
    }

    // Decode the stack item produced by exactly one complete constant push.
    // The returned string is normalized payload hex (without a push prefix).
    // This is sufficient for OP_EQUAL folding, whose semantics are byte-wise.
    static std::optional<std::string> decodedPushPayload(
        const std::string& encoding
    )
    {
        if (!wellFormedPushSize(encoding)) {
            return std::nullopt;
        }

        const uint8_t opcode = hexByteAt(encoding, 0);
        if (opcode == 0x00) {
            return std::string{};
        }
        if (opcode == 0x4f) {
            return std::string{"81"};
        }
        if (opcode >= 0x51 && opcode <= 0x60) {
            static constexpr char kHexDigits[] = "0123456789abcdef";
            const uint8_t value = static_cast<uint8_t>(opcode - 0x50);
            return std::string{
                kHexDigits[(value >> 4) & 0x0f],
                kHexDigits[value & 0x0f]
            };
        }

        size_t prefixBytes = 1;
        if (opcode == 0x4c) {
            prefixBytes = 2;
        } else if (opcode == 0x4d) {
            prefixBytes = 3;
        } else if (opcode == 0x4e) {
            prefixBytes = 5;
        }

        std::string payload = encoding.substr(prefixBytes * 2);
        std::transform(
            payload.begin(),
            payload.end(),
            payload.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            }
        );
        return payload;
    }

    static bool isProfitableDuplicatePush(
        const std::string& first,
        const std::string& second,
        const LocalRewriteOptions& options
    )
    {
        if (first != second) {
            return false;
        }

        const auto decoded = tbc::ScriptCodec::decodePushHex(first, false);
        if (decoded.ok()) {
            const auto serializedSize = tbc::ScriptCodec::serializedPushSize(
                decoded.value->payload, options.pushEncodingPolicy
            );
            return serializedSize.has_value() && *serializedSize > 1;
        }

        const auto legacySize = knownPlaceholderPushSize(first, options);
        if (!legacySize.has_value()) {
            return false;
        }
        if (options.pushEncodingPolicy ==
            tbc::PushEncodingPolicy::Legacy) {
            return *legacySize > 1;
        }

        // Placeholder contents are intentionally unknown here. A one-byte
        // payload might materialize as OP_1..OP_16/OP_1NEGATE (one byte), so
        // DUP is provably smaller only once the payload itself exceeds one
        // byte. Empty pushes are also one-byte OP_0 in canonical form.
        const std::string label = first.substr(1, first.size() - 2);
        const auto known = options.knownDataPlaceholders.find(label);
        return known != options.knownDataPlaceholders.end() &&
               known->second > 1;
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
            // 融合后的指令继承第二条指令的来源。对 VERIFY/IF 融合，这会
            // 保留终止/控制流操作的断点与错误位置；对栈规范化，则保留
            // PICK/ROLL/DROP 等实际操作的来源。
            (*oldToNew)[oldPC + 1] = out.size();
        }
        out.push_back(replacement);
    }

    enum class RewriteKind { None, DeletePair, ReplacePair };

    struct PairRewrite
    {
        RewriteKind kind{RewriteKind::None};
        std::string replacement;
    };

    struct ReplacementInstruction
    {
        std::string encoding;
        // Source instruction within the consumed window whose provenance the
        // replacement inherits.
        size_t originOffset{0};
    };

    struct TripleRewrite
    {
        bool matched{false};
        std::vector<ReplacementInstruction> replacements;
    };

    static PairRewrite deletePair()
    {
        return {RewriteKind::DeletePair, {}};
    }

    static PairRewrite replacePair(tbc::BytOpcode replacement)
    {
        return {RewriteKind::ReplacePair, opHex(replacement)};
    }

    // Compute a conservative lower bound for the successful straight-line
    // prefix already emitted in this round. Unknown instructions and control
    // flow reset the proof; complete constant pushes rebuild it. This is
    // enough to preserve underflow behavior while still deleting identities
    // generated after locally visible producers.
    static bool hasProvenStackDepth(
        const std::vector<std::string>& emitted,
        size_t requiredDepth
    )
    {
        size_t depth = 0;
        for (const auto& instruction : emitted) {
            if (isInert(instruction) || isControlFlow(instruction)) {
                depth = 0;
                continue;
            }
            if (wellFormedPushSize(instruction).has_value()) {
                ++depth;
                continue;
            }

            // Retain a proof only for the small set of exact stack effects
            // needed by the identity rules. Everything else is a barrier.
            if (isOpDup(instruction)) {
                if (depth == 0) {
                    depth = 0;
                } else {
                    ++depth;
                }
            } else if (isOpDrop(instruction)) {
                if (depth > 0) {
                    --depth;
                }
            } else if (isOpSwap(instruction)) {
                if (depth < 2) {
                    depth = 0;
                }
            } else {
                depth = 0;
            }
        }
        return depth >= requiredDepth;
    }

    static TripleRewrite makeTripleRewrite(
        std::vector<ReplacementInstruction> replacements
    )
    {
        TripleRewrite rewrite;
        rewrite.matched = true;
        rewrite.replacements = std::move(replacements);
        return rewrite;
    }

    static TripleRewrite keepFirstTriple(const std::string& first)
    {
        return makeTripleRewrite({ReplacementInstruction{first, 0}});
    }

    static TripleRewrite keepFirstThenOpTriple(
        const std::string& first,
        tbc::BytOpcode terminal
    )
    {
        return makeTripleRewrite(
            {ReplacementInstruction{first, 0},
             ReplacementInstruction{opHex(terminal), 2}}
        );
    }

    static bool tripleRewriteIsStrictlySmaller(
        const std::string& first,
        const std::string& second,
        const std::string& third,
        const TripleRewrite& rewrite
    )
    {
        if (!rewrite.matched || !isPureHex(first) || !isPureHex(second) ||
            !isPureHex(third)) {
            return false;
        }
        size_t replacementBytes = 0;
        for (const auto& replacement : rewrite.replacements) {
            if (!isPureHex(replacement.encoding)) {
                return false;
            }
            replacementBytes += replacement.encoding.size() / 2;
        }
        const size_t originalBytes =
            (first.size() + second.size() + third.size()) / 2;
        return replacementBytes < originalBytes;
    }

    // Three-instruction rewrites run before all pair rules.  Apart from being
    // more profitable, this ordering is required for constant equality:
    // rewriting C;C to C;DUP first would hide the exact constant result.
    static TripleRewrite tryRewriteTriple(
        const std::string& first,
        const std::string& second,
        const std::string& third
    )
    {
        TripleRewrite rewrite;

        const auto firstPayload = decodedPushPayload(first);
        const auto secondPayload = decodedPushPayload(second);
        if (firstPayload && secondPayload && isOpEqual(third)) {
            rewrite = makeTripleRewrite({ReplacementInstruction{
                opHex(*firstPayload == *secondPayload
                          ? tbc::BytOpcode::OP_1
                          : tbc::BytOpcode::OP_0),
                2
            }});
        } else if (firstPayload && isOpDup(second) && isOpEqual(third)) {
            rewrite = makeTripleRewrite({ReplacementInstruction{
                opHex(tbc::BytOpcode::OP_1), 2
            }});
        } else if (firstPayload && isOpDup(second) && isOpDrop(third)) {
            rewrite = keepFirstTriple(first);
        } else if (firstPayload && isOpDup(second) &&
                   isOp(third, tbc::BytOpcode::OP_2DROP)) {
            rewrite = makeTripleRewrite({});
        } else if (producesCanonicalBool(first) && isOpNot(second) &&
                   isOpNot(third)) {
            rewrite = keepFirstTriple(first);
        } else if (producesCanonicalBool(first) &&
                   isOp(second, tbc::BytOpcode::OP_1) &&
                   (isOpEqual(third) || isOpNumEqual(third))) {
            rewrite = keepFirstTriple(first);
        } else if (producesCanonicalBool(first) && isOpZero(second) &&
                   (isOpEqual(third) || isOpNumEqual(third))) {
            rewrite = keepFirstThenOpTriple(first, tbc::BytOpcode::OP_NOT);
        } else if (producesCanonicalBool(first) && isOpZero(second) &&
                   isOpNumNotEqual(third)) {
            rewrite = keepFirstTriple(first);
        } else if (producesCanonicalBool(first) &&
                   isOp(second, tbc::BytOpcode::OP_1) &&
                   isOpNumNotEqual(third)) {
            rewrite = keepFirstThenOpTriple(first, tbc::BytOpcode::OP_NOT);
        }

        if (!tripleRewriteIsStrictlySmaller(
                first, second, third, rewrite
            )) {
            return {};
        }
        return rewrite;
    }

    static void applyTripleRewrite(
        std::vector<std::string>& out,
        std::vector<size_t>* oldToNew,
        size_t oldPC,
        const TripleRewrite& rewrite
    )
    {
        for (const auto& replacement : rewrite.replacements) {
            if (oldToNew) {
                (*oldToNew)[oldPC + replacement.originOffset] = out.size();
            }
            out.push_back(replacement.encoding);
        }
    }

    // 返回 producer+VERIFY 对应的融合操作；不在共识 opcode 中的 producer
    // 不做变换。
    static std::optional<tbc::BytOpcode>
    fusedVerifyOpcode(const std::string& producer)
    {
        if (isOp(producer, tbc::BytOpcode::OP_EQUAL)) {
            return tbc::BytOpcode::OP_EQUALVERIFY;
        }
        if (isOp(producer, tbc::BytOpcode::OP_NUMEQUAL)) {
            return tbc::BytOpcode::OP_NUMEQUALVERIFY;
        }
        if (isOp(producer, tbc::BytOpcode::OP_CHECKSIG)) {
            return tbc::BytOpcode::OP_CHECKSIGVERIFY;
        }
        if (isOp(producer, tbc::BytOpcode::OP_CHECKMULTISIG)) {
            return tbc::BytOpcode::OP_CHECKMULTISIGVERIFY;
        }
        return std::nullopt;
    }

    // 所有局部窗口规则集中在此处。以控制流指令结束的专用融合必须先于
    // 通用控制流屏障判断；其余规则仍不得跨越控制流边界。
    static PairRewrite tryRewritePair(
        const std::string& cur,
        const std::string& nxt,
        const std::vector<std::string>& emitted,
        bool allowTrueVerifyElision
    )
    {
        // 条件反相：仅接受可证明由前一 opcode 产生的规范布尔值。
        if (isOpNot(cur) &&
            (isOp(nxt, tbc::BytOpcode::OP_IF) ||
             isOp(nxt, tbc::BytOpcode::OP_NOTIF)) &&
            !emitted.empty() && producesCanonicalBool(emitted.back())) {
            return replacePair(
                isOp(nxt, tbc::BytOpcode::OP_IF)
                    ? tbc::BytOpcode::OP_NOTIF
                    : tbc::BytOpcode::OP_IF
            );
        }

        // 共识定义的 producer+VERIFY 融合。
        if (isOp(nxt, tbc::BytOpcode::OP_VERIFY)) {
            if (allowTrueVerifyElision &&
                isOp(cur, tbc::BytOpcode::OP_1)) {
                return deletePair();
            }
            if (auto fused = fusedVerifyOpcode(cur)) {
                return replacePair(*fused);
            }
        }

        if (isControlFlow(nxt)) {
            return {};
        }

        // Numeric specializations preserve ScriptNum validation and
        // underflow behavior while saving one byte.  These commonly appear
        // when a statically expanded loop supplies +1/-1 as a fixed value.
        if (isOp(cur, tbc::BytOpcode::OP_1) &&
            isOp(nxt, tbc::BytOpcode::OP_ADD)) {
            return replacePair(tbc::BytOpcode::OP_1ADD);
        }
        if (isOp(cur, tbc::BytOpcode::OP_1) &&
            isOp(nxt, tbc::BytOpcode::OP_SUB)) {
            return replacePair(tbc::BytOpcode::OP_1SUB);
        }
        if (isOp(cur, tbc::BytOpcode::OP_1NEGATE) &&
            isOp(nxt, tbc::BytOpcode::OP_ADD)) {
            return replacePair(tbc::BytOpcode::OP_1SUB);
        }
        if (isOp(cur, tbc::BytOpcode::OP_1NEGATE) &&
            isOp(nxt, tbc::BytOpcode::OP_SUB)) {
            return replacePair(tbc::BytOpcode::OP_1ADD);
        }

        // 栈恒等/专用操作。这里只加入 underflow 行为也保持一致的规则；
        // OP_0 OP_ROLL 需要额外栈深证明，故不在局部 pass 中删除。
        if (isOpSwap(cur) && isOpDrop(nxt)) {
            return replacePair(tbc::BytOpcode::OP_NIP);
        }
        if (isOpOver(cur) && isOpOver(nxt)) {
            return replacePair(tbc::BytOpcode::OP_2DUP);
        }
        if (isOp(cur, tbc::BytOpcode::OP_0) &&
            isOp(nxt, tbc::BytOpcode::OP_PICK)) {
            return replacePair(tbc::BytOpcode::OP_DUP);
        }
        if (isOp(cur, tbc::BytOpcode::OP_1) &&
            isOp(nxt, tbc::BytOpcode::OP_PICK)) {
            return replacePair(tbc::BytOpcode::OP_OVER);
        }
        if (isOp(cur, tbc::BytOpcode::OP_1) &&
            isOp(nxt, tbc::BytOpcode::OP_ROLL)) {
            return replacePair(tbc::BytOpcode::OP_SWAP);
        }
        if (isOp(cur, tbc::BytOpcode::OP_2) &&
            isOp(nxt, tbc::BytOpcode::OP_ROLL)) {
            return replacePair(tbc::BytOpcode::OP_ROT);
        }

        // 既有规则。
        if (wellFormedPushSize(cur).has_value() && isOpDrop(nxt)) {
            return deletePair();
        }
        // DUP;DROP and SWAP;SWAP are identities only after their respective
        // minimum input depths (1 and 2) have been proved. Naked pairs remain
        // unchanged, while a straight-line prefix of complete pushes can
        // establish the required depth without assuming an entry stack.
        if (isOpDup(cur) && isOpDrop(nxt) &&
            hasProvenStackDepth(emitted, 1)) {
            return deletePair();
        }
        if (isOpSwap(cur) && isOpSwap(nxt) &&
            hasProvenStackDepth(emitted, 2)) {
            return deletePair();
        }
        if (isOpNot(cur) && isOpNot(nxt) && !emitted.empty() &&
            producesCanonicalBool(emitted.back())) {
            return replacePair(tbc::BytOpcode::OP_0NOTEQUAL);
        }
        if (isOpDrop(cur) && isOpDrop(nxt)) {
            return replacePair(tbc::BytOpcode::OP_2DROP);
        }
        if (isOpZero(cur) && isOpEqual(nxt) && !emitted.empty() &&
            producesCanonicalBool(emitted.back())) {
            return replacePair(tbc::BytOpcode::OP_NOT);
        }
        if (isOpZero(cur) && isOpNumEqual(nxt) && !emitted.empty() &&
            producesCanonicalBool(emitted.back())) {
            return replacePair(tbc::BytOpcode::OP_NOT);
        }

        return {};
    }

    // 一轮扫描, 返回是否发生替换.
    static bool runOnePass(
        std::vector<std::string>& v,
        std::vector<size_t>* oldToNew,
        bool allowTrueVerifyElision,
        const LocalRewriteOptions& options,
        const tbc::BytecodeArtifact* artifact = nullptr
    )
    {
        if (oldToNew) {
            oldToNew->assign(v.size(), std::numeric_limits<size_t>::max());
        }

        std::vector<std::string> out;
        out.reserve(v.size());
        bool changed = false;
        const size_t executableEnd = optimizablePrefixLength(v, artifact);
        size_t i = 0;
        while (i < executableEnd) {
            const std::string& cur = v[i];

            // Prefer a profitable three-instruction rewrite over every pair
            // rule at the same PC.  All triple rules require complete hex
            // atoms and stay within one straight-line region.
            if (i + 2 < executableEnd && !isInert(cur) &&
                !isInert(v[i + 1]) && !isInert(v[i + 2]) &&
                !isControlFlow(cur) && !isControlFlow(v[i + 1]) &&
                !isControlFlow(v[i + 2])) {
                const TripleRewrite triple =
                    tryRewriteTriple(cur, v[i + 1], v[i + 2]);
                if (triple.matched) {
                    applyTripleRewrite(out, oldToNew, i, triple);
                    i += 3;
                    changed = true;
                    continue;
                }
            }

            // Keep the first push and replace only the identical second push
            // with DUP.  This is a one-to-one provenance rewrite, unlike the
            // pair fusions below.
            if (i + 1 < executableEnd &&
                isProfitableDuplicatePush(cur, v[i + 1], options)) {
                keepInstruction(v, out, oldToNew, i);
                if (oldToNew) {
                    (*oldToNew)[i + 1] = out.size();
                }
                out.push_back(opHex(tbc::BytOpcode::OP_DUP));
                i += 2;
                changed = true;
                continue;
            }

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

            if (i + 1 < executableEnd) {
                const std::string& nxt = v[i + 1];
                if (!isInert(nxt)) {
                    const PairRewrite rewrite =
                        tryRewritePair(
                            cur,
                            nxt,
                            out,
                            allowTrueVerifyElision
                        );
                    if (rewrite.kind == RewriteKind::DeletePair) {
                        i += 2;
                        changed = true;
                        continue;
                    }
                    if (rewrite.kind == RewriteKind::ReplacePair) {
                        replacePairWithOne(
                            out, oldToNew, i, rewrite.replacement
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
        while (i < v.size()) {
            keepInstruction(v, out, oldToNew, i);
            ++i;
        }
        if (changed) {
            v = std::move(out);
        }
        return changed;
    }

    // 仅统计纯 hex 指令的字节数.
    static size_t countBytes(
        const std::vector<std::string>& v,
        size_t begin = 0,
        size_t end = std::numeric_limits<size_t>::max()
    )
    {
        size_t n = 0;
        end = std::min(end, v.size());
        for (size_t i = std::min(begin, end); i < end; ++i) {
            const auto& s = v[i];
            if (!s.empty() && s[0] != '#' && isPureHex(s)) {
                n += s.size() / 2;
            }
        }
        return n;
    }
};

#endif // BYTECODE_PEEPHOLE_PASS_H
