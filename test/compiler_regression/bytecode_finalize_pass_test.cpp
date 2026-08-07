#include "bytecode/bytecode_ir.h"
#include "bytecode/legacy_bytecode_adapter.h"
#include "bytecode_finalize_pass.h"
#include "debugger/info/debug_info.h"
#include "error/error_manager.h"
#include "log/logger.h"
#include "pass/pass_context.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef ENABLE_DEBUGGER
#error "bytecode_finalize_pass_test must exercise the debugger transaction"
#endif

namespace
{

using BytecodeData = std::pair<
    std::vector<std::string>,
    std::unordered_map<std::string, std::string>>;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool artifactsEqual(
    const tbc::BytecodeArtifact& left,
    const tbc::BytecodeArtifact& right
)
{
    return left.format == right.format &&
           left.lockingScript == right.lockingScript &&
           left.unlockingScripts == right.unlockingScripts &&
           left.constructorSchema == right.constructorSchema &&
           left.layout == right.layout;
}

std::shared_ptr<apc_debug::DebugInfo> makeDebugInfo(bool valid)
{
    auto debugInfo = std::make_shared<apc_debug::DebugInfo>();
    debugInfo->sourceFilename = valid ? "finalize_test.ct" : "";
    debugInfo->contractName = "FinalizeTest";
    debugInfo->version = "2.0";
    return debugInfo;
}

void setCommonContext(
    PassContext& context,
    const std::shared_ptr<BytecodeData>& bytecode,
    const std::shared_ptr<tbc::BytecodeArtifact>& artifact,
    const std::shared_ptr<apc_debug::DebugInfo>& debugInfo
)
{
    context.set("bytcode", bytecode);
    context.set("bytecode_artifact", artifact);
    context.set("debug_info", debugInfo);
    context.set(
        "self_placeholder_lengths",
        std::make_shared<std::unordered_map<std::string, size_t>>(
            std::unordered_map<std::string, size_t>{{"self.owner", 20}}
        )
    );
}

void runFinalize(PassContext& context)
{
    BytecodeFinalizePass pass;
    pass.execute(context);
}

void testArtifactSizeMismatchFailsClosedBeforePlaceholderCommit()
{
    auto bytecode = std::make_shared<BytecodeData>(BytecodeData{
        {"<self.owner>", "6a", "aabb"},
        {},
    });
    auto artifact = std::make_shared<tbc::BytecodeArtifact>(
        tbc::LegacyBytecodeAdapter::import(
            BytecodeData{{"<self.owner>", "6a"}, {}}
        )
    );
    auto debugInfo = makeDebugInfo(true);

    const BytecodeData originalBytecode = *bytecode;
    const tbc::BytecodeArtifact originalArtifact = *artifact;
    const std::string originalDebug = debugInfo->toJson();

    PassContext context;
    setCommonContext(context, bytecode, artifact, debugInfo);
    runFinalize(context);

    require(
        *bytecode == originalBytecode,
        "size mismatch committed placeholder normalization/padding"
    );
    require(
        artifactsEqual(*artifact, originalArtifact),
        "size mismatch mutated the typed artifact"
    );
    require(
        debugInfo->toJson() == originalDebug,
        "size mismatch mutated DebugInfo"
    );
}

void testDebugRemapFailureRollsBackEveryRepresentation()
{
    auto bytecode = std::make_shared<BytecodeData>(BytecodeData{
        {"<self.owner>", "6a", "aabb"},
        {},
    });
    auto artifact = std::make_shared<tbc::BytecodeArtifact>(
        tbc::LegacyBytecodeAdapter::import(*bytecode)
    );
    // An empty source filename makes DebugInfo::validate() fail after the
    // candidate bytecode and artifact remaps have already been computed.
    auto debugInfo = makeDebugInfo(false);

    const BytecodeData originalBytecode = *bytecode;
    const tbc::BytecodeArtifact originalArtifact = *artifact;
    const std::string originalDebug = debugInfo->toJson();

    PassContext context;
    setCommonContext(context, bytecode, artifact, debugInfo);
    runFinalize(context);

    require(
        *bytecode == originalBytecode,
        "DebugInfo failure leaked normalized placeholder/padding"
    );
    require(
        artifactsEqual(*artifact, originalArtifact),
        "DebugInfo failure committed the artifact candidate"
    );
    require(
        debugInfo->toJson() == originalDebug,
        "failed DebugInfo remap mutated its receiver"
    );
}

void testLegacyPlaceholderNormalizationCommitsAsOneTransaction()
{
    auto bytecode = std::make_shared<BytecodeData>(BytecodeData{
        {"<self.owner>", "6a", "aabb"},
        {},
    });
    auto artifact = std::make_shared<tbc::BytecodeArtifact>(
        tbc::LegacyBytecodeAdapter::import(*bytecode)
    );
    auto debugInfo = makeDebugInfo(true);

    PassContext context;
    setCommonContext(context, bytecode, artifact, debugInfo);
    runFinalize(context);

    require(bytecode->first.size() == 4, "Legacy padding was not inserted");
    require(
        bytecode->first[0] == "<self.owner20>",
        "fixed-size placeholder was not normalized"
    );
    require(
        bytecode->first[1] == "6a" && bytecode->first[3] == "aabb",
        "OP_RETURN or immutable suffix moved incorrectly"
    );
    require(
        bytecode->first[2].size() == 84 &&
            bytecode->first[2].starts_with("29"),
        "42-byte Legacy padding encoding is wrong"
    );

    const auto artifactView =
        tbc::LegacyBytecodeAdapter::exportPreserving(*artifact);
    require(
        artifactView == *bytecode,
        "typed artifact and Legacy view did not commit together"
    );
    require(
        artifact->lockingScript[2].region == tbc::ScriptRegion::Padding &&
            std::holds_alternative<tbc::PushDataInstruction>(
                artifact->lockingScript[2].body
            ),
        "synthetic padding was not recorded as typed PushData"
    );
    const auto* placeholder =
        std::get_if<tbc::PlaceholderPushInstruction>(
            &artifact->lockingScript[0].body
        );
    require(
        placeholder && placeholder->label == "self.owner20",
        "normalized placeholder was not reflected in typed IR"
    );
    require(debugInfo->validate(), "valid DebugInfo became invalid");
}

void testCanonicalConcreteAlwaysDefersMaterializationAndPadding()
{
    auto bytecode = std::make_shared<BytecodeData>(BytecodeData{
        {"0101", "6a"},
        {},
    });
    auto artifact = std::make_shared<tbc::BytecodeArtifact>(
        tbc::LegacyBytecodeAdapter::import(*bytecode)
    );
    artifact->format = tbc::ArtifactFormat::CanonicalV2;
    artifact->layout.executableAlignment.reset();
    artifact->layout.requiresMaterialization = false;
    auto debugInfo = makeDebugInfo(true);

    const BytecodeData originalBytecode = *bytecode;
    const auto originalInstructions = artifact->lockingScript;
    const std::string originalDebug = debugInfo->toJson();

    PassContext context;
    setCommonContext(context, bytecode, artifact, debugInfo);
    runFinalize(context);

    require(
        *bytecode == originalBytecode,
        "Canonical concrete script was pre-padded or canonicalized"
    );
    require(
        artifact->lockingScript == originalInstructions,
        "Canonical concrete instructions changed before materialization"
    );
    require(
        artifact->layout.executableAlignment == 64,
        "Canonical alignment directive was not recorded"
    );
    require(
        artifact->layout.requiresMaterialization,
        "concrete CanonicalV2 incorrectly bypassed materialization"
    );
    require(
        debugInfo->toJson() == originalDebug,
        "deferred Canonical layout mutated DebugInfo"
    );
}

void testTypedSuffixReturnByteAndRepeatedFinalizeAreStable()
{
    auto bytecode = std::make_shared<BytecodeData>(BytecodeData{
        {"6a", "51", "6a", "aabb"},
        {},
    });

    auto artifact = std::make_shared<tbc::BytecodeArtifact>();
    artifact->format = tbc::ArtifactFormat::LegacyV1;
    const auto makeInstruction = [](
        tbc::InstructionId id,
        tbc::InstructionBody body,
        tbc::ScriptRegion region,
        std::string encoding
    ) {
        tbc::BytecodeInstruction instruction;
        instruction.id = id;
        instruction.body = std::move(body);
        instruction.region = region;
        instruction.origins = {id};
        instruction.legacyEncoding = std::move(encoding);
        return instruction;
    };
    artifact->lockingScript = {
        makeInstruction(
            0,
            tbc::OpcodeInstruction{tbc::BytOpcode::OP_RETURN},
            tbc::ScriptRegion::Executable,
            "6a"
        ),
        makeInstruction(
            1,
            tbc::RawSuffixInstruction{"51"},
            tbc::ScriptRegion::ImmutableSuffix,
            "51"
        ),
        makeInstruction(
            2,
            tbc::RawSuffixInstruction{"6a"},
            tbc::ScriptRegion::ImmutableSuffix,
            "6a"
        ),
        makeInstruction(
            3,
            tbc::RawSuffixInstruction{"aabb"},
            tbc::ScriptRegion::ImmutableSuffix,
            "aabb"
        ),
    };
    auto debugInfo = makeDebugInfo(true);

    PassContext context;
    setCommonContext(context, bytecode, artifact, debugInfo);
    runFinalize(context);

    require(bytecode->first.size() == 5, "typed padding was not inserted");
    require(
        bytecode->first[0] == "6a" && bytecode->first[2] == "51" &&
            bytecode->first[3] == "6a" && bytecode->first[4] == "aabb",
        "RawSuffix OP_RETURN byte changed the typed executable boundary"
    );
    require(
        bytecode->first[1].size() == 126 &&
            bytecode->first[1].starts_with("3e"),
        "63-byte padding was not placed after the real typed OP_RETURN"
    );
    require(
        artifact->lockingScript[1].region == tbc::ScriptRegion::Padding &&
            std::holds_alternative<tbc::PushDataInstruction>(
                artifact->lockingScript[1].body
            ) &&
            artifact->lockingScript[2].region ==
                tbc::ScriptRegion::ImmutableSuffix &&
            artifact->lockingScript[3].region ==
                tbc::ScriptRegion::ImmutableSuffix,
        "typed regions were not preserved around padding"
    );

    const BytecodeData firstBytecode = *bytecode;
    const tbc::BytecodeArtifact firstArtifact = *artifact;
    const std::string firstDebug = debugInfo->toJson();
    runFinalize(context);

    require(
        *bytecode == firstBytecode,
        "running BytecodeFinalizePass twice changed bytecode"
    );
    require(
        artifactsEqual(*artifact, firstArtifact),
        "running BytecodeFinalizePass twice changed the typed artifact"
    );
    require(
        debugInfo->toJson() == firstDebug,
        "running BytecodeFinalizePass twice changed DebugInfo"
    );
}

void testNormalizedPlaceholderWhoseFieldEndsInDigitsRemainsResolvable()
{
    ErrorManager::getInstance().clear();
    auto bytecode = std::make_shared<BytecodeData>(BytecodeData{
        {"<self.foo2>", "6a"},
        {},
    });
    auto artifact = std::make_shared<tbc::BytecodeArtifact>(
        tbc::LegacyBytecodeAdapter::import(*bytecode)
    );
    auto debugInfo = makeDebugInfo(true);

    PassContext context;
    setCommonContext(context, bytecode, artifact, debugInfo);
    context.set(
        "self_placeholder_lengths",
        std::make_shared<std::unordered_map<std::string, size_t>>(
            std::unordered_map<std::string, size_t>{{"self.foo2", 20}}
        )
    );

    runFinalize(context);
    require(
        bytecode->first.front() == "<self.foo220>",
        "digit-ending field was not normalized with its fixed size"
    );
    const BytecodeData firstBytecode = *bytecode;
    const tbc::BytecodeArtifact firstArtifact = *artifact;

    runFinalize(context);
    require(
        !ErrorManager::getInstance().hasErrors(),
        "normalized digit-ending field lost its fixed-size declaration"
    );
    require(
        *bytecode == firstBytecode && artifactsEqual(*artifact, firstArtifact),
        "digit-ending normalized placeholder was not finalize-idempotent"
    );
}

} // namespace

int main()
{
    Logger::GetInstance().SetLogLevel(LogLevel::NONE);
    ErrorManager::getInstance().setColorOutput(false);
    ErrorManager::getInstance().setShowContext(false);

    try {
        testArtifactSizeMismatchFailsClosedBeforePlaceholderCommit();
        testDebugRemapFailureRollsBackEveryRepresentation();
        testLegacyPlaceholderNormalizationCommitsAsOneTransaction();
        testCanonicalConcreteAlwaysDefersMaterializationAndPadding();
        testTypedSuffixReturnByteAndRepeatedFinalizeAreStable();
        testNormalizedPlaceholderWhoseFieldEndsInDigitsRemainsResolvable();
    } catch (const std::exception& error) {
        std::cerr << "bytecode_finalize_pass_test: " << error.what() << '\n';
        return 1;
    }

    ErrorManager::getInstance().clear();
    std::cout << "bytecode_finalize_pass_test: all checks passed\n";
    return 0;
}
