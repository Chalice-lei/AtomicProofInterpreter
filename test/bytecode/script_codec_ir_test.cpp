#include "bytecode/bytecode_helper_fun.h"
#include "bytecode/bytecode_artifact_rewriter.h"
#include "bytecode/bytecode_ir.h"
#include "bytecode/bytecode_materializer.h"
#include "bytecode/legacy_bytecode_adapter.h"
#include "bytecode/script_codec.h"
#include "util/byt_fun.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
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

void requireError(
    const std::string& encoding,
    ScriptCodecError expected,
    bool requireCanonical = true
)
{
    const auto result =
        ScriptCodec::decodePushHex(encoding, requireCanonical);
    require(
        !result.ok() && result.error == expected,
        "expected '" + encoding + "' to fail with " +
            ScriptCodec::errorName(expected) + ", got " +
            ScriptCodec::errorName(result.error)
    );
}

std::string encode(
    const std::vector<uint8_t>& payload,
    PushEncodingPolicy policy
)
{
    auto result = ScriptCodec::encodePush(payload, policy);
    require(result.has_value(), "push unexpectedly exceeded codec limits");
    return result.value();
}

void testPrefixBoundaries()
{
    struct Case
    {
        size_t size;
        std::string prefix;
    };
    const Case cases[] = {
        {0, "00"},       {1, "01"},       {75, "4b"},
        {76, "4c4c"},   {255, "4cff"},   {256, "4d0001"},
        {65535, "4dffff"}, {65536, "4e00000100"},
    };
    for (const auto& test : cases) {
        auto actual = ScriptCodec::legacyPushPrefixHex(test.size);
        require(
            actual.has_value() && actual.value() == test.prefix,
            "wrong legacy prefix at payload size " +
                std::to_string(test.size)
        );
        require(
            bytEncodeLengthOpcode(static_cast<int>(test.size)) == test.prefix,
            "byt_fun compatibility adapter drifted"
        );
    }

    const size_t maximum = std::numeric_limits<uint32_t>::max();
    require(
        ScriptCodec::legacyPushPrefixHex(maximum).value() == "4effffffff",
        "uint32 maximum prefix is wrong"
    );
    if (std::numeric_limits<size_t>::max() > maximum) {
        require(
            !ScriptCodec::legacyPushPrefixHex(maximum + size_t{1})
                 .has_value(),
            "oversized payload prefix must be rejected"
        );
    }
}

void testCanonicalEncoding()
{
    require(encode({}, PushEncodingPolicy::Canonical) == "00", "empty");
    require(
        encode({0x00}, PushEncodingPolicy::Canonical) == "0100",
        "byte 00 is not the empty vector"
    );
    require(encode({0x01}, PushEncodingPolicy::Canonical) == "51", "one");
    require(encode({0x10}, PushEncodingPolicy::Canonical) == "60", "sixteen");
    require(
        encode({0x11}, PushEncodingPolicy::Canonical) == "0111",
        "seventeen"
    );
    require(
        encode({0x81}, PushEncodingPolicy::Canonical) == "4f",
        "minus one"
    );
    require(
        encode({0x82}, PushEncodingPolicy::Canonical) == "0182",
        "non-special one-byte payload"
    );

    require(
        encode({0x01}, PushEncodingPolicy::Legacy) == "0101",
        "legacy one-byte push changed"
    );
    require(
        encode({0x81}, PushEncodingPolicy::Legacy) == "0181",
        "legacy negative-one push changed"
    );

    const std::vector<size_t> boundaries{75, 76, 255, 256, 65535, 65536};
    for (size_t size : boundaries) {
        std::vector<uint8_t> payload(size, 0xaa);
        const std::string encoded =
            encode(payload, PushEncodingPolicy::Canonical);
        const auto decoded = ScriptCodec::decodePushHex(encoded, true);
        require(decoded.ok(), "canonical boundary push did not decode");
        require(
            decoded.value->payload == payload,
            "boundary payload changed after round trip"
        );
        require(
            ScriptCodec::serializedPushSize(
                payload, PushEncodingPolicy::Canonical
            ) == encoded.size() / 2,
            "serialized size does not match encoding"
        );
    }
}

