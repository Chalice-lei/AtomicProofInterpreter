#include "bytecode/bytecode_materializer.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace tbc;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

BytecodeInstruction instruction(
    InstructionId id,
    InstructionBody body,
    ScriptRegion region = ScriptRegion::Executable
)
{
    BytecodeInstruction result;
    result.id = id;
    result.body = std::move(body);
    result.region = region;
    result.origins.push_back(id);
    return result;
}

BytecodeArtifact canonical(
    std::vector<BytecodeInstruction> instructions,
    std::optional<size_t> alignment = std::nullopt
)
{
    BytecodeArtifact result;
    result.format = ArtifactFormat::CanonicalV2;
    result.lockingScript = std::move(instructions);
    result.layout.executableAlignment = alignment;
    return result;
}

const MaterializedBytecode& requireSuccess(
    const MaterializationResult& result,
    std::string_view context
)
{
    const std::string contextText(context);
    require(
        result.ok(),
        contextText + ": " +
            BytecodeMaterializer::errorName(result.error.code) +
            " (" + result.error.message + ")"
    );
    require(
        result.value.has_value(), contextText + ": value is absent"
    );
    return *result.value;
}

void requireFailure(
    const MaterializationResult& result,
    MaterializationErrorCode expected,
    std::string_view context
)
{
    const std::string contextText(context);
    require(!result.ok(), contextText + ": unexpectedly succeeded");
    require(
        !result.value.has_value(),
        contextText + ": failure leaked partial final bytecode"
    );
    require(
        result.error.code == expected,
        contextText + ": expected " +
            BytecodeMaterializer::errorName(expected) + ", got " +
            BytecodeMaterializer::errorName(result.error.code)
    );
}

void testTemplateRelocations()
{
    BytecodeArtifact artifact = canonical({
        instruction(
            42,
            PlaceholderPushInstruction{"owner", size_t{20}}
        ),
        instruction(43, OpcodeInstruction{BytOpcode::OP_RETURN}),
        instruction(
            99,
            PlaceholderPushInstruction{"state", std::nullopt},
            ScriptRegion::ImmutableSuffix
        ),
    });

    const auto description =
        BytecodeMaterializer::describeTemplate(artifact);
    require(
        description.format == ArtifactFormat::CanonicalV2,
        "template format was not preserved"
    );
    require(
        description.hasUnresolvedPlaceholders(),
        "relocatable template claimed to be final"
    );
    require(description.relocations.size() == 2, "relocation was lost");
    require(
        description.relocations[0] == TemplateRelocation{
            42,
            0,
            "owner",
            size_t{20},
            ScriptRegion::Executable,
        },
        "executable relocation descriptor is wrong"
    );
    require(
        description.relocations[1] == TemplateRelocation{
            99,
            2,
            "state",
            std::nullopt,
            ScriptRegion::ImmutableSuffix,
        },
        "suffix relocation descriptor is wrong"
    );

    BytecodeArtifact concrete = canonical({
        instruction(1, OpcodeInstruction{BytOpcode::OP_RETURN}),
    });
    require(
        !BytecodeMaterializer::describeTemplate(concrete)
             .hasUnresolvedPlaceholders(),
        "concrete template reported unresolved relocations"
    );
}

void testCanonicalPushForms()
{
    auto legacySpelledOne =
        instruction(2, PushDataInstruction{{0x01}});
    legacySpelledOne.legacyEncoding = "0101";
    BytecodeArtifact artifact = canonical({
        instruction(0, PushDataInstruction{{}}),
        instruction(1, PushDataInstruction{{0x00}}),
        std::move(legacySpelledOne),
        instruction(3, PushDataInstruction{{0x10}}),
        instruction(4, PushDataInstruction{{0x81}}),
        instruction(5, PushDataInstruction{{0x11}}),
        instruction(6, OpcodeInstruction{BytOpcode::OP_RETURN}),
    });

    const auto result = BytecodeMaterializer::materialize(artifact, {});
    const auto& value = requireSuccess(result, "canonical push forms");
    const std::vector<std::string> expected{
        "00", "0100", "51", "60", "4f", "0111", "6a",
    };
    require(value.atoms.size() == expected.size(), "canonical atom loss");
    std::string expectedHex;
    for (size_t index = 0; index < expected.size(); ++index) {
        require(
            value.atoms[index].encodedHex == expected[index],
            "wrong canonical push form at atom " + std::to_string(index)
        );
        expectedHex += expected[index];
    }
    require(value.finalHex == expectedHex, "canonical final hex is wrong");
    require(
        value.atoms[2].encodedHex == "51",
        "CanonicalV2 replayed a LegacyV1 push spelling"
    );
    require(
        !value.hasUnresolvedPlaceholders(),
        "materialized output reported unresolved placeholders"
    );
}

