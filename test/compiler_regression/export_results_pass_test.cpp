#include "bytecode/bytecode_ir.h"
#include "bytecode/legacy_bytecode_adapter.h"
#include "error/error_manager.h"
#include "export_results_pass.h"
#include "log/logger.h"
#include "pass/pass_context.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

using LegacyBytecode = std::pair<
    std::vector<std::string>,
    std::unordered_map<std::string, std::string>>;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
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
               ("apc_export_results_pass_" + std::to_string(seed));
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

tbc::BytecodeInstruction makeInstruction(
    tbc::InstructionId id,
    tbc::InstructionBody body,
    tbc::ScriptRegion region = tbc::ScriptRegion::Executable
)
{
    tbc::BytecodeInstruction instruction;
    instruction.id = id;
    instruction.body = std::move(body);
    instruction.region = region;
    instruction.origins.push_back(id + 100);
    return instruction;
}

tbc::BytecodeArtifact canonicalArtifact(
    std::vector<tbc::BytecodeInstruction> instructions,
    std::optional<size_t> alignment
)
{
    tbc::BytecodeArtifact artifact;
    artifact.format = tbc::ArtifactFormat::CanonicalV2;
    artifact.lockingScript = std::move(instructions);
    artifact.layout.executableAlignment = alignment;
    return artifact;
}

void setCommonContext(
    PassContext& context,
    const std::filesystem::path& outputBase
)
{
    context.set(
        "code_file_name",
        std::make_shared<std::string>(outputBase.string())
    );
    const char* jsonKeys[] = {
        "structs",
        "abi",
        "unlock",
        "constructorParams",
        "all_functions",
    };
    for (const char* key : jsonKeys) {
        context.set(
            key,
            std::make_shared<nlohmann::ordered_json>(
                nlohmann::ordered_json::object()
            )
        );
    }
}

std::filesystem::path outputPath(const std::filesystem::path& base)
{
    return std::filesystem::path(base.string() + ".json");
}

nlohmann::ordered_json readJson(const std::filesystem::path& path)
{
    std::ifstream input(path);
    require(input.is_open(), "failed to open exported JSON: " + path.string());
    nlohmann::ordered_json json;
    input >> json;
    return json;
}

void runExport(PassContext& context)
{
    ExportResultsPass pass;
    pass.execute(context);
}

void testLegacyArtifactIsByteCompatibleAndAuthoritative(
    const TemporaryDirectory& directory
)
{
    const LegacyBytecode legacy{
        {"51", "# ignored", "0100", "6a"},
        {},
    };

    ErrorManager::getInstance().clear();
    const auto fallbackBase = directory.path / "legacy_fallback";
    PassContext fallback;
    setCommonContext(fallback, fallbackBase);
    fallback.set("bytcode", std::make_shared<LegacyBytecode>(legacy));
    runExport(fallback);
    require(
        !ErrorManager::getInstance().hasErrors(),
        "legacy fallback export reported an error"
    );
    const auto fallbackJson = readJson(outputPath(fallbackBase));

    ErrorManager::getInstance().clear();
    const auto artifactBase = directory.path / "legacy_artifact";
    PassContext typed;
    setCommonContext(typed, artifactBase);
    auto artifact = tbc::LegacyBytecodeAdapter::import(legacy);
    typed.set(
        "bytecode_artifact",
        std::make_shared<tbc::BytecodeArtifact>(std::move(artifact))
    );
    // A stale compatibility view must not override the typed artifact.
    typed.set(
        "bytcode",
        std::make_shared<LegacyBytecode>(
            LegacyBytecode{{"00"}, {}}
        )
    );
    runExport(typed);
    require(
        !ErrorManager::getInstance().hasErrors(),
        "legacy typed export reported an error"
    );
    const auto typedJson = readJson(outputPath(artifactBase));

    require(
        fallbackJson["lock"] == typedJson["lock"],
        "LegacyV1 lock output changed at the typed boundary"
    );
    require(
        typedJson["lock"] == nlohmann::ordered_json{
            // Interpreter's historical disassembler preserves the direct
            // push-length byte in its ASM token. LegacyV1 must retain that
            // schema even though Compiler's newer decoder prints only the
            // payload.
            {"asm", "OP_1 0100 OP_RETURN"},
            {"hex", "5101006a"},
        },
        "LegacyV1 asm/hex bytes differ from the historical schema"
    );
    require(
        !typedJson["lock"].contains("format") &&
            !typedJson["lock"].contains("template") &&
            !typedJson["lock"].contains("relocations"),
        "LegacyV1 lock object acquired CanonicalV2 fields"
    );
}