void testStrictDecoding()
{
    const auto op0 = ScriptCodec::decodePushHex("0x00");
    require(op0.ok() && op0.value->payload.empty(), "OP_0 decode failed");

    const auto one = ScriptCodec::decodePushHex("51");
    require(
        one.ok() && one.value->payload == std::vector<uint8_t>{0x01},
        "OP_1 decode failed"
    );
    const auto minusOne = ScriptCodec::decodePushHex("4F");
    require(
        minusOne.ok() &&
            minusOne.value->payload == std::vector<uint8_t>{0x81},
        "OP_1NEGATE decode failed"
    );

    const auto nonMinimal = ScriptCodec::decodePushHex("0101", false);
    require(
        nonMinimal.ok() &&
            nonMinimal.value->payload == std::vector<uint8_t>{0x01},
        "structural mode must accept a complete legacy push"
    );
    requireError("0101", ScriptCodecError::NonMinimalPush);
    requireError("4c0100", ScriptCodecError::NonMinimalPush);
    requireError("4d010000", ScriptCodecError::NonMinimalPush);
    requireError("4e0100000000", ScriptCodecError::NonMinimalPush);

    requireError("", ScriptCodecError::EmptyInput);
    requireError("0x", ScriptCodecError::EmptyInput);
    requireError("0xz1", ScriptCodecError::InvalidHex);
    requireError("abc", ScriptCodecError::OddHexLength);
    requireError("01", ScriptCodecError::TruncatedPayload);
    requireError("4c", ScriptCodecError::TruncatedLength);
    requireError("4d01", ScriptCodecError::TruncatedLength);
    requireError("4e010203", ScriptCodecError::TruncatedLength);
    requireError("4c02aa", ScriptCodecError::TruncatedPayload);
    requireError("4d0200aa", ScriptCodecError::TruncatedPayload);
    requireError("4e02000000aa", ScriptCodecError::TruncatedPayload);
    requireError("4c01aabb", ScriptCodecError::TrailingBytes, false);
    requireError("4d0100aabb", ScriptCodecError::TrailingBytes, false);
    requireError("4e01000000aabb", ScriptCodecError::TrailingBytes, false);
    requireError("76", ScriptCodecError::NotPush);

    const std::vector<uint8_t> fragment{0x01, 0xaa, 0x75};
    const auto prefix = ScriptCodec::decodePushPrefix(fragment, false);
    require(
        prefix.ok() && prefix.value->encodedSize == 2 &&
            prefix.value->payload == std::vector<uint8_t>{0xaa},
        "prefix decoder consumed a following instruction"
    );
}

void testLegacyHelperCompatibility()
{
    require(
        encodePushData(std::vector<uint8_t>{0x01}) == "0101",
        "encodePushData no longer uses Legacy policy"
    );
    require(numberToScriptHex(1) == "0x51", "Script number changed");
    require(
        stringToScriptHex("A") == "0x0141",
        "legacy string encoding changed"
    );
    require(
        hexToScriptHex("0x01") == "0x0101",
        "legacy hex literal encoding changed"
    );
    require(
        hexToScriptHex("abc") == "0x01abc",
        "odd-length LegacyV1 compatibility changed"
    );
}

void testTypedIrAndLosslessAdapter()
{
    LegacyBytecode legacy{
        {"0X51", "0101", "76AA", "<arg>", "6A", "AAFF",
         "<self.state20>", "not-hex"},
        {{"spend", "<sig><pub>"}}};

    BytecodeArtifact artifact = LegacyBytecodeAdapter::import(legacy);
    require(artifact.format == ArtifactFormat::LegacyV1, "format drifted");
    require(artifact.lockingScript.size() == legacy.first.size(), "atom loss");
    require(
        std::holds_alternative<PushDataInstruction>(
            artifact.lockingScript[0].body
        ),
        "small integer must be represented as typed push data"
    );
    require(
        std::get<PushDataInstruction>(artifact.lockingScript[0].body)
                .payload == std::vector<uint8_t>{0x01},
        "small integer payload semantics changed"
    );
    require(
        std::holds_alternative<PushDataInstruction>(
            artifact.lockingScript[1].body
        ),
        "legacy non-minimal push was not decoded"
    );
    require(
        std::holds_alternative<LegacyBarrierInstruction>(
            artifact.lockingScript[2].body
        ),
        "compound malformed atom must be a barrier"
    );
    require(
        std::holds_alternative<PlaceholderPushInstruction>(
            artifact.lockingScript[3].body
        ),
        "placeholder was not typed"
    );
    require(
        std::holds_alternative<OpcodeInstruction>(
            artifact.lockingScript[4].body
        ),
        "OP_RETURN was not typed"
    );
    for (size_t index : {size_t{5}, size_t{7}}) {
        const auto& instruction = artifact.lockingScript[index];
        require(
            instruction.region == ScriptRegion::ImmutableSuffix &&
                std::holds_alternative<RawSuffixInstruction>(instruction.body),
            "post-RETURN state must remain opaque"
        );
    }
    require(
        artifact.lockingScript[6].region == ScriptRegion::ImmutableSuffix &&
            std::holds_alternative<PlaceholderPushInstruction>(
                artifact.lockingScript[6].body
            ) &&
            std::get<PlaceholderPushInstruction>(
                artifact.lockingScript[6].body
            ).label == "self.state20",
        "suffix placeholder lost its relocation semantics"
    );
    for (size_t index = 0; index < artifact.lockingScript.size(); ++index) {
        require(
            artifact.lockingScript[index].id == index &&
                artifact.lockingScript[index].origins ==
                    std::vector<OriginId>{index},
            "legacy provenance was not initialized"
        );
    }

    require(
        LegacyBytecodeAdapter::exportPreserving(artifact) == legacy,
        "legacy adapter did not round-trip byte-for-byte"
    );

    BytecodeArtifact synthesized;
    synthesized.lockingScript = {
        {1, OpcodeInstruction{BytOpcode::OP_DROP}, ScriptRegion::Executable,
         {}, std::nullopt},
        {2, PushDataInstruction{{0x01}}, ScriptRegion::Executable, {},
         std::nullopt},
        {3, PlaceholderPushInstruction{"x", 1}, ScriptRegion::Executable, {},
         std::nullopt},
        {4, RawSuffixInstruction{"deadbeef"},
         ScriptRegion::ImmutableSuffix, {}, std::nullopt},
    };
    const LegacyBytecode exported =
        LegacyBytecodeAdapter::exportPreserving(synthesized);
    require(
        exported.first ==
            std::vector<std::string>{"75", "0101", "<x>", "deadbeef"},
        "typed instructions did not synthesize LegacyV1 correctly"
    );
}

