#include "bytecode_materializer.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <span>
#include <unordered_set>
#include <utility>

#include "script_codec.h"

SPACE_TBC_START

namespace
{

struct EncodedRegions
{
    std::vector<MaterializedAtom> executable;
    std::vector<MaterializedAtom> suffix;
    size_t executableBytes{0};
    size_t suffixBytes{0};
    std::optional<size_t> finalReturnInstructionIndex;
};

[[nodiscard]] bool checkedAdd(size_t left, size_t right, size_t& result)
{
    if (right > std::numeric_limits<size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] MaterializationResult fail(
    MaterializationErrorCode code,
    std::string message,
    const BytecodeInstruction* instruction = nullptr,
    std::optional<size_t> instructionIndex = std::nullopt,
    std::string label = {}
)
{
    MaterializationResult result;
    result.error.code = code;
    result.error.message = std::move(message);
    result.error.instructionIndex = instructionIndex;
    result.error.label = std::move(label);
    if (instruction) {
        result.error.instructionId = instruction->id;
    }
    return result;
}

[[nodiscard]] std::optional<std::string> encodeOpcode(BytOpcode opcode)
{
    const uint16_t value = static_cast<uint16_t>(opcode);
    if (value > std::numeric_limits<uint8_t>::max() ||
        opcode == BytOpcode::OP_INVALIDOPCODE) {
        return std::nullopt;
    }

    // 0x01..0x4e are push prefixes and are incomplete without their length
    // and payload. Typed pushes must use PushDataInstruction instead.
    if (value >= 0x01 && value <= 0x4e) {
        return std::nullopt;
    }

    const uint8_t byte = static_cast<uint8_t>(value);
    return ScriptCodec::bytesToHex(std::span<const uint8_t>(&byte, 1));
}

[[nodiscard]] MaterializationResult encodeInstruction(
    const BytecodeInstruction& instruction,
    size_t instructionIndex,
    const PlaceholderBindings& bindings,
    MaterializedAtom& output
)
{
    output.sourceInstructionId = instruction.id;
    output.sourceInstructionIndex = instructionIndex;
    output.region = instruction.region;

    if (const auto* opcode =
            std::get_if<OpcodeInstruction>(&instruction.body)) {
        auto encoded = encodeOpcode(opcode->opcode);
        if (!encoded.has_value()) {
            return fail(
                MaterializationErrorCode::InvalidOpcode,
                "opcode is not a complete canonical instruction",
                &instruction,
                instructionIndex
            );
        }
        output.encodedHex = std::move(encoded.value());
        return MaterializationResult{};
    }

    if (const auto* push =
            std::get_if<PushDataInstruction>(&instruction.body)) {
        auto encoded = ScriptCodec::encodePush(
            push->payload, PushEncodingPolicy::Canonical
        );
        if (!encoded.has_value()) {
            return fail(
                MaterializationErrorCode::PayloadTooLarge,
                "push payload exceeds ScriptCodec limits",
                &instruction,
                instructionIndex
            );
        }
        output.encodedHex = std::move(encoded.value());
        return MaterializationResult{};
    }

    if (const auto* placeholder =
            std::get_if<PlaceholderPushInstruction>(&instruction.body)) {
        if (placeholder->label.empty()) {
            return fail(
                MaterializationErrorCode::InvalidPlaceholderLabel,
                "placeholder label must not be empty",
                &instruction,
                instructionIndex
            );
        }

        const auto binding = bindings.find(placeholder->label);
        if (binding == bindings.end()) {
            return fail(
                MaterializationErrorCode::MissingBinding,
                "missing payload binding for placeholder '" +
                    placeholder->label + "'",
                &instruction,
                instructionIndex,
                placeholder->label
            );
        }
        if (placeholder->expectedPayloadSize.has_value() &&
            binding->second.size() !=
                *placeholder->expectedPayloadSize) {
            return fail(
                MaterializationErrorCode::PayloadSizeMismatch,
                "placeholder '" + placeholder->label + "' expects " +
                    std::to_string(*placeholder->expectedPayloadSize) +
                    " payload bytes but received " +
                    std::to_string(binding->second.size()),
                &instruction,
                instructionIndex,
                placeholder->label
            );
        }

        auto encoded = ScriptCodec::encodePush(
            binding->second, PushEncodingPolicy::Canonical
        );
        if (!encoded.has_value()) {
            return fail(
                MaterializationErrorCode::PayloadTooLarge,
                "placeholder payload exceeds ScriptCodec limits",
                &instruction,
                instructionIndex,
                placeholder->label
            );
        }
        output.encodedHex = std::move(encoded.value());
        return MaterializationResult{};
    }

    if (const auto* raw =
            std::get_if<RawSuffixInstruction>(&instruction.body)) {
        if (instruction.region != ScriptRegion::ImmutableSuffix) {
            return fail(
                MaterializationErrorCode::RawSuffixOutsideImmutableSuffix,
                "raw suffix bytes are valid only in ImmutableSuffix",
                &instruction,
                instructionIndex
            );
        }

        ScriptCodecError codecError = ScriptCodecError::None;
        auto bytes = ScriptCodec::hexToBytes(raw->raw, &codecError);
        if (!bytes.has_value()) {
            return fail(
                MaterializationErrorCode::InvalidRawSuffix,
                std::string("invalid raw suffix: ") +
                    ScriptCodec::errorName(codecError),
                &instruction,
                instructionIndex
            );
        }
        output.encodedHex = ScriptCodec::bytesToHex(bytes.value());
        return MaterializationResult{};
    }

    return fail(
        MaterializationErrorCode::LegacyBarrier,
        "LegacyBarrier cannot be emitted as CanonicalV2",
        &instruction,
        instructionIndex
    );
}

[[nodiscard]] bool isReturn(const BytecodeInstruction& instruction)
{
    const auto* opcode =
        std::get_if<OpcodeInstruction>(&instruction.body);
    return opcode && opcode->opcode == BytOpcode::OP_RETURN;
}

[[nodiscard]] MaterializationResult materializeRegions(
    const BytecodeArtifact& artifact,
    const PlaceholderBindings& bindings,
    EncodedRegions& regions
)
{
    bool sawSuffix = false;
    std::optional<size_t> finalExecutableInstructionIndex;

    // Executable materialization is intentionally completed before suffix
    // materialization. Padding is derived solely from the concrete executable
    // bytes; suffix payload sizes can never affect it.
    for (size_t index = 0; index < artifact.lockingScript.size(); ++index) {
        const BytecodeInstruction& instruction =
            artifact.lockingScript[index];
        if (instruction.region == ScriptRegion::Padding) {
            return fail(
                MaterializationErrorCode::PreexistingPadding,
                "CanonicalV2 templates must not contain precomputed padding",
                &instruction,
                index
            );
        }
        if (instruction.region == ScriptRegion::ImmutableSuffix) {
            sawSuffix = true;
            continue;
        }
        if (sawSuffix) {
            return fail(
                MaterializationErrorCode::InvalidRegionOrder,
                "Executable instruction appears after ImmutableSuffix",
                &instruction,
                index
            );
        }
        finalExecutableInstructionIndex = index;

        MaterializedAtom atom;
        auto encoded =
            encodeInstruction(instruction, index, bindings, atom);
        if (encoded.error.code != MaterializationErrorCode::None) {
            return encoded;
        }

        size_t updated = 0;
        const size_t atomBytes = atom.encodedHex.size() / 2;
        if (!checkedAdd(regions.executableBytes, atomBytes, updated)) {
            return fail(
                MaterializationErrorCode::ScriptSizeOverflow,
                "executable script byte count overflowed",
                &instruction,
                index
            );
        }
        regions.executableBytes = updated;
        regions.executable.push_back(std::move(atom));
        if (isReturn(instruction)) {
            regions.finalReturnInstructionIndex = index;
        }
    }

    if (artifact.layout.executableAlignment.has_value()) {
        const size_t alignment = *artifact.layout.executableAlignment;
        if (alignment == 0) {
            return fail(
                MaterializationErrorCode::InvalidAlignment,
                "executable alignment must be greater than zero"
            );
        }
        if (!regions.finalReturnInstructionIndex.has_value()) {
            return fail(
                MaterializationErrorCode::MissingOpReturn,
                "executable alignment requires an OP_RETURN"
            );
        }

        if (!finalExecutableInstructionIndex.has_value() ||
            *regions.finalReturnInstructionIndex !=
                *finalExecutableInstructionIndex) {
            const size_t offendingIndex =
                *regions.finalReturnInstructionIndex + 1;
            return fail(
                MaterializationErrorCode::ExecutableAfterFinalReturn,
                "executable bytes after the final OP_RETURN must be marked "
                "ImmutableSuffix",
                &artifact.lockingScript[offendingIndex],
                offendingIndex
            );
        }
    } else if (sawSuffix) {
        // Immutable suffix bytes have an unambiguous boundary only when the
        // final typed executable atom is OP_RETURN. This invariant is
        // independent of whether executable alignment was requested.
        if (!regions.finalReturnInstructionIndex.has_value()) {
            return fail(
                MaterializationErrorCode::MissingOpReturn,
                "ImmutableSuffix requires a preceding OP_RETURN"
            );
        }
        if (!finalExecutableInstructionIndex.has_value() ||
            *regions.finalReturnInstructionIndex !=
                *finalExecutableInstructionIndex) {
            const size_t offendingIndex =
                *regions.finalReturnInstructionIndex + 1;
            return fail(
                MaterializationErrorCode::ExecutableAfterFinalReturn,
                "the final executable atom before ImmutableSuffix must be "
                "OP_RETURN",
                &artifact.lockingScript[offendingIndex],
                offendingIndex
            );
        }
    }

    for (size_t index = 0; index < artifact.lockingScript.size(); ++index) {
        const BytecodeInstruction& instruction =
            artifact.lockingScript[index];
        if (instruction.region != ScriptRegion::ImmutableSuffix) {
            continue;
        }

        MaterializedAtom atom;
        auto encoded =
            encodeInstruction(instruction, index, bindings, atom);
        if (encoded.error.code != MaterializationErrorCode::None) {
            return encoded;
        }

        size_t updated = 0;
        const size_t atomBytes = atom.encodedHex.size() / 2;
        if (!checkedAdd(regions.suffixBytes, atomBytes, updated)) {
            return fail(
                MaterializationErrorCode::ScriptSizeOverflow,
                "immutable suffix byte count overflowed",
                &instruction,
                index
            );
        }
        regions.suffixBytes = updated;
        regions.suffix.push_back(std::move(atom));
    }

    return MaterializationResult{};
}

[[nodiscard]] std::optional<std::vector<MaterializedAtom>> makePadding(
    size_t paddingBytes
)
{
    std::vector<MaterializedAtom> atoms;
    if (paddingBytes == 0) {
        return atoms;
    }

    // A direct push can occupy exactly 1..76 bytes (OP_0 for one byte, or a
    // one-byte length plus up to 75 bytes of 0xff). Split larger alignments
    // into such chunks; every resulting push is independently minimal.
    constexpr size_t kMaximumDirectPushBytes = 76;
    atoms.reserve(
        paddingBytes / kMaximumDirectPushBytes +
        (paddingBytes % kMaximumDirectPushBytes != 0 ? 1 : 0)
    );

    size_t remaining = paddingBytes;
    while (remaining != 0) {
        const size_t encodedBytes =
            std::min(remaining, kMaximumDirectPushBytes);
        std::vector<uint8_t> payload(
            encodedBytes == 1 ? 0 : encodedBytes - 1, 0xff
        );
        auto encoded = ScriptCodec::encodePush(
            payload, PushEncodingPolicy::Canonical
        );
        if (!encoded.has_value() ||
            encoded->size() / 2 != encodedBytes) {
            return std::nullopt;
        }

        MaterializedAtom atom;
        atom.region = ScriptRegion::Padding;
        atom.encodedHex = std::move(encoded.value());
        atoms.push_back(std::move(atom));
        remaining -= encodedBytes;
    }
    return atoms;
}

} // namespace

BytecodeTemplateDescription BytecodeMaterializer::describeTemplate(
    const BytecodeArtifact& artifact
)
{
    BytecodeTemplateDescription description;
    description.format = artifact.format;
    for (size_t index = 0; index < artifact.lockingScript.size(); ++index) {
        const BytecodeInstruction& instruction =
            artifact.lockingScript[index];
        const auto* placeholder =
            std::get_if<PlaceholderPushInstruction>(&instruction.body);
        if (!placeholder) {
            continue;
        }
        description.relocations.push_back(TemplateRelocation{
            instruction.id,
            index,
            placeholder->label,
            placeholder->expectedPayloadSize,
            instruction.region,
        });
    }
    return description;
}

MaterializationResult BytecodeMaterializer::materialize(
    const BytecodeArtifact& artifact,
    const PlaceholderBindings& bindings
)
{
    if (artifact.format != ArtifactFormat::CanonicalV2) {
        return fail(
            MaterializationErrorCode::UnsupportedArtifactFormat,
            "canonical materialization requires ArtifactFormat::CanonicalV2"
        );
    }

    std::unordered_set<InstructionId> instructionIds;
    for (size_t index = 0; index < artifact.lockingScript.size(); ++index) {
        const auto& instruction = artifact.lockingScript[index];
        if (!instructionIds.insert(instruction.id).second) {
            return fail(
                MaterializationErrorCode::DuplicateInstructionId,
                "typed artifact contains duplicate instruction IDs",
                &instruction,
                index
            );
        }
    }

    EncodedRegions regions;
    auto encoded = materializeRegions(artifact, bindings, regions);
    if (encoded.error.code != MaterializationErrorCode::None) {
        return encoded;
    }

    size_t paddingBytes = 0;
    if (artifact.layout.executableAlignment.has_value()) {
        const size_t alignment = *artifact.layout.executableAlignment;
        const size_t remainder = regions.executableBytes % alignment;
        paddingBytes = remainder == 0 ? 0 : alignment - remainder;
    }

    auto padding = makePadding(paddingBytes);
    if (!padding.has_value()) {
        return fail(
            MaterializationErrorCode::ScriptSizeOverflow,
            "unable to construct exact canonical push padding"
        );
    }

    MaterializedBytecode value;
    value.relocations = describeTemplate(artifact).relocations;
    value.executableBytesBeforePadding = regions.executableBytes;
    value.paddingBytes = paddingBytes;
    value.immutableSuffixBytes = regions.suffixBytes;

    size_t atomCount = 0;
    if (!checkedAdd(regions.executable.size(), padding->size(), atomCount) ||
        !checkedAdd(atomCount, regions.suffix.size(), atomCount)) {
        return fail(
            MaterializationErrorCode::ScriptSizeOverflow,
            "materialized atom count overflowed"
        );
    }
    value.atoms.reserve(atomCount);
    value.atoms.insert(
        value.atoms.end(),
        std::make_move_iterator(regions.executable.begin()),
        std::make_move_iterator(regions.executable.end())
    );
    value.atoms.insert(
        value.atoms.end(),
        std::make_move_iterator(padding->begin()),
        std::make_move_iterator(padding->end())
    );
    value.atoms.insert(
        value.atoms.end(),
        std::make_move_iterator(regions.suffix.begin()),
        std::make_move_iterator(regions.suffix.end())
    );

    size_t totalBytes = 0;
    if (!checkedAdd(value.executableBytesBeforePadding,
                    value.paddingBytes,
                    totalBytes) ||
        !checkedAdd(totalBytes, value.immutableSuffixBytes, totalBytes) ||
        totalBytes > std::numeric_limits<size_t>::max() / 2) {
        return fail(
            MaterializationErrorCode::ScriptSizeOverflow,
            "materialized script size overflowed"
        );
    }
    value.finalHex.reserve(totalBytes * 2);
    for (const MaterializedAtom& atom : value.atoms) {
        value.finalHex += atom.encodedHex;
    }

    MaterializationResult result;
    result.value = std::move(value);
    return result;
}

const char* BytecodeMaterializer::errorName(
    MaterializationErrorCode code
) noexcept
{
    switch (code) {
        case MaterializationErrorCode::None:
            return "none";
        case MaterializationErrorCode::UnsupportedArtifactFormat:
            return "unsupported artifact format";
        case MaterializationErrorCode::DuplicateInstructionId:
            return "duplicate instruction ID";
        case MaterializationErrorCode::InvalidAlignment:
            return "invalid alignment";
        case MaterializationErrorCode::MissingOpReturn:
            return "missing OP_RETURN";
        case MaterializationErrorCode::ExecutableAfterFinalReturn:
            return "executable bytes after final OP_RETURN";
        case MaterializationErrorCode::InvalidRegionOrder:
            return "invalid region order";
        case MaterializationErrorCode::PreexistingPadding:
            return "preexisting padding";
        case MaterializationErrorCode::RawSuffixOutsideImmutableSuffix:
            return "raw suffix outside immutable suffix";
        case MaterializationErrorCode::InvalidRawSuffix:
            return "invalid raw suffix";
        case MaterializationErrorCode::LegacyBarrier:
            return "legacy barrier";
        case MaterializationErrorCode::InvalidOpcode:
            return "invalid opcode";
        case MaterializationErrorCode::InvalidPlaceholderLabel:
            return "invalid placeholder label";
        case MaterializationErrorCode::MissingBinding:
            return "missing binding";
        case MaterializationErrorCode::PayloadSizeMismatch:
            return "payload size mismatch";
        case MaterializationErrorCode::PayloadTooLarge:
            return "payload too large";
        case MaterializationErrorCode::ScriptSizeOverflow:
            return "script size overflow";
    }
    return "unknown materialization error";
}

SPACE_TBC_END
