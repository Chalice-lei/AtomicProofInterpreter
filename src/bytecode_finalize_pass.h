#ifndef BYTECODE_FINALIZE_PASS_H
#define BYTECODE_FINALIZE_PASS_H

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bytecode/bytecode_opcodes.h"
#include "bytecode/bytecode_artifact_rewriter.h"
#include "bytecode/legacy_bytecode_adapter.h"
#include "error/error_manager.h"
#include "include/pass_type.h"
#include "log/logger.h"
#include "pass/pass.h"
#include "pass/pass_context.h"
#include "pass/pass_context_keys.h"
#include "pass/pass_macros.h"

#ifdef ENABLE_DEBUGGER
#include "debugger/info/debug_info_save.h"
#endif

// Final bytecode layout pass.
//
// The compiler may append immutable suffix/state data after OP_RETURN. Padding
// must therefore be inserted between OP_RETURN and that suffix, after peephole
// optimization has settled the executable code length.
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
        auto artifact = data.tryGet<tbc::BytecodeArtifact>(
            apc_pipeline::key::kBytecodeArtifact
        );

        // CanonicalV2 cannot know executable size until concrete placeholder
        // payloads are available.  Record the layout directive and leave both
        // template and legacy compatibility view untouched; ScriptMaterializer
        // performs canonicalization before it computes padding.
        if (artifact && artifact->format == tbc::ArtifactFormat::CanonicalV2) {
            artifact->layout.executableAlignment = 64;
            // Even a fully concrete CanonicalV2 template still needs the
            // canonical encoder and deferred alignment phase.  This flag is
            // cleared only by a successful materialization boundary, never by
            // the compile-time layout pass.
            artifact->layout.requiresMaterialization = true;
#ifdef ENABLE_DEBUGGER
            auto debugInfo = data.tryGet<apc_debug::DebugInfo>(
                apc_pipeline::key::kDebugInfo
            );
            apc_debug::saveDebugInfoIfRequested(data, debugInfo);
#endif
            LOG_INFO(
                "BytecodeFinalizePass: deferred CanonicalV2 padding until "
                "placeholder materialization"
            );
            return;
        }
        auto selfPlaceholderLengths =
            data.tryGet<std::unordered_map<std::string, size_t>>(
                apc_pipeline::key::kSelfPlaceholderLengths
            );
        const std::unordered_map<std::string, size_t> emptyLengths;
        const auto& placeholderLengths =
            selfPlaceholderLengths ? *selfPlaceholderLengths : emptyLengths;

#ifdef ENABLE_DEBUGGER
        auto debugInfo = data.tryGet<apc_debug::DebugInfo>(
            apc_pipeline::key::kDebugInfo
        );