void testStrictLegacyFragmentSplitting()
{
    const auto split =
        LegacyBytecodeAdapter::splitScriptFragment("0X514C01AA75", 10);
    require(split.ok(), "valid compatibility fragment was rejected");
    require(split.instructions.size() == 3, "fragment boundary count drifted");
    require(
        split.instructions[0].legacyEncoding == "51" &&
            split.instructions[1].legacyEncoding == "4C01AA" &&
            split.instructions[2].legacyEncoding == "75",
        "fragment spelling was not preserved"
    );
    require(
        split.instructions[0].id == 10 &&
            split.instructions[2].id == 12,
        "fragment IDs were not assigned monotonically"
    );
    require(
        std::holds_alternative<PushDataInstruction>(
            split.instructions[0].body
        ) &&
            std::holds_alternative<PushDataInstruction>(
                split.instructions[1].body
            ) &&
            std::holds_alternative<OpcodeInstruction>(
                split.instructions[2].body
            ),
        "fragment instructions were mistyped"
    );

    const auto placeholder =
        LegacyBytecodeAdapter::splitScriptFragment("<self.x20>", 5);
    require(
        placeholder.ok() && placeholder.instructions.size() == 1 &&
            std::holds_alternative<PlaceholderPushInstruction>(
                placeholder.instructions.front().body
            ),
        "whole placeholder fragment was rejected"
    );

    const auto truncated =
        LegacyBytecodeAdapter::splitScriptFragment("754c02aa");
    require(
        !truncated.ok() && truncated.instructions.empty() &&
            truncated.error == ScriptCodecError::TruncatedPayload &&
            truncated.errorByteOffset == 1,
        "malformed fragment must fail transactionally at its push"
    );
    const auto mixed =
        LegacyBytecodeAdapter::splitScriptFragment("51<self.x20>");
    require(
        !mixed.ok() && mixed.instructions.empty() &&
            mixed.error == ScriptCodecError::InvalidHex,
        "mixed placeholder fragment must be rejected"
    );
}

