#ifndef LEGACY_BYTECODE_ADAPTER_H
#define LEGACY_BYTECODE_ADAPTER_H

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "bytecode_ir.h"
#include "script_codec.h"

SPACE_TBC_START

using LegacyBytecode = std::pair<
    std::vector<std::string>,
    std::unordered_map<std::string, std::string>>;

struct LegacyFragmentParseResult
{
    std::vector<BytecodeInstruction> instructions;
    ScriptCodecError error{ScriptCodecError::None};
    size_t errorByteOffset{0};

    [[nodiscard]] bool ok() const noexcept
    {
        return error == ScriptCodecError::None;
    }
};

// Lossless migration boundary between the current vector<string> pipeline and
// the typed IR. Malformed or compound legacy atoms become explicit barriers;
// the importer never guesses instruction boundaries.
class LegacyBytecodeAdapter final
{
public:
    [[nodiscard]] static BytecodeArtifact import(const LegacyBytecode& legacy)
    {
        BytecodeArtifact artifact;
        artifact.format = ArtifactFormat::LegacyV1;
        artifact.unlockingScripts = legacy.second;
        artifact.lockingScript.reserve(legacy.first.size());

        const std::optional<size_t> suffixBoundary =
            findImmutableSuffixBoundary(legacy.first);

        for (size_t index = 0; index < legacy.first.size(); ++index) {
            const std::string& atom = legacy.first[index];
            BytecodeInstruction instruction;
            instruction.id = static_cast<InstructionId>(index);
            instruction.origins.push_back(static_cast<OriginId>(index));
            instruction.legacyEncoding = atom;

            if (suffixBoundary.has_value() && index >= *suffixBoundary) {
                instruction.region = ScriptRegion::ImmutableSuffix;
                // Keep relocation semantics for a whole deployment
                // placeholder even though suffix bytes are otherwise opaque.
                instruction.body = isPlaceholder(atom)
                                       ? parseExecutableAtom(atom)
                                       : InstructionBody{
                                             RawSuffixInstruction{atom}};
            } else {
                instruction.region = ScriptRegion::Executable;
                instruction.body = parseExecutableAtom(atom);
            }
            artifact.lockingScript.push_back(std::move(instruction));
        }
        return artifact;
    }

    [[nodiscard]] static LegacyBytecode
    exportPreserving(const BytecodeArtifact& artifact)
    {
        LegacyBytecode legacy;
        legacy.second = artifact.unlockingScripts;
        legacy.first.reserve(artifact.lockingScript.size());
        for (const BytecodeInstruction& instruction : artifact.lockingScript) {
            legacy.first.push_back(
                serializeInstructionPreserving(instruction)
            );
        }
        return legacy;
    }

    [[nodiscard]] static InstructionBody
    parseExecutableAtom(const std::string& atom)
    {
        if (isPlaceholder(atom)) {
            return PlaceholderPushInstruction{
                atom.substr(1, atom.size() - 2), std::nullopt};
        }

        auto decoded = ScriptCodec::decodePushHex(atom, false);
        if (decoded.ok()) {
            return PushDataInstruction{std::move(decoded.value->payload)};
        }

        ScriptCodecError hexError = ScriptCodecError::None;
        auto bytes = ScriptCodec::hexToBytes(atom, &hexError);
        if (bytes.has_value() && bytes->size() == 1 &&
            decoded.error == ScriptCodecError::NotPush) {
            return OpcodeInstruction{static_cast<BytOpcode>(bytes->front())};
        }

        return LegacyBarrierInstruction{atom};
    }

    [[nodiscard]] static std::string serializeInstructionPreserving(
        const BytecodeInstruction& instruction
    )
    {
        if (instruction.legacyEncoding.has_value()) {
            return *instruction.legacyEncoding;
        }
        return synthesizeLegacy(instruction.body);
    }

