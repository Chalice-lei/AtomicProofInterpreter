#include "bytecode/bytecode_materializer.h"
#include "bytecode/bytecode_generator.h"
#include "bytecode/legacy_bytecode_adapter.h"
#include "compiler/bytecode_pipeline.h"
#include "compiler/compiler_driver.h"
#include "config/config_manager.h"
#include "error/error_manager.h"
#include "pass/pass_context_keys.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

PassContext compile(const std::string& format)
{
    static const std::string source =
        "Contract TypedPipeline:\n"
        "    def main():\n"
        "        Return(1)\n";

    ErrorManager::getInstance().clear();
    ErrorManager::getInstance().setSourceContent("typed_pipeline.ct", source);

    apc_compiler::BytecodePipelineOptions options;
    options.exportResults = false;
    options.codeFileName = "typed_pipeline";
    options.artifactFormat = format;
    return apc_compiler::runBytecodePipeline(
        "typed_pipeline.ct", source, options
    );
}

void verifyGeneratorClearResetsBothViews()
{
    tbc::BytecodeGenerator generator;
    generator.emit(tbc::BytOpcode::OP_1);
    generator.mergeSubOverall();
    generator.emitUnlockName("old");
    generator.emitUnlock("<value>");
    generator.mergeSubUnoverall();

    generator.clear();
    generator.emit(tbc::BytOpcode::OP_0);
    generator.mergeSubOverall();

    const auto legacy = generator.instructions();
    const auto artifact = generator.artifact();
    require(
        legacy.first == std::vector<std::string>{"00"} &&
            legacy.second.empty(),
        "BytecodeGenerator::clear retained a previous compile artifact"
    );
    require(
        tbc::LegacyBytecodeAdapter::exportPreserving(artifact) == legacy &&
            artifact.lockingScript.front().id == 0,
        "BytecodeGenerator::clear desynchronized or failed to reset typed IR"
    );
}

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        const auto seed = std::chrono::high_resolution_clock::now()
                              .time_since_epoch()
                              .count();
        path = std::filesystem::temp_directory_path() /
               ("apc_typed_pipeline_" + std::to_string(seed));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    std::filesystem::path path;
};

PassContext compileFixedSelf(
    const std::filesystem::path& outputBase,
    bool exportResults
)
{
    static const std::string source =
        "Contract FixedSelf:\n"
        "    def __init__(ownerInput: hex20):\n"
        "        self.owner = ownerInput\n"
        "\n"
        "    def main(candidate: hex20):\n"
        "        EqualVerify(candidate, self.owner)\n"
        "        Return(1)\n"
        "        Push(self.owner)\n";

    ErrorManager::getInstance().clear();
    ErrorManager::getInstance().setSourceContent("fixed_self.ct", source);

    apc_compiler::BytecodePipelineOptions options;
    options.exportResults = exportResults;
    options.codeFileName = outputBase.string();
    options.artifactFormat = "canonical_v2";
    return apc_compiler::runBytecodePipeline(
        "fixed_self.ct", source, options
    );
}

void verifyLegacy()
{
    auto context = compile("legacy_v1");
    require(
        !ErrorManager::getInstance().hasErrors(),
        "LegacyV1 pipeline reported errors"
    );
    auto artifact = context.tryGet<tbc::BytecodeArtifact>(
        apc_pipeline::key::kBytecodeArtifact
    );
    auto legacy = context.tryGet<apc_pipeline::BytecodeOutput>(
        apc_pipeline::key::kBytecode
    );
    require(artifact && legacy, "pipeline did not publish both bytecode views");
    require(
        artifact->format == tbc::ArtifactFormat::LegacyV1,
        "legacy request produced the wrong artifact format"
    );
    require(
        tbc::LegacyBytecodeAdapter::exportPreserving(*artifact) == *legacy,
        "typed and legacy views diverged"
    );
}