void testTypedArtifactRewriteKeepsOrigins()
{
    LegacyBytecode legacy{{"51", "52", "87", "6a", "<self.state20>"}, {}};
    auto artifact = LegacyBytecodeAdapter::import(legacy);
    artifact.format = ArtifactFormat::CanonicalV2;
    artifact.layout.executableAlignment = 64;
    auto* placeholder = std::get_if<PlaceholderPushInstruction>(
        &artifact.lockingScript.back().body
    );
    require(placeholder != nullptr, "fixture placeholder was not typed");
    placeholder->expectedPayloadSize = 20;

    const std::vector<size_t> oldToNew{0, 0, 1, 2, 3};
    const auto reverse = BytecodeArtifactRewriter::reverseMapping(
        oldToNew, 4
    );
    auto rewritten = BytecodeArtifactRewriter::rewrite(
        artifact,
        {"52", "87", "6a", "<self.state20>"},
        reverse
    );
    require(rewritten.has_value(), "typed rewrite failed");
    require(
        rewritten->format == ArtifactFormat::CanonicalV2 &&
            rewritten->layout.executableAlignment == 64,
        "artifact metadata was lost"
    );
    require(
        rewritten->lockingScript[0].origins ==
            std::vector<OriginId>({0, 1}),
        "many-to-one instruction origins were lost"
    );
    const auto* rewrittenPlaceholder =
        std::get_if<PlaceholderPushInstruction>(
            &rewritten->lockingScript.back().body
        );
    require(
        rewrittenPlaceholder &&
            rewrittenPlaceholder->expectedPayloadSize == 20,
        "placeholder schema was lost across a pass rewrite"
    );

    const std::vector<size_t> withSynthetic{0, 2, 3, 4};
    const auto withPadding = BytecodeArtifactRewriter::reverseMapping(
        withSynthetic, 5
    );
    require(
        withPadding.size() == 5 && withPadding[1].empty(),
        "synthetic output slot was not represented"
    );
}

BytecodeInstruction typedInstruction(
    InstructionId id,
    OriginId origin,
    InstructionBody body,
    ScriptRegion region,
    const std::string& legacyEncoding
)
{
    BytecodeInstruction instruction;
    instruction.id = id;
    instruction.origins = {origin};
    instruction.body = std::move(body);
    instruction.region = region;
    instruction.legacyEncoding = legacyEncoding;
    return instruction;
}

void testTypedArtifactRewriteIdentityAndRegions()
{
    BytecodeArtifact oneToOne;
    oneToOne.format = ArtifactFormat::CanonicalV2;
    oneToOne.lockingScript.push_back(typedInstruction(
        42,
        7,
        PushDataInstruction{{0x01}},
        ScriptRegion::Executable,
        "0101"
    ));
    auto preserved = BytecodeArtifactRewriter::rewrite(
        oneToOne, {"0101"}, {{0}}
    );
    require(preserved.has_value(), "one-to-one typed rewrite failed");
    require(
        preserved->lockingScript[0].id == 42 &&
            preserved->lockingScript[0].origins ==
                std::vector<OriginId>{7} &&
            std::holds_alternative<PushDataInstruction>(
                preserved->lockingScript[0].body
            ),
        "one-to-one rewrite replaced stable identity with an origin ID"
    );

    BytecodeArtifact merged;
    merged.lockingScript = {
        typedInstruction(
            90,
            100,
            PushDataInstruction{{0x01}},
            ScriptRegion::Executable,
            "51"
        ),
        typedInstruction(
            4,
            200,
            PushDataInstruction{{0x02}},
            ScriptRegion::Executable,
            "52"
        ),
    };
    auto folded = BytecodeArtifactRewriter::rewrite(
        merged, {"52"}, {{1, 0}}
    );
    require(folded.has_value(), "many-to-one typed rewrite failed");
    require(
        folded->lockingScript[0].id == 90 &&
            folded->lockingScript[0].origins ==
                std::vector<OriginId>({100, 200}),
        "many-to-one rewrite did not choose the lowest source PC identity"
    );

    merged.lockingScript[1].region = ScriptRegion::ImmutableSuffix;
    std::string regionError;
    require(
        !BytecodeArtifactRewriter::rewrite(
             merged, {"52"}, {{0, 1}}, &regionError
         ).has_value() &&
            regionError.find("different script regions") !=
                std::string::npos,
        "cross-region merge was accepted"
    );
}

void testTypedArtifactRewriteSyntheticIdentity()
{
    BytecodeArtifact artifact;
    BytecodeInstruction implicitOrigin;
    implicitOrigin.id = 1;
    implicitOrigin.body = PushDataInstruction{{0x01}};
    implicitOrigin.region = ScriptRegion::Executable;
    implicitOrigin.legacyEncoding = "51";
    artifact.lockingScript.push_back(implicitOrigin);

    auto rewritten = BytecodeArtifactRewriter::rewrite(
        artifact, {"51", "00"}, {{0}, {}}
    );
    require(rewritten.has_value(), "synthetic typed rewrite failed");
    require(
        rewritten->lockingScript[0].id == 1 &&
            rewritten->lockingScript[0].origins ==
                std::vector<OriginId>{1} &&
            rewritten->lockingScript[1].id == 2 &&
            rewritten->lockingScript[1].origins ==
                std::vector<OriginId>{2},
        "synthetic instruction/origin allocators collided with implicit origins"
    );

    BytecodeArtifact independent;
    independent.lockingScript.push_back(typedInstruction(
        100,
        5,
        PushDataInstruction{{0x01}},
        ScriptRegion::Executable,
        "51"
    ));
    auto independentRewrite = BytecodeArtifactRewriter::rewrite(
        independent, {"51", "00"}, {{0}, {}}
    );
    require(
        independentRewrite.has_value() &&
            independentRewrite->lockingScript[1].id == 101 &&
            independentRewrite->lockingScript[1].origins ==
                std::vector<OriginId>{6},
        "instruction and origin namespaces did not allocate independently"
    );

    BytecodeArtifact exhausted = independent;
    exhausted.lockingScript[0].id =
        std::numeric_limits<InstructionId>::max();
    require(
        !BytecodeArtifactRewriter::rewrite(
             exhausted, {"51", "00"}, {{0}, {}}
         ).has_value(),
        "synthetic identity reused an old ID after UINT64_MAX"
    );
}