void testPushBoundaries()
{
    struct Case
    {
        size_t payloadSize;
        std::string expectedPrefix;
        size_t expectedEncodedBytes;
    };
    const Case cases[] = {
        {75, "4b", 76},
        {76, "4c4c", 78},
        {255, "4cff", 257},
        {256, "4d0001", 259},
        {65535, "4dffff", 65538},
        {65536, "4e00000100", 65541},
    };

    for (const auto& test : cases) {
        BytecodeArtifact artifact = canonical({
            instruction(
                1,
                PushDataInstruction{
                    std::vector<uint8_t>(test.payloadSize, 0xaa)}
            ),
        });
        const auto result =
            BytecodeMaterializer::materialize(artifact, {});
        const auto& value =
            requireSuccess(result, "push boundary materialization");
        require(value.atoms.size() == 1, "boundary push split into atoms");
        require(
            value.atoms[0].encodedHex.starts_with(test.expectedPrefix),
            "wrong push prefix at payload size " +
                std::to_string(test.payloadSize)
        );
        require(
            value.atoms[0].encodedHex.size() / 2 ==
                test.expectedEncodedBytes,
            "wrong encoded size at payload boundary"
        );
    }
}

void testPlaceholderChangesDeferredPadding()
{
    BytecodeArtifact artifact = canonical(
        {
            instruction(
                7,
                PlaceholderPushInstruction{"value", size_t{1}}
            ),
            instruction(8, OpcodeInstruction{BytOpcode::OP_RETURN}),
        },
        8
    );

    const auto smallInteger = BytecodeMaterializer::materialize(
        artifact, {{"value", {0x01}}}
    );
    const auto& small =
        requireSuccess(smallInteger, "small-integer placeholder");
    require(
        small.executableBytesBeforePadding == 2 &&
            small.paddingBytes == 6,
        "OP_1 placeholder did not drive padding from concrete bytes"
    );
    require(
        small.finalHex == "516a05ffffffffff",
        "OP_1 placeholder padding bytes are wrong"
    );

    const auto zeroByte = BytecodeMaterializer::materialize(
        artifact, {{"value", {0x00}}}
    );
    const auto& zero = requireSuccess(zeroByte, "zero-byte placeholder");
    require(
        zero.executableBytesBeforePadding == 3 && zero.paddingBytes == 5,
        "byte 00 placeholder did not change delayed padding"
    );
    require(
        zero.finalHex == "01006a04ffffffff",
        "byte 00 placeholder padding bytes are wrong"
    );
    require(
        (small.executableBytesBeforePadding + small.paddingBytes) % 8 == 0 &&
            (zero.executableBytesBeforePadding + zero.paddingBytes) % 8 == 0,
        "materialized executable is not aligned"
    );
}

void testSuffixMaterializationAndOrdering()
{
    BytecodeArtifact artifact = canonical(
        {
            instruction(1, OpcodeInstruction{BytOpcode::OP_RETURN}),
            instruction(
                2,
                PlaceholderPushInstruction{"state", size_t{1}},
                ScriptRegion::ImmutableSuffix
            ),
            instruction(
                3,
                RawSuffixInstruction{"0xAAbB"},
                ScriptRegion::ImmutableSuffix
            ),
        },
        4
    );

    const auto result = BytecodeMaterializer::materialize(
        artifact, {{"state", {0x01}}}
    );
    const auto& value = requireSuccess(result, "suffix placeholder");
    require(value.paddingBytes == 3, "suffix changed executable padding");
    require(value.immutableSuffixBytes == 3, "suffix size is wrong");
    require(
        value.finalHex == "6a02ffff51aabb",
        "suffix was not appended after materialized padding"
    );
    require(value.atoms.size() == 4, "suffix/padding atom count is wrong");
    require(
        value.atoms[1].region == ScriptRegion::Padding &&
            !value.atoms[1].sourceInstructionId.has_value() &&
            !value.atoms[1].sourceInstructionIndex.has_value(),
        "generated padding acquired a source identity"
    );
    require(
        value.atoms[2].sourceInstructionId == 2 &&
            value.atoms[2].sourceInstructionIndex == 1,
        "suffix placeholder source identity was lost"
    );
}