void verifyCanonical()
{
    auto context = compile("canonical_v2");
    require(
        !ErrorManager::getInstance().hasErrors(),
        "CanonicalV2 pipeline reported errors"
    );
    auto artifact = context.tryGet<tbc::BytecodeArtifact>(
        apc_pipeline::key::kBytecodeArtifact
    );
    require(artifact != nullptr, "CanonicalV2 artifact was not published");
    require(
        artifact->format == tbc::ArtifactFormat::CanonicalV2,
        "canonical request produced the wrong artifact format"
    );
    require(
        artifact->layout.executableAlignment == 64 &&
            artifact->layout.requiresMaterialization,
        "canonical final layout was not deferred to materialization"
    );

    auto materialized = tbc::BytecodeMaterializer::materialize(
        *artifact, tbc::PlaceholderBindings{}
    );
    require(materialized.ok(), "concrete canonical artifact did not materialize");
    require(
        (materialized.value->finalHex.size() / 2) % 64 == 0,
        "canonical executable was not aligned to 64 bytes"
    );
}

void verifyUnknownFormatIsRejected()
{
    auto context = compile("future_v3");
    require(
        ErrorManager::getInstance().hasErrors(),
        "unknown artifact format silently fell back to LegacyV1"
    );
    require(
        !context.tryGet<tbc::BytecodeArtifact>(
            apc_pipeline::key::kBytecodeArtifact
        ),
        "unknown artifact format published a bytecode artifact"
    );
}

void verifyFixedSelfCanonicalTemplate()
{
    const TemporaryDirectory directory;
    const auto outputBase = directory.path / "fixed_self";
    auto context = compileFixedSelf(outputBase, true);
    require(
        !ErrorManager::getInstance().hasErrors(),
        "fixed self CanonicalV2 pipeline reported errors"
    );

    auto artifact = context.tryGet<tbc::BytecodeArtifact>(
        apc_pipeline::key::kBytecodeArtifact
    );
    auto legacy = context.tryGet<apc_pipeline::BytecodeOutput>(
        apc_pipeline::key::kBytecode
    );
    require(artifact && legacy, "fixed self pipeline lost a bytecode view");
    require(
        tbc::LegacyBytecodeAdapter::exportPreserving(*artifact) == *legacy,
        "fixed self typed and legacy views diverged"
    );
    require(
        artifact->constructorSchema.fields.size() == 1,
        "constructor schema did not preserve its only field"
    );
    const auto& field = artifact->constructorSchema.fields.front();
    require(
        field.name == "ownerInput" && field.type == "hex20" &&
            field.fixedPayloadSize == size_t{20},
        "constructor schema lost the fixed hex20 payload length"
    );

    const tbc::PlaceholderPushInstruction* selfPlaceholder = nullptr;
    size_t selfPlaceholderCount = 0;
    bool hasExecutableSelf = false;
    bool hasSuffixSelf = false;
    for (const auto& instruction : artifact->lockingScript) {
        const auto* candidate =
            std::get_if<tbc::PlaceholderPushInstruction>(
                &instruction.body
            );
        if (candidate && candidate->label.starts_with("self.owner")) {
            if (!selfPlaceholder) {
                selfPlaceholder = candidate;
            }
            ++selfPlaceholderCount;
            hasExecutableSelf =
                hasExecutableSelf ||
                instruction.region == tbc::ScriptRegion::Executable;
            hasSuffixSelf =
                hasSuffixSelf ||
                instruction.region == tbc::ScriptRegion::ImmutableSuffix;
            require(
                candidate->label == selfPlaceholder->label &&
                    candidate->expectedPayloadSize == size_t{20},
                "self.owner relocations disagree on label or fixed size"
            );
        }
    }
    require(selfPlaceholder != nullptr, "self.owner relocation is missing");
    require(
        selfPlaceholderCount == 2 && hasExecutableSelf && hasSuffixSelf,
        "self.owner was not represented in both CanonicalV2 regions"
    );

    tbc::PlaceholderBindings bindings;
    bindings.emplace(
        selfPlaceholder->label, std::vector<uint8_t>(20, uint8_t{0x11})
    );
    const auto materialized =
        tbc::BytecodeMaterializer::materialize(*artifact, bindings);
    require(
        materialized.ok(),
        "fixed self CanonicalV2 template did not materialize"
    );
    require(
        materialized.value->executableBytesBeforePadding +
                materialized.value->paddingBytes ==
            64,
        "fixed self CanonicalV2 executable was not aligned after binding"
    );
    require(
        materialized.value->immutableSuffixBytes == 21,
        "fixed self suffix changed the executable alignment boundary"
    );

    bindings[selfPlaceholder->label].resize(19);
    const auto wrongSize =
        tbc::BytecodeMaterializer::materialize(*artifact, bindings);
    require(
        !wrongSize.ok() &&
            wrongSize.error.code ==
                tbc::MaterializationErrorCode::PayloadSizeMismatch,
        "fixed self relocation accepted a non-hex20 payload"
    );

    const auto outputPath =
        std::filesystem::path(outputBase.string() + ".json");
    std::ifstream input(outputPath);
    require(input.is_open(), "CanonicalV2 export file is missing");
    nlohmann::ordered_json exported;
    input >> exported;
    const auto& lock = exported["lock"];
    require(
        lock["format"] == "canonical_v2" &&
            lock.contains("template") && lock.contains("relocations") &&
            !lock.contains("hex") && !lock.contains("asm"),
        "unresolved fixed self export is not a CanonicalV2 template"
    );
    require(
        lock["relocations"].size() == 2 &&
            lock["relocations"][0]["label"] == selfPlaceholder->label &&
            lock["relocations"][0]["expectedPayloadSize"] == 20 &&
            lock["relocations"][1]["label"] == selfPlaceholder->label &&
            lock["relocations"][1]["expectedPayloadSize"] == 20,
        "fixed self export relocation is incomplete"
    );
}