void testTypedArtifactRewritePlaceholderConflicts()
{
    BytecodeArtifact artifact;
    artifact.lockingScript = {
        typedInstruction(
            1,
            1,
            PlaceholderPushInstruction{"owner", size_t{20}},
            ScriptRegion::Executable,
            "<owner>"
        ),
        typedInstruction(
            2,
            2,
            PlaceholderPushInstruction{"owner", size_t{32}},
            ScriptRegion::Executable,
            "<owner>"
        ),
    };
    std::string error;
    require(
        !BytecodeArtifactRewriter::rewrite(
             artifact, {"<owner>", "<owner>"}, {{0}, {1}}, &error
         ).has_value() &&
            error.find("conflicting placeholder sizes") !=
                std::string::npos,
        "rewrite silently normalized conflicting placeholder schemas"
    );
}

void testSuffixTypedPushSurvivesExecutableRewrite()
{
    BytecodeArtifact artifact;
    artifact.format = ArtifactFormat::CanonicalV2;
    artifact.lockingScript = {
        typedInstruction(
            10,
            10,
            PushDataInstruction{{0xaa}},
            ScriptRegion::Executable,
            "01aa"
        ),
        typedInstruction(
            11,
            11,
            OpcodeInstruction{BytOpcode::OP_DUP},
            ScriptRegion::Executable,
            "76"
        ),
        typedInstruction(
            12,
            12,
            OpcodeInstruction{BytOpcode::OP_DROP},
            ScriptRegion::Executable,
            "75"
        ),
        typedInstruction(
            13,
            13,
            OpcodeInstruction{BytOpcode::OP_RETURN},
            ScriptRegion::Executable,
            "6a"
        ),
        typedInstruction(
            14,
            14,
            PushDataInstruction{{0x01}},
            ScriptRegion::ImmutableSuffix,
            "0101"
        ),
    };

    auto rewritten = BytecodeArtifactRewriter::rewrite(
        artifact,
        {"01aa", "6a", "0101"},
        {{0}, {3}, {4}}
    );
    require(rewritten.has_value(), "executable identity rewrite failed");
    const auto& suffix = rewritten->lockingScript.back();
    require(
        suffix.id == 14 && suffix.region == ScriptRegion::ImmutableSuffix &&
            std::holds_alternative<PushDataInstruction>(suffix.body),
        "unrelated executable rewrite demoted typed suffix PushData"
    );

    const auto before = BytecodeMaterializer::materialize(artifact, {});
    const auto after = BytecodeMaterializer::materialize(*rewritten, {});
    require(
        before.ok() && after.ok() &&
            before.value->atoms.back().encodedHex == "51" &&
            after.value->atoms.back().encodedHex ==
                before.value->atoms.back().encodedHex,
        "canonical suffix bytes changed after an executable-only rewrite"
    );
}

} // namespace

int main()
{
    try {
        testPrefixBoundaries();
        testCanonicalEncoding();
        testStrictDecoding();
        testLegacyHelperCompatibility();
        testTypedIrAndLosslessAdapter();
        testStrictLegacyFragmentSplitting();
        testTypedArtifactRewriteKeepsOrigins();
        testTypedArtifactRewriteIdentityAndRegions();
        testTypedArtifactRewriteSyntheticIdentity();
        testTypedArtifactRewritePlaceholderConflicts();
        testSuffixTypedPushSurvivesExecutableRewrite();
    } catch (const std::exception& error) {
        std::cerr << "script_codec_ir_test: " << error.what() << '\n';
        return 1;
    }

    std::cout << "script codec and typed IR tests passed\n";
    return 0;
}