void testCanonicalUnresolvedExportsOnlyStructuredTemplate(
    const TemporaryDirectory& directory
)
{
    auto artifact = canonicalArtifact(
        {
            makeInstruction(
                10,
                tbc::PlaceholderPushInstruction{"owner", size_t{1}}
            ),
            makeInstruction(
                11,
                tbc::OpcodeInstruction{tbc::BytOpcode::OP_RETURN}
            ),
            makeInstruction(
                12,
                tbc::RawSuffixInstruction{"aabb"},
                tbc::ScriptRegion::ImmutableSuffix
            ),
        },
        64
    );

    ErrorManager::getInstance().clear();
    const auto base = directory.path / "canonical_template";
    PassContext context;
    setCommonContext(context, base);
    context.set(
        "bytecode_artifact",
        std::make_shared<tbc::BytecodeArtifact>(std::move(artifact))
    );
    runExport(context);

    require(
        !ErrorManager::getInstance().hasErrors(),
        "valid CanonicalV2 template reported an error"
    );
    const auto json = readJson(outputPath(base));
    const auto& lock = json["lock"];
    require(lock["format"] == "canonical_v2", "canonical format missing");
    require(lock["alignment"] == 64, "canonical alignment missing");
    require(lock.contains("template"), "structured template missing");
    require(lock.contains("relocations"), "relocations missing");
    require(
        !lock.contains("hex") && !lock.contains("asm"),
        "unresolved template incorrectly claimed final bytecode"
    );

    const auto& instructions = lock["template"];
    require(instructions.is_array(), "lock.template is not an array");
    require(instructions.size() == 3, "template instruction was lost");
    require(
        instructions[0]["id"] == 10 &&
            instructions[0]["kind"] == "placeholder_push" &&
            instructions[0]["region"] == "executable" &&
            instructions[0]["label"] == "owner" &&
            instructions[0]["expectedPayloadSize"] == 1 &&
            instructions[0]["origins"] ==
                nlohmann::ordered_json::array({110}),
        "placeholder instruction is not reconstructible"
    );
    require(
        instructions[1]["kind"] == "opcode" &&
            instructions[1]["opcode"] == "OP_RETURN" &&
            instructions[1]["hex"] == "6a",
        "opcode template content is incomplete"
    );
    require(
        instructions[2]["kind"] == "raw_suffix" &&
            instructions[2]["region"] == "immutable_suffix" &&
            instructions[2]["rawHex"] == "aabb",
        "raw suffix template content is incomplete"
    );

    const auto& relocations = lock["relocations"];
    require(relocations.size() == 1, "wrong relocation count");
    require(
        relocations[0]["instructionId"] == 10 &&
            relocations[0]["instructionIndex"] == 0 &&
            relocations[0]["label"] == "owner" &&
            relocations[0]["expectedPayloadSize"] == 1 &&
            relocations[0]["region"] == "executable",
        "relocation descriptor is incomplete"
    );
}