#endif

        if (artifact && artifact->lockingScript.size() != instrs.size()) {
            LOG_WARNING(
                "BytecodeFinalizePass: typed artifact and legacy view have "
                "different instruction counts; leaving all representations "
                "unchanged"
            );
            return;
        }

        // Placeholder spelling normalization is part of the candidate.  Keep
        // it isolated until the typed artifact and DebugInfo remaps have both
        // validated, otherwise an early failure would partially mutate only
        // the legacy representation.
        std::vector<std::string> workingInstrs = instrs;

        std::optional<size_t> returnIndexOpt;
        size_t oldPaddingEnd = 0;
        if (artifact) {
            bool sawPadding = false;
            bool sawSuffix = false;
            std::optional<size_t> finalExecutableIndex;
            bool validRegionOrder = true;
            for (size_t i = 0; i < artifact->lockingScript.size(); ++i) {
                const auto region = artifact->lockingScript[i].region;
                if (region == tbc::ScriptRegion::Executable) {
                    if (sawPadding || sawSuffix) {
                        validRegionOrder = false;
                        break;
                    }
                    finalExecutableIndex = i;
                } else if (region == tbc::ScriptRegion::Padding) {
                    if (sawSuffix) {
                        validRegionOrder = false;
                        break;
                    }
                    sawPadding = true;
                } else {
                    sawSuffix = true;
                }
            }

            if (!validRegionOrder) {
                LOG_WARNING(
                    "BytecodeFinalizePass: invalid typed script region "
                    "order; leaving all representations unchanged"
                );
                return;
            }

            if (finalExecutableIndex.has_value()) {
                const auto* opcode =
                    std::get_if<tbc::OpcodeInstruction>(
                        &artifact->lockingScript[*finalExecutableIndex].body
                    );
                if (opcode && opcode->opcode == tbc::BytOpcode::OP_RETURN) {
                    returnIndexOpt = *finalExecutableIndex;
                }
            }
        } else {
            const auto returnIt = std::find_if(
                workingInstrs.rbegin(),
                workingInstrs.rend(),
                [](const std::string& instr) {
                    return isSingleOpcode(instr, tbc::BytOpcode::OP_RETURN);
                }
            );
            if (returnIt != workingInstrs.rend()) {
                returnIndexOpt = static_cast<size_t>(
                    std::distance(returnIt, workingInstrs.rend()) - 1
                );
            }
        }

        if (!returnIndexOpt.has_value()) {
            LOG_DEBUG(
                "BytecodeFinalizePass: no OP_RETURN found, skipping padding"
            );
#ifdef ENABLE_DEBUGGER
            apc_debug::saveDebugInfoIfRequested(data, debugInfo);
#endif
            return;
        }

        const size_t returnIndex = *returnIndexOpt;
        oldPaddingEnd = returnIndex + 1;
        if (artifact) {
            while (oldPaddingEnd < artifact->lockingScript.size() &&
                   artifact->lockingScript[oldPaddingEnd].region ==
                       tbc::ScriptRegion::Padding) {
                ++oldPaddingEnd;
            }
        }

        bool hadSelfPlaceholderError = false;
        const auto codeBytesOpt = countBytes(
            workingInstrs,
            0,
            returnIndex + 1,
            placeholderLengths,
            true,
            &hadSelfPlaceholderError
        );
        if (!codeBytesOpt.has_value()) {
            if (!hadSelfPlaceholderError) {
                LOG_WARNING(
                    "BytecodeFinalizePass: non-hex instruction before "
                    "OP_RETURN; cannot determine final padding"
                );
            }
#ifdef ENABLE_DEBUGGER
            apc_debug::saveDebugInfoIfRequested(data, debugInfo);
#endif
            return;
        }

        const size_t codeBytes = codeBytesOpt.value();
        const size_t paddingBytes = bytesToAlign(codeBytes, 64);
        const std::string paddingInstr = makePushPadding(paddingBytes);
        const size_t paddingInstrCount = paddingInstr.empty() ? 0 : 1;
        // Typed artifacts identify pre-existing padding explicitly. Replace
        // that region instead of guessing from bytes: immutable suffix data
        // may legitimately have the same encoding as padding (including 6a).
        // The legacy-only fallback cannot distinguish those cases, so it
        // preserves every atom after OP_RETURN as before.
        const size_t suffixStart = oldPaddingEnd;
        const size_t oldPaddingBytes = countBestEffortBytes(
            workingInstrs,
            returnIndex + 1,
            oldPaddingEnd,
            placeholderLengths
        );
        const size_t suffixBytes =
            countBestEffortBytes(
                workingInstrs,
                suffixStart,
                workingInstrs.size(),
                placeholderLengths
            );
        const size_t beforeBytes =
            countBestEffortBytes(
                workingInstrs,
                0,
                workingInstrs.size(),
                placeholderLengths
            );

        std::vector<size_t> pcRemap(
            workingInstrs.size(), std::numeric_limits<size_t>::max()
        );
        for (size_t i = 0; i <= returnIndex; ++i) {
            pcRemap[i] = i;
        }
        if (paddingInstrCount != 0) {
            for (size_t i = returnIndex + 1; i < oldPaddingEnd; ++i) {
                pcRemap[i] = returnIndex + 1;
            }
        }
        for (size_t i = suffixStart; i < workingInstrs.size(); ++i) {
            pcRemap[i] =
                returnIndex + 1 + paddingInstrCount + (i - suffixStart);
        }

        std::vector<std::string> finalized;
        finalized.reserve(
            returnIndex + 1 + paddingInstrCount +
            (workingInstrs.size() - suffixStart)
        );
        finalized.insert(
            finalized.end(),
            workingInstrs.begin(),
            workingInstrs.begin() + returnIndex + 1
        );
        if (!paddingInstr.empty()) {
            finalized.push_back(paddingInstr);
        }
        finalized.insert(finalized.end(),
                         workingInstrs.begin() + suffixStart,
                         workingInstrs.end());

        std::optional<tbc::BytecodeArtifact> finalizedArtifact;
        if (artifact) {
            const auto newToOld =
                tbc::BytecodeArtifactRewriter::reverseMapping(
                    pcRemap, finalized.size()
                );
            std::string artifactError;
            finalizedArtifact = tbc::BytecodeArtifactRewriter::rewrite(
                *artifact, finalized, newToOld, &artifactError
            );
            if (!finalizedArtifact.has_value()) {
                LOG_WARNING(
                    "BytecodeFinalizePass: typed artifact layout failed: ",
                    artifactError,
                    "; leaving all representations unchanged"
                );
                return;
            }
            if (paddingInstrCount != 0) {
                auto& padding =
                    finalizedArtifact->lockingScript[returnIndex + 1];
                padding.region = tbc::ScriptRegion::Padding;
                padding.body =
                    tbc::LegacyBytecodeAdapter::parseExecutableAtom(
                        paddingInstr
                    );
                padding.legacyEncoding = paddingInstr;
            }
        }