void testExactPaddingBoundaries()
{
    BytecodeArtifact oneByte = canonical(
        {instruction(1, OpcodeInstruction{BytOpcode::OP_RETURN})}, 64
    );
    const auto oneResult =
        BytecodeMaterializer::materialize(oneByte, {});
    const auto& one = requireSuccess(oneResult, "63-byte padding");
    require(one.paddingBytes == 63, "alignment-1 padding is wrong");
    require(one.atoms.size() == 2, "63-byte padding was split needlessly");
    require(
        one.atoms[1].encodedHex.size() / 2 == 63 &&
            one.atoms[1].encodedHex.starts_with("3e"),
        "63-byte padding is not one canonical direct push"
    );

    std::vector<BytecodeInstruction> alignedInstructions;
    for (InstructionId id = 0; id < 63; ++id) {
        alignedInstructions.push_back(
            instruction(id, OpcodeInstruction{BytOpcode::OP_NOP})
        );
    }
    alignedInstructions.push_back(
        instruction(63, OpcodeInstruction{BytOpcode::OP_RETURN})
    );
    BytecodeArtifact aligned =
        canonical(std::move(alignedInstructions), 64);
    const auto alignedResult =
        BytecodeMaterializer::materialize(aligned, {});
    const auto& exact = requireSuccess(alignedResult, "already aligned");
    require(
        exact.paddingBytes == 0 && exact.atoms.size() == 64,
        "already aligned executable received padding"
    );

    BytecodeArtifact wide = canonical(
        {instruction(1, OpcodeInstruction{BytOpcode::OP_RETURN})}, 128
    );
    const auto wideResult = BytecodeMaterializer::materialize(wide, {});
    const auto& wideValue = requireSuccess(wideResult, "wide alignment");
    require(wideValue.paddingBytes == 127, "wide padding size is wrong");
    require(
        wideValue.atoms.size() == 3 &&
            wideValue.atoms[1].encodedHex.size() / 2 == 76 &&
            wideValue.atoms[2].encodedHex.size() / 2 == 51,
        "wide padding was not split into exact canonical pushes"
    );
}

void testNoReturnSemantics()
{
    BytecodeArtifact unaligned = canonical({
        instruction(1, PushDataInstruction{{0x01}}),
    });
    requireSuccess(
        BytecodeMaterializer::materialize(unaligned, {}),
        "unaligned script without return"
    );

    BytecodeArtifact aligned = canonical(
        {instruction(1, PushDataInstruction{{0x01}})}, 64
    );
    requireFailure(
        BytecodeMaterializer::materialize(aligned, {}),
        MaterializationErrorCode::MissingOpReturn,
        "aligned script without return"
    );

    BytecodeArtifact zeroAlignment = canonical(
        {instruction(1, OpcodeInstruction{BytOpcode::OP_RETURN})}, 0
    );
    requireFailure(
        BytecodeMaterializer::materialize(zeroAlignment, {}),
        MaterializationErrorCode::InvalidAlignment,
        "zero alignment"
    );
}

void testStrictFailuresAreAtomic()
{
    BytecodeArtifact missing = canonical({
        instruction(5, PlaceholderPushInstruction{"owner", size_t{2}}),
    });
    auto missingResult = BytecodeMaterializer::materialize(missing, {});
    requireFailure(
        missingResult,
        MaterializationErrorCode::MissingBinding,
        "missing placeholder binding"
    );
    require(
        missingResult.error.instructionId == 5 &&
            missingResult.error.instructionIndex == 0 &&
            missingResult.error.label == "owner",
        "missing-binding diagnostics lost relocation identity"
    );

    auto mismatchResult = BytecodeMaterializer::materialize(
        missing, {{"owner", {0xaa}}}
    );
    requireFailure(
        mismatchResult,
        MaterializationErrorCode::PayloadSizeMismatch,
        "placeholder size mismatch"
    );

    BytecodeArtifact missingSuffix = canonical(
        {
            instruction(10, OpcodeInstruction{BytOpcode::OP_RETURN}),
            instruction(
                11,
                PlaceholderPushInstruction{"state", size_t{1}},
                ScriptRegion::ImmutableSuffix
            ),
        },
        64
    );
    requireFailure(
        BytecodeMaterializer::materialize(missingSuffix, {}),
        MaterializationErrorCode::MissingBinding,
        "missing suffix binding after padding computation"
    );

    BytecodeArtifact barrier = canonical({
        instruction(1, PushDataInstruction{{0x01}}),
        instruction(2, LegacyBarrierInstruction{"not-hex"}),
    });
    requireFailure(
        BytecodeMaterializer::materialize(barrier, {}),
        MaterializationErrorCode::LegacyBarrier,
        "legacy barrier"
    );

    BytecodeArtifact invalidRaw = canonical({
        instruction(1, OpcodeInstruction{BytOpcode::OP_RETURN}),
        instruction(
            2,
            RawSuffixInstruction{"0xz1"},
            ScriptRegion::ImmutableSuffix
        ),
    });
    requireFailure(
        BytecodeMaterializer::materialize(invalidRaw, {}),
        MaterializationErrorCode::InvalidRawSuffix,
        "invalid raw suffix"
    );

    BytecodeArtifact misplacedRaw = canonical({
        instruction(1, RawSuffixInstruction{"aa"}),
    });
    requireFailure(
        BytecodeMaterializer::materialize(misplacedRaw, {}),
        MaterializationErrorCode::RawSuffixOutsideImmutableSuffix,
        "misplaced raw suffix"
    );

    BytecodeArtifact padding = canonical({
        instruction(
            1,
            PushDataInstruction{{}},
            ScriptRegion::Padding
        ),
    });
    requireFailure(
        BytecodeMaterializer::materialize(padding, {}),
        MaterializationErrorCode::PreexistingPadding,
        "preexisting padding"
    );

    BytecodeArtifact invalidOpcode = canonical({
        instruction(
            1,
            OpcodeInstruction{static_cast<BytOpcode>(0x01)}
        ),
    });
    requireFailure(
        BytecodeMaterializer::materialize(invalidOpcode, {}),
        MaterializationErrorCode::InvalidOpcode,
        "incomplete push opcode"
    );
}