void verifyConfiguredCanonicalAndDriverLegacyBoundary()
{
    const TemporaryDirectory directory;
    const auto configPath = directory.path / "user_preferences.json";
    {
        std::ofstream output(configPath);
        require(output.is_open(), "failed to create artifact format config");
        output << R"({"output":{"bytecode_format":"canonical_v2"}})";
        require(
            static_cast<bool>(output),
            "failed to write artifact format config"
        );
    }
    require(
        ConfigManager::getInstance().initialize(configPath.string()),
        "failed to initialize artifact format config"
    );

    auto configured = compile("");
    require(
        !ErrorManager::getInstance().hasErrors(),
        "configured CanonicalV2 pipeline reported errors"
    );
    auto configuredArtifact = configured.tryGet<tbc::BytecodeArtifact>(
        apc_pipeline::key::kBytecodeArtifact
    );
    require(
        configuredArtifact &&
            configuredArtifact->format ==
                tbc::ArtifactFormat::CanonicalV2,
        "ConfigManager artifact format was ignored"
    );

    auto explicitLegacy = compile("LeGaCy");
    auto explicitArtifact = explicitLegacy.tryGet<tbc::BytecodeArtifact>(
        apc_pipeline::key::kBytecodeArtifact
    );
    require(
        !ErrorManager::getInstance().hasErrors() && explicitArtifact &&
            explicitArtifact->format == tbc::ArtifactFormat::LegacyV1,
        "explicit pipeline format did not override ConfigManager"
    );

    static const std::string source =
        "Contract DriverBoundary:\n"
        "    def main():\n"
        "        Return(1)\n";
    apc::CompilerOptions options;
    options.exportResults = true;
    options.colorDiagnostics = false;
    options.showDiagnosticContext = false;
    options.codeFileName = (directory.path / "driver_boundary").string();
    const auto result = apc::CompilerDriver::compileSource(
        "driver_boundary.ct", source, options
    );
    require(result.success, "CompilerDriver legacy boundary failed to compile");

    std::ifstream input(options.codeFileName + ".json");
    require(input.is_open(), "CompilerDriver export file is missing");
    nlohmann::ordered_json exported;
    input >> exported;
    require(
        exported.contains("lock") && exported["lock"].contains("hex") &&
            !exported["lock"].contains("format") &&
            !exported["lock"].contains("template"),
        "CompilerDriver leaked configured CanonicalV2 into a legacy consumer"
    );
}

} // namespace

int main()
{
    try {
        verifyGeneratorClearResetsBothViews();
        verifyLegacy();
        verifyCanonical();
        verifyUnknownFormatIsRejected();
        verifyFixedSelfCanonicalTemplate();
        verifyConfiguredCanonicalAndDriverLegacyBoundary();
        std::cout << "typed_bytecode_pipeline_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "typed_bytecode_pipeline_test: " << error.what() << '\n';
        return 1;
    }
}
