#ifndef BYTECODE_IR_H
#define BYTECODE_IR_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "byt_defs.h"
#include "bytecode_opcodes.h"

SPACE_TBC_START

using InstructionId = uint64_t;
using OriginId = uint64_t;

enum class ScriptRegion
{
    Executable,
    Padding,
    ImmutableSuffix,
};

enum class ArtifactFormat
{
    LegacyV1,
    CanonicalV2,
};

struct OpcodeInstruction
{
    BytOpcode opcode{BytOpcode::OP_INVALIDOPCODE};

    bool operator==(const OpcodeInstruction&) const = default;
};

struct PushDataInstruction
{
    std::vector<uint8_t> payload;

    bool operator==(const PushDataInstruction&) const = default;
};

struct PlaceholderPushInstruction
{
    std::string label;
    std::optional<size_t> expectedPayloadSize;

    bool operator==(const PlaceholderPushInstruction&) const = default;
};

// Immutable state data may still contain deployment placeholders, so it is
// represented as an opaque atom until the materialization phase.
struct RawSuffixInstruction
{
    std::string raw;

    bool operator==(const RawSuffixInstruction&) const = default;
};

// A migration-only escape hatch. Optimizers must treat this as an effectful
// barrier and CanonicalV2 emission must reject it.
struct LegacyBarrierInstruction
{
    std::string raw;

    bool operator==(const LegacyBarrierInstruction&) const = default;
};

using InstructionBody = std::variant<
    OpcodeInstruction,
    PushDataInstruction,
    PlaceholderPushInstruction,
    RawSuffixInstruction,
    LegacyBarrierInstruction>;

struct BytecodeInstruction
{
    InstructionId id{0};
    InstructionBody body{LegacyBarrierInstruction{}};
    ScriptRegion region{ScriptRegion::Executable};
    std::vector<OriginId> origins;

    // Populated only by the compatibility importer. Legacy export replays it
    // verbatim, retaining prefix spelling, case and non-minimal Push forms.
    std::optional<std::string> legacyEncoding;

    bool operator==(const BytecodeInstruction&) const = default;
};

using UnlockTemplateMap = std::unordered_map<std::string, std::string>;

struct ConstructorFieldSchema
{
    std::string name;
    std::string type;
    std::optional<size_t> fixedPayloadSize;

    bool operator==(const ConstructorFieldSchema&) const = default;
};

struct ConstructorSchema
{
    std::vector<ConstructorFieldSchema> fields;

    bool operator==(const ConstructorSchema&) const = default;
};

struct LayoutDirectives
{
    std::optional<size_t> executableAlignment;
    bool requiresMaterialization{false};

    bool operator==(const LayoutDirectives&) const = default;
};

struct BytecodeArtifact
{
    ArtifactFormat format{ArtifactFormat::LegacyV1};
    std::vector<BytecodeInstruction> lockingScript;
    UnlockTemplateMap unlockingScripts;
    ConstructorSchema constructorSchema;
    LayoutDirectives layout;
};

SPACE_TBC_END

#endif // BYTECODE_IR_H