    // Split a pure-hex compatibility fragment into typed instruction atoms.
    // The operation is transactional: malformed length/payload data returns
    // no partial instructions. A whole placeholder is one atom; placeholders
    // mixed with hex are rejected rather than assigned guessed boundaries.
    [[nodiscard]] static LegacyFragmentParseResult splitScriptFragment(
        std::string_view fragment,
        InstructionId firstId = 0,
        ScriptRegion region = ScriptRegion::Executable
    )
    {
        LegacyFragmentParseResult result;
        if (isPlaceholder(fragment)) {
            BytecodeInstruction instruction;
            instruction.id = firstId;
            instruction.body = PlaceholderPushInstruction{
                std::string(fragment.substr(1, fragment.size() - 2)),
                std::nullopt};
            instruction.region = region;
            instruction.origins.push_back(firstId);
            instruction.legacyEncoding = std::string(fragment);
            result.instructions.push_back(std::move(instruction));
            return result;
        }

        std::string_view rawHex = fragment;
        if (rawHex.size() >= 2 && rawHex[0] == '0' &&
            (rawHex[1] == 'x' || rawHex[1] == 'X')) {
            rawHex.remove_prefix(2);
        }

        ScriptCodecError parseError = ScriptCodecError::None;
        auto bytes = ScriptCodec::hexToBytes(fragment, &parseError);
        if (!bytes.has_value()) {
            result.error = parseError;
            return result;
        }

        size_t byteOffset = 0;
        while (byteOffset < bytes->size()) {
            const std::span<const uint8_t> remaining(
                bytes->data() + byteOffset, bytes->size() - byteOffset
            );
            auto decoded = ScriptCodec::decodePushPrefix(remaining, false);

            BytecodeInstruction instruction;
            instruction.id = firstId + result.instructions.size();
            instruction.region = region;
            instruction.origins.push_back(instruction.id);

            size_t consumed = 0;
            if (decoded.ok()) {
                consumed = decoded.value->encodedSize;
                instruction.body = PushDataInstruction{
                    std::move(decoded.value->payload)};
            } else if (decoded.error == ScriptCodecError::NotPush) {
                consumed = 1;
                instruction.body = OpcodeInstruction{
                    static_cast<BytOpcode>((*bytes)[byteOffset])};
            } else {
                result.instructions.clear();
                result.error = decoded.error;
                result.errorByteOffset = byteOffset;
                return result;
            }

            instruction.legacyEncoding = std::string(
                rawHex.substr(byteOffset * 2, consumed * 2)
            );
            result.instructions.push_back(std::move(instruction));
            byteOffset += consumed;
        }
        return result;
    }

private:
    [[nodiscard]] static bool isPlaceholder(std::string_view atom)
    {
        return atom.size() > 2 && atom.front() == '<' && atom.back() == '>' &&
               atom.find('<', 1) == std::string_view::npos &&
               atom.find('>') == atom.size() - 1;
    }

    [[nodiscard]] static std::optional<size_t>
    findImmutableSuffixBoundary(const std::vector<std::string>& atoms)
    {
        for (size_t index = atoms.size(); index > 0; --index) {
            ScriptCodecError error = ScriptCodecError::None;
            auto bytes = ScriptCodec::hexToBytes(atoms[index - 1], &error);
            if (bytes.has_value() && bytes->size() == 1 &&
                bytes->front() == static_cast<uint8_t>(BytOpcode::OP_RETURN)) {
                return index;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::string
    synthesizeLegacy(const InstructionBody& body)
    {
        return std::visit(
            [](const auto& value) -> std::string {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, OpcodeInstruction>) {
                    const uint16_t raw = static_cast<uint16_t>(value.opcode);
                    if (raw > std::numeric_limits<uint8_t>::max()) {
                        return {};
                    }
                    const std::vector<uint8_t> bytes{
                        static_cast<uint8_t>(raw)};
                    return ScriptCodec::bytesToHex(bytes);
                } else if constexpr (std::is_same_v<T, PushDataInstruction>) {
                    auto encoded = ScriptCodec::encodePush(
                        value.payload, PushEncodingPolicy::Legacy
                    );
                    return encoded.value_or(std::string{});
                } else if constexpr (
                    std::is_same_v<T, PlaceholderPushInstruction>) {
                    return "<" + value.label + ">";
                } else {
                    return value.raw;
                }
            },
            body
        );
    }
};

SPACE_TBC_END

#endif // LEGACY_BYTECODE_ADAPTER_H
