#ifndef BYTECODE_MATERIALIZER_H
#define BYTECODE_MATERIALIZER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bytecode_ir.h"

SPACE_TBC_START

using PlaceholderBindings =
    std::unordered_map<std::string, std::vector<uint8_t>>;

struct TemplateRelocation
{
    InstructionId instructionId{0};
    size_t instructionIndex{0};
    std::string label;
    std::optional<size_t> expectedPayloadSize;
    ScriptRegion region{ScriptRegion::Executable};

    bool operator==(const TemplateRelocation&) const = default;
};

struct BytecodeTemplateDescription
{
    ArtifactFormat format{ArtifactFormat::LegacyV1};
    std::vector<TemplateRelocation> relocations;

    // A template containing relocations is intentionally not a final locking
    // script. Only MaterializedBytecode exposes finalHex.
    [[nodiscard]] bool hasUnresolvedPlaceholders() const noexcept
    {
        return !relocations.empty();
    }
};

struct MaterializedAtom
{
    // Padding is generated during materialization and therefore has no source
    // instruction identity/index. All template atoms retain both values.
    std::optional<InstructionId> sourceInstructionId;
    std::optional<size_t> sourceInstructionIndex;
    ScriptRegion region{ScriptRegion::Executable};
    std::string encodedHex;

    bool operator==(const MaterializedAtom&) const = default;
};

struct MaterializedBytecode
{
    std::string finalHex;
    std::vector<MaterializedAtom> atoms;
    std::vector<TemplateRelocation> relocations;
    size_t executableBytesBeforePadding{0};
    size_t paddingBytes{0};
    size_t immutableSuffixBytes{0};

    // Construction is transactional: an instance exists only after every
    // placeholder has been resolved and every atom has been validated.
    [[nodiscard]] bool hasUnresolvedPlaceholders() const noexcept
    {
        return false;
    }
};

enum class MaterializationErrorCode
{
    None,
    UnsupportedArtifactFormat,
    DuplicateInstructionId,
    InvalidAlignment,
    MissingOpReturn,
    ExecutableAfterFinalReturn,
    InvalidRegionOrder,
    PreexistingPadding,
    RawSuffixOutsideImmutableSuffix,
    InvalidRawSuffix,
    LegacyBarrier,
    InvalidOpcode,
    InvalidPlaceholderLabel,
    MissingBinding,
    PayloadSizeMismatch,
    PayloadTooLarge,
    ScriptSizeOverflow,
};

struct MaterializationError
{
    MaterializationErrorCode code{MaterializationErrorCode::None};
    std::optional<InstructionId> instructionId;
    std::optional<size_t> instructionIndex;
    std::string label;
    std::string message;
};

struct MaterializationResult
{
    std::optional<MaterializedBytecode> value;
    MaterializationError error;

    [[nodiscard]] bool ok() const noexcept
    {
        return value.has_value() &&
               error.code == MaterializationErrorCode::None;
    }

    explicit operator bool() const noexcept
    {
        return ok();
    }
};

// Finalizes a CanonicalV2 artifact in three ordered phases:
//   1. materialize and canonically encode the executable region;
//   2. insert exact canonical push padding after its final OP_RETURN;
//   3. materialize/validate and append the immutable suffix.
//
// LegacyV1 is rejected deliberately. Its byte-preserving serialization stays
// at LegacyBytecodeAdapter::exportPreserving(), so calling this class can
// never silently canonicalize a deployed legacy template.
class BytecodeMaterializer final
{
public:
    [[nodiscard]] static BytecodeTemplateDescription describeTemplate(
        const BytecodeArtifact& artifact
    );

    [[nodiscard]] static MaterializationResult materialize(
        const BytecodeArtifact& artifact,
        const PlaceholderBindings& bindings
    );

    [[nodiscard]] static const char* errorName(
        MaterializationErrorCode code
    ) noexcept;
};

SPACE_TBC_END

#endif // BYTECODE_MATERIALIZER_H