void testCanonicalConcreteExportsFinalBytecode(
    const TemporaryDirectory& directory
)
{
    auto artifact = canonicalArtifact(
        {
            makeInstruction(20, tbc::PushDataInstruction{{0x01}}),
            makeInstruction(
                21,
                tbc::OpcodeInstruction{tbc::BytOpcode::OP_RETURN}
            ),
        },
        4
    );

    ErrorManager::getInstance().clear();
    const auto base = directory.path / "canonical_concrete";
    PassContext context;
    setCommonContext(context, base);
    context.set(
        "bytecode_artifact",
        std::make_shared<tbc::BytecodeArtifact>(std::move(artifact))
    );
    runExport(context);

    require(
        !ErrorManager::getInstance().hasErrors(),
        "concrete CanonicalV2 export reported an error"
    );
    const auto json = readJson(outputPath(base));
    const auto& lock = json["lock"];
    require(lock["format"] == "canonical_v2", "canonical format missing");
    require(lock["alignment"] == 4, "concrete alignment missing");
    require(lock["hex"] == "516a01ff", "canonical final hex is wrong");
    require(
        lock["asm"] == "OP_1 OP_RETURN 01ff",
        "canonical final asm is wrong"
    );
    require(
        lock["relocations"].empty(),
        "concrete script exported relocations"
    );
    require(
        !lock.contains("template"),
        "concrete script was exported as an unresolved template"
    );
}

void testInvalidTemplateReportsErrorAndWritesNothing(
    const TemporaryDirectory& directory
)
{
    auto artifact = canonicalArtifact(
        {
            makeInstruction(
                30,
                tbc::PlaceholderPushInstruction{"owner", size_t{1}}
            ),
            makeInstruction(
                31,
                tbc::LegacyBarrierInstruction{"not-hex"}
            ),
            makeInstruction(
                32,
                tbc::OpcodeInstruction{tbc::BytOpcode::OP_RETURN}
            ),
        },
        64
    );

    ErrorManager::getInstance().clear();
    const auto base = directory.path / "invalid_template";
    const auto finalPath = outputPath(base);
    const auto temporaryPath =
        std::filesystem::path(finalPath.string() + ".tmp");
    PassContext context;
    setCommonContext(context, base);
    context.set(
        "bytecode_artifact",
        std::make_shared<tbc::BytecodeArtifact>(std::move(artifact))
    );
    runExport(context);

    require(
        ErrorManager::getInstance().hasErrors(),
        "invalid template did not reach ErrorManager"
    );
    require(
        !std::filesystem::exists(finalPath),
        "invalid template wrote a final JSON file"
    );
    require(
        !std::filesystem::exists(temporaryPath),
        "invalid template left a partial temporary file"
    );
}

void testDuplicateInstructionIdentityIsNotExported(
    const TemporaryDirectory& directory
)
{
    auto artifact = canonicalArtifact(
        {
            makeInstruction(
                40,
                tbc::PlaceholderPushInstruction{"owner", size_t{1}}
            ),
            makeInstruction(
                40,
                tbc::OpcodeInstruction{tbc::BytOpcode::OP_RETURN}
            ),
        },
        64
    );

    ErrorManager::getInstance().clear();
    const auto base = directory.path / "duplicate_instruction_id";
    PassContext context;
    setCommonContext(context, base);
    context.set(
        "bytecode_artifact",
        std::make_shared<tbc::BytecodeArtifact>(std::move(artifact))
    );
    runExport(context);

    require(
        ErrorManager::getInstance().hasErrors(),
        "duplicate CanonicalV2 instruction ID was exported"
    );
    require(
        !std::filesystem::exists(outputPath(base)),
        "duplicate instruction identity wrote a final JSON file"
    );
}

} // namespace

int main()
{
    Logger::GetInstance().SetLogLevel(LogLevel::NONE);
    ErrorManager::getInstance().setColorOutput(false);
    ErrorManager::getInstance().setShowContext(false);

    try {
        const TemporaryDirectory directory;
        testLegacyArtifactIsByteCompatibleAndAuthoritative(directory);
        testCanonicalUnresolvedExportsOnlyStructuredTemplate(directory);
        testCanonicalConcreteExportsFinalBytecode(directory);
        testInvalidTemplateReportsErrorAndWritesNothing(directory);
        testDuplicateInstructionIdentityIsNotExported(directory);
    } catch (const std::exception& error) {
        std::cerr << "export_results_pass_test: " << error.what() << '\n';
        return 1;
    }

    ErrorManager::getInstance().clear();
    std::cout << "export_results_pass_test: all checks passed\n";
    return 0;
}