void testRegionValidation()
{
    BytecodeArtifact duplicateIds = canonical({
        instruction(7, PushDataInstruction{{0x01}}),
        instruction(7, PushDataInstruction{{0x02}}),
    });
    const auto duplicateResult =
        BytecodeMaterializer::materialize(duplicateIds, {});
    requireFailure(
        duplicateResult,
        MaterializationErrorCode::DuplicateInstructionId,
        "duplicate instruction identity"
    );
    require(
        duplicateResult.error.instructionId == 7 &&
            duplicateResult.error.instructionIndex == 1,
        "duplicate identity error lost its offending instruction"
    );

    BytecodeArtifact suffixWithoutReturn = canonical({
        instruction(0, OpcodeInstruction{BytOpcode::OP_NOP}),
        instruction(
            1,
            RawSuffixInstruction{"aa"},
            ScriptRegion::ImmutableSuffix
        ),
    });
    requireFailure(
        BytecodeMaterializer::materialize(suffixWithoutReturn, {}),
        MaterializationErrorCode::MissingOpReturn,
        "immutable suffix without terminal return"
    );

    BytecodeArtifact wrongOrder = canonical({
        instruction(
            1,
            RawSuffixInstruction{"aa"},
            ScriptRegion::ImmutableSuffix
        ),
        instruction(2, OpcodeInstruction{BytOpcode::OP_RETURN}),
    });
    requireFailure(
        BytecodeMaterializer::materialize(wrongOrder, {}),
        MaterializationErrorCode::InvalidRegionOrder,
        "executable after suffix"
    );

    BytecodeArtifact afterReturn = canonical(
        {
            instruction(1, OpcodeInstruction{BytOpcode::OP_RETURN}),
            instruction(2, OpcodeInstruction{BytOpcode::OP_NOP}),
        },
        64
    );
    requireFailure(
        BytecodeMaterializer::materialize(afterReturn, {}),
        MaterializationErrorCode::ExecutableAfterFinalReturn,
        "executable after final return"
    );
}

void testLegacyV1IsNeverCanonicalized()
{
    BytecodeArtifact legacy;
    legacy.format = ArtifactFormat::LegacyV1;
    BytecodeInstruction instructionValue =
        instruction(1, PushDataInstruction{{0x01}});
    instructionValue.legacyEncoding = "0101";
    legacy.lockingScript.push_back(std::move(instructionValue));

    requireFailure(
        BytecodeMaterializer::materialize(legacy, {}),
        MaterializationErrorCode::UnsupportedArtifactFormat,
        "LegacyV1 canonical materialization"
    );
}

} // namespace

int main()
{
    try {
        testTemplateRelocations();
        testCanonicalPushForms();
        testPushBoundaries();
        testPlaceholderChangesDeferredPadding();
        testSuffixMaterializationAndOrdering();
        testExactPaddingBoundaries();
        testNoReturnSemantics();
        testStrictFailuresAreAtomic();
        testRegionValidation();
        testLegacyV1IsNeverCanonicalized();
    } catch (const std::exception& error) {
        std::cerr << "bytecode_materializer_test: " << error.what() << '\n';
        return 1;
    }

    std::cout << "bytecode_materializer_test: all checks passed\n";
    return 0;
}