#ifdef ENABLE_DEBUGGER
        std::shared_ptr<apc_debug::DebugInfo> finalizedDebugInfo;
        if (debugInfo) {
            finalizedDebugInfo = debugInfo->remapped(
                pcRemap, finalized.size()
            );
            if (!finalizedDebugInfo) {
                LOG_WARNING(
                    "BytecodeFinalizePass: DebugInfo layout remap failed; "
                    "leaving all representations unchanged"
                );
                return;
            }
            finalizedDebugInfo->syncInstructionOpcodes(finalized);
        }
#endif

        instrs = std::move(finalized);
        if (artifact && finalizedArtifact.has_value()) {
            *artifact = std::move(*finalizedArtifact);
        }

#ifdef ENABLE_DEBUGGER
        if (debugInfo && finalizedDebugInfo) {
            *debugInfo = std::move(*finalizedDebugInfo);
        }
        apc_debug::saveDebugInfoIfRequested(data, debugInfo);
#endif

        const size_t afterBytes =
            countBestEffortBytes(instrs, 0, instrs.size(), placeholderLengths);
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
    using PlaceholderLengthMap = std::unordered_map<std::string, size_t>;

    static std::string normalizeHex(std::string value)
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

    static bool isPureHex(const std::string& value)
    {
        const std::string hex = normalizeHex(value);
        if (hex.empty() || (hex.size() % 2) != 0) {
            return false;
        }
        return std::all_of(hex.begin(), hex.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        });
    }

    static bool isIgnored(const std::string& value)
    {
        return value.empty() || value[0] == '#';
    }

    static bool isHexChar(char c)
    {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    }

    static bool startsWith(const std::string& value, const std::string& prefix)
    {
        return value.size() >= prefix.size() &&
               value.compare(0, prefix.size(), prefix) == 0;
    }

    static bool endsWith(const std::string& value, const std::string& suffix)
    {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(),
                             suffix) == 0;
    }

    static size_t pushEncodedByteLength(size_t payloadBytes)
    {
        if (payloadBytes < 76) {
            return 1 + payloadBytes;
        }
        if (payloadBytes <= 255) {
            return 2 + payloadBytes;
        }
        if (payloadBytes <= 65535) {
            return 3 + payloadBytes;
        }
        return 5 + payloadBytes;
    }

    static std::optional<size_t> selfPlaceholderPayloadLength(
        const std::string& label,
        const PlaceholderLengthMap& placeholderLengths,
        bool enforceSelfLength,
        bool* hadSelfPlaceholderError
    )
    {
        if (!startsWith(label, "self.")) {
            return std::nullopt;
        }

        auto it = placeholderLengths.find(label);
        if (it != placeholderLengths.end()) {
            return it->second;
        }

        // A normalized legacy label appends its fixed payload size directly
        // to the source field name. The field name may itself end in digits
        // (`self.foo2` + `20` -> `self.foo220`), so stripping the whole numeric
        // tail loses the declaration boundary. Match the longest declared
        // base whose exact decimal size suffix consumes the remainder.
        std::optional<size_t> matchedLength;
        size_t matchedBaseLength = 0;
        for (const auto& [base, payloadLength] : placeholderLengths) {
            const std::string suffix = std::to_string(payloadLength);
            if (label.size() != base.size() + suffix.size() ||
                label.compare(0, base.size(), base) != 0 ||
                label.compare(base.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            if (!matchedLength.has_value() ||
                base.size() > matchedBaseLength) {
                matchedLength = payloadLength;
                matchedBaseLength = base.size();
            }
        }
        if (matchedLength.has_value()) {
            return matchedLength;
        }

        if (enforceSelfLength) {
            if (hadSelfPlaceholderError) {
                *hadSelfPlaceholderError = true;
            }
            SEMANTIC_ERROR(
                "placeholder '<" + label +
                    ">' appears before OP_RETURN but has no fixed byte "
                    "length declaration",
                SourceLocation("", 0, 0),
                "Declare the constructor source parameter with a fixed-size "
                "type such as hex20, or move variable-length data after "
                "OP_RETURN"
            );
            LOG_ERROR(
                "BytecodeFinalizePass: unresolved fixed length for placeholder "
                "<",
                label,
                "> before OP_RETURN"
            );
        }

        return std::nullopt;
    }

    static bool isSingleOpcode(const std::string& value, tbc::BytOpcode opcode)
    {
        if (!isPureHex(value)) {
            return false;
        }
        return normalizeHex(value) == tbc::opcodeToHex(opcode);
    }

    static std::optional<size_t> instructionBytes(
        std::string& value,
        const PlaceholderLengthMap& placeholderLengths,
        bool rewriteSelfPlaceholders,
        bool enforceSelfLength,
        bool* hadSelfPlaceholderError
    )
    {
        if (isIgnored(value)) {
            return 0;
        }
        if (isPureHex(value)) {
            return normalizeHex(value).size() / 2;
        }

        std::string source = value;
        std::string rewritten;
        rewritten.reserve(value.size() + 8);

        if (source.size() >= 2 && source[0] == '0' &&
            (source[1] == 'x' || source[1] == 'X')) {
            rewritten += source.substr(0, 2);
            source = source.substr(2);
        }

        bool sawPlaceholder = false;
        size_t total = 0;
        size_t i = 0;
        while (i < source.size()) {
            if (source[i] == '<') {
                const size_t end = source.find('>', i + 1);
                if (end == std::string::npos) {
                    return std::nullopt;
                }

                sawPlaceholder = true;
                const std::string label =
                    source.substr(i + 1, end - i - 1);
                auto payloadLen = selfPlaceholderPayloadLength(
                    label,
                    placeholderLengths,
                    enforceSelfLength,
                    hadSelfPlaceholderError
                );
                if (!payloadLen.has_value()) {
                    return std::nullopt;
                }

                total += pushEncodedByteLength(payloadLen.value());

                std::string rewrittenLabel = label;
                const std::string lengthSuffix =
                    std::to_string(payloadLen.value());
                if (!endsWith(rewrittenLabel, lengthSuffix)) {
                    rewrittenLabel += lengthSuffix;
                }
                rewritten += "<" + rewrittenLabel + ">";
                i = end + 1;
                continue;
            }

            if (!isHexChar(source[i])) {
                return std::nullopt;
            }

            const size_t start = i;
            while (i < source.size() && isHexChar(source[i])) {
                ++i;
            }
            const size_t hexChars = i - start;
            if ((hexChars % 2) != 0) {
                return std::nullopt;
            }
            total += hexChars / 2;
            rewritten += source.substr(start, hexChars);
        }

        if (!sawPlaceholder) {
            return std::nullopt;
        }

        if (rewriteSelfPlaceholders) {
            value = std::move(rewritten);
        }

        return total;
    }

    static std::optional<size_t> countBytes(
        std::vector<std::string>& instrs,
        size_t start,
        size_t end,
        const PlaceholderLengthMap& placeholderLengths,
        bool rewriteSelfPlaceholders,
        bool* hadSelfPlaceholderError
    )
    {
        size_t total = 0;
        for (size_t i = start; i < end; ++i) {
            auto bytesOpt = instructionBytes(
                instrs[i],
                placeholderLengths,
                rewriteSelfPlaceholders,
                true,
                hadSelfPlaceholderError
            );
            if (!bytesOpt.has_value()) {
                return std::nullopt;
            }
            total += bytesOpt.value();
        }
        return total;
    }

    static size_t countBestEffortBytes(
        std::vector<std::string>& instrs,
        size_t start,
        size_t end,
        const PlaceholderLengthMap& placeholderLengths
    )
    {
        size_t total = 0;
        for (size_t i = start; i < end; ++i) {
            auto bytesOpt = instructionBytes(
                instrs[i],
                placeholderLengths,
                false,
                false,
                nullptr
            );
            if (bytesOpt.has_value()) {
                total += bytesOpt.value();
            }
        }
        return total;
    }

    static size_t bytesToAlign(size_t length, size_t alignment)
    {
        const size_t remainder = length % alignment;
        return remainder == 0 ? 0 : alignment - remainder;
    }

    static std::string byteToHex(size_t value)
    {
        static constexpr char kHexDigits[] = "0123456789abcdef";
        return std::string{
            kHexDigits[(value >> 4) & 0x0f],
            kHexDigits[value & 0x0f]
        };
    }

    static std::string makePushPadding(size_t paddingBytes)
    {
        if (paddingBytes == 0) {
            return "";
        }
        if (paddingBytes == 1) {
            return "00";
        }

        const size_t dataBytes = paddingBytes - 1;
        if (dataBytes > 75) {
            return "";
        }

        std::string result = byteToHex(dataBytes);
        result.reserve(paddingBytes * 2);
        for (size_t i = 0; i < dataBytes; ++i) {
            result += "ff";
        }
        return result;
    }

};

#endif // BYTECODE_FINALIZE_PASS_H
