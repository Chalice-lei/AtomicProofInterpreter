#ifndef EXPORT_RESULTS_H
#define EXPORT_RESULTS_H

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "bytecode/bytecode_materializer.h"
#include "bytecode/legacy_bytecode_adapter.h"
#include "bytecode/script_decoder.h"
#include "bytecode/script_codec.h"
#include "config/config_manager.h"
#include "error/error_manager.h"
#include "include/pass_type.h"
#include "log/logger.h"
#include "pass/pass.h"
#include "pass/pass_context.h"
#include "pass/pass_context_keys.h"
#include "pass/pass_macros.h"

class ExportResultsPass : public Pass
{
    DECLARE_PASS(ExportResultsPass)
public:
    void execute(PassContext& data) override
    {
        // 有错误则跳过, 避免写出残缺产物
        if (ErrorManager::getInstance().hasErrors()) {
            LOG_INFO(
                "ExportResultsPass::execute - Skipped due to previous "
                "compilation errors"
            );
            return;
        }

        auto artifact = data.tryGet<tbc::BytecodeArtifact>(
            apc_pipeline::key::kBytecodeArtifact
        );
        auto legacyBytecode = data.tryGet<LegacyBytecode>(
            apc_pipeline::key::kBytecode
        );

        try {
            std::string resultFileName{"clean_script.json"};
            auto codefileNamePtr = data.get<std::string>(
                apc_pipeline::key::kCodeFileName
            );
            if (codefileNamePtr && !codefileNamePtr->empty()) {
                resultFileName = *codefileNamePtr + ".json";
            }

            // Resolve and validate the complete lock object before opening an
            // output file. This keeps a failed CanonicalV2 template or
            // materialization from leaving a partial JSON artifact.
            nlohmann::ordered_json lockData;
            if (!buildLockData(artifact, legacyBytecode, lockData)) {
                return;
            }

            // ordered_json 保证字段顺序
            nlohmann::ordered_json resultJson;

            // 1. metadata
            std::string sourceFileName = codefileNamePtr ? *codefileNamePtr
                                                         : "unknown";
            if (ConfigManager::getInstance().isInitialized()) {
                resultJson["metadata"] = ConfigManager::getInstance()
                                             .generateMetadata(sourceFileName);
            } else {
                // 回退到简单元数据
                std::string version = "1.0.0";
                try {
                    version = ConfigManager::getInstance().getCompilerVersion();
                } catch (const std::exception& e) {
                    LOG_DEBUG(
                        "Failed to get compiler version from ConfigManager: ",
                        e.what()
                    );
                }

                resultJson["metadata"] = nlohmann::ordered_json{
                    {"compiler_version", version},
                    {"generated_time", std::time(nullptr)},
                    {"source_file", sourceFileName}
                };
                LOG_WARNING(
                    "ConfigManager not initialized, using basic "
                    "metadata with version: ",
                    version
                );
            }

            // 2. structs (置于 ABI 前)
            auto structs = data.get<nlohmann::ordered_json>(
                apc_pipeline::key::kStructs
            );
            if (structs && !structs->empty()) {
                resultJson["structs"] = nlohmann::ordered_json::array();
                resultJson["structs"] = *structs;
                LOG_INFO("Struct definitions added to result JSON");
            }

            // 3. abi
            auto abi = data.get<nlohmann::ordered_json>(
                apc_pipeline::key::kAbi
            );
            if (abi && !abi->empty()) {
                resultJson["abi"] = nlohmann::ordered_json::array();
                resultJson["abi"] = *abi;
                LOG_INFO("ABI data added to result JSON");
            } else {
                LOG_WARNING(
                    "ABI data not added: ",
                    abi ? "empty ABI object" : "null ABI pointer"
                );
            }

            // 4. lock. LegacyV1 keeps the historical asm/hex object exactly;
            // CanonicalV2 additionally carries an explicit format/layout or a
            // structured unresolved template.
            resultJson["lock"] = lockData;

            // 5. unlock
            auto unlock = data.get<nlohmann::ordered_json>(
                apc_pipeline::key::kUnlock
            );
            if (unlock && !unlock->empty()) {
                resultJson["unlock"] = *unlock;
                LOG_INFO("Unlock data added to result JSON");
            } else {
                LOG_WARNING(
                    "Unlock data not added: ",
                    unlock ? "empty unlock object" : "null unlock pointer"
                );
            }

            // 6. constructorParams
            auto constructorParams = data.get<nlohmann::ordered_json>(
                apc_pipeline::key::kConstructorParams
            );
            if (constructorParams && !constructorParams->empty()) {
                resultJson["constructorParams"] =
                    nlohmann::ordered_json::array();
                resultJson["constructorParams"] = *constructorParams;
                LOG_INFO("ConstructorParams data added to result JSON");
            } else {
                LOG_WARNING(
                    "ConstructorParams data not added: ",
                    constructorParams ? "empty constructorParams object"
                                      : "null constructorParams pointer"
                );
            }

            // 7. functions: 全部函数信息 (含私有函数, 供调试器使用)
            auto allFunctions = data.get<nlohmann::ordered_json>(
                apc_pipeline::key::kAllFunctions
            );
            if (allFunctions && !allFunctions->empty()) {
                resultJson["functions"] = nlohmann::ordered_json::array();
                resultJson["functions"] = *allFunctions;
                LOG_INFO("All functions data added to result JSON");
            } else {
                LOG_WARNING(
                    "All functions data not added: ",
                    allFunctions ? "empty functions object" : "null functions pointer"
                );
            }

            const std::string serialized = resultJson.dump(2);
            if (writeAtomically(resultFileName, serialized)) {
                LOG_INFO("Clean script saved to ", resultFileName);
            }
        } catch (const std::out_of_range& e) {
            reportExportError(
                std::string(
                    "out of range error while exporting bytecode: "
                ) + e.what(),
                ErrorCategory::INTERNAL
            );
        } catch (const std::exception& e) {
            reportExportError(
                std::string("exception while exporting bytecode: ") +
                    e.what(),
                ErrorCategory::INTERNAL
            );
        } catch (...) {
            reportExportError(
                "unknown exception while exporting bytecode",
                ErrorCategory::INTERNAL
            );
        }
    }

    std::vector<std::string> getDependencies() const override
    {
        return {DependTypeToString(DependType::BytecodeFinalizePass)};
    }

private:
    using LegacyBytecode = apc_pipeline::BytecodeOutput;

    static const char* formatName(tbc::ArtifactFormat format) noexcept
    {
        switch (format) {
            case tbc::ArtifactFormat::LegacyV1:
                return "legacy_v1";
            case tbc::ArtifactFormat::CanonicalV2:
                return "canonical_v2";
        }
        return "unknown";
    }

    static const char* regionName(tbc::ScriptRegion region) noexcept
    {
        switch (region) {
            case tbc::ScriptRegion::Executable:
                return "executable";
            case tbc::ScriptRegion::Padding:
                return "padding";
            case tbc::ScriptRegion::ImmutableSuffix:
                return "immutable_suffix";
        }
        return "unknown";
    }

    static void reportExportError(
        const std::string& message,
        ErrorCategory category = ErrorCategory::SEMANTIC,
        const std::string& suggestion =
            "Fix the bytecode artifact before exporting"
    )
    {
        LOG_ERROR("ExportResultsPass::execute - ", message);
        ErrorManager::getInstance().reportError(
            ErrorSeverity::ERROR,
            category,
            message,
            SourceLocation("", 0, 0),
            suggestion
        );
    }

    static void reportMaterializationError(
        const tbc::MaterializationError& error,
        const std::string& phase
    )
    {
        std::ostringstream message;
        message << "CanonicalV2 " << phase << " failed: "
                << tbc::BytecodeMaterializer::errorName(error.code);
        if (error.instructionIndex.has_value()) {
            message << " at instruction index " << *error.instructionIndex;
        }
        if (error.instructionId.has_value()) {
            message << " (id " << *error.instructionId << ")";
        }
        if (!error.label.empty()) {
            message << " for placeholder '" << error.label << "'";
        }
        if (!error.message.empty()) {
            message << ": " << error.message;
        }
        reportExportError(message.str());
    }

    static nlohmann::ordered_json relocationJson(
        const tbc::TemplateRelocation& relocation
    )
    {
        nlohmann::ordered_json json{
            {"instructionId", relocation.instructionId},
            {"instructionIndex", relocation.instructionIndex},
            {"label", relocation.label},
            {"region", regionName(relocation.region)},
        };
        if (relocation.expectedPayloadSize.has_value()) {
            json["expectedPayloadSize"] =
                *relocation.expectedPayloadSize;
        } else {
            json["expectedPayloadSize"] = nullptr;
        }
        return json;
    }

    static nlohmann::ordered_json instructionJson(
        const tbc::BytecodeInstruction& instruction
    )
    {
        nlohmann::ordered_json json{
            {"id", instruction.id},
            {"region", regionName(instruction.region)},
            {"origins", instruction.origins},
        };

        if (const auto* opcode =
                std::get_if<tbc::OpcodeInstruction>(&instruction.body)) {
            json["kind"] = "opcode";
            json["opcode"] = tbc::OpcodeMapper::toString(opcode->opcode);
            json["hex"] = tbc::opcodeToHex(opcode->opcode);
        } else if (const auto* push =
                       std::get_if<tbc::PushDataInstruction>(
                           &instruction.body
                       )) {
            json["kind"] = "push_data";
            json["payloadHex"] =
                tbc::ScriptCodec::bytesToHex(push->payload);
            json["payloadSize"] = push->payload.size();
        } else if (const auto* placeholder =
                       std::get_if<tbc::PlaceholderPushInstruction>(
                           &instruction.body
                       )) {
            json["kind"] = "placeholder_push";
            json["label"] = placeholder->label;
            if (placeholder->expectedPayloadSize.has_value()) {
                json["expectedPayloadSize"] =
                    *placeholder->expectedPayloadSize;
            } else {
                json["expectedPayloadSize"] = nullptr;
            }
        } else if (const auto* raw =
                       std::get_if<tbc::RawSuffixInstruction>(
                           &instruction.body
                       )) {
            json["kind"] = "raw_suffix";
            json["rawHex"] = raw->raw;
        } else {
            const auto& barrier =
                std::get<tbc::LegacyBarrierInstruction>(instruction.body);
            json["kind"] = "legacy_barrier";
            json["raw"] = barrier.raw;
        }
        return json;
    }

    static void addCanonicalLayout(
        const tbc::BytecodeArtifact& artifact,
        nlohmann::ordered_json& lockData
    )
    {
        lockData["format"] = formatName(artifact.format);
        if (artifact.layout.executableAlignment.has_value()) {
            lockData["alignment"] =
                *artifact.layout.executableAlignment;
        } else {
            lockData["alignment"] = nullptr;
        }
    }

    static bool validateCanonicalTemplate(
        const tbc::BytecodeArtifact& artifact
    )
    {
        tbc::BytecodeArtifact validationArtifact = artifact;
        tbc::PlaceholderBindings validationBindings;
        std::unordered_map<std::string, size_t> fixedSizes;

        for (auto& instruction : validationArtifact.lockingScript) {
            auto* placeholder =
                std::get_if<tbc::PlaceholderPushInstruction>(
                    &instruction.body
                );
            if (!placeholder) {
                continue;
            }

            if (placeholder->expectedPayloadSize.has_value()) {
                if (*placeholder->expectedPayloadSize >
                    std::numeric_limits<uint32_t>::max()) {
                    reportExportError(
                        "CanonicalV2 placeholder '" + placeholder->label +
                        "' has an unencodable expected payload size"
                    );
                    return false;
                }
                const auto [fixed, inserted] = fixedSizes.emplace(
                    placeholder->label,
                    *placeholder->expectedPayloadSize
                );
                if (!inserted &&
                    fixed->second != *placeholder->expectedPayloadSize) {
                    reportExportError(
                        "CanonicalV2 placeholder '" + placeholder->label +
                        "' has conflicting expected payload sizes"
                    );
                    return false;
                }
            }

            validationBindings.try_emplace(
                placeholder->label, std::vector<uint8_t>{}
            );
            // Structural validation must not allocate the declared payload.
            // The real size constraint remains present in the exported
            // relocation and is enforced during actual materialization.
            placeholder->expectedPayloadSize.reset();
        }

        const auto validation = tbc::BytecodeMaterializer::materialize(
            validationArtifact, validationBindings
        );
        if (!validation.ok()) {
            reportMaterializationError(validation.error, "template validation");
            return false;
        }
        return true;
    }

    static bool buildCanonicalLock(
        const tbc::BytecodeArtifact& artifact,
        nlohmann::ordered_json& lockData
    )
    {
        const auto description =
            tbc::BytecodeMaterializer::describeTemplate(artifact);
        lockData = nlohmann::ordered_json::object();
        addCanonicalLayout(artifact, lockData);

        if (description.hasUnresolvedPlaceholders()) {
            if (!validateCanonicalTemplate(artifact)) {
                return false;
            }

            lockData["template"] = nlohmann::ordered_json::array();
            for (const auto& instruction : artifact.lockingScript) {
                lockData["template"].push_back(
                    instructionJson(instruction)
                );
            }
            lockData["relocations"] = nlohmann::ordered_json::array();
            for (const auto& relocation : description.relocations) {
                lockData["relocations"].push_back(
                    relocationJson(relocation)
                );
            }
            return true;
        }

        const auto materialized = tbc::BytecodeMaterializer::materialize(
            artifact, tbc::PlaceholderBindings{}
        );
        if (!materialized.ok()) {
            reportMaterializationError(
                materialized.error, "materialization"
            );
            return false;
        }

        lockData["relocations"] = nlohmann::ordered_json::array();
        try {
            lockData["asm"] = tbc::script_decoder::hex_to_asm(
                materialized.value->finalHex
            );
        } catch (const std::exception& error) {
            reportExportError(
                std::string("CanonicalV2 disassembly failed: ") +
                    error.what()
            );
            return false;
        }
        lockData["hex"] = materialized.value->finalHex;
        return true;
    }

    static nlohmann::ordered_json buildLegacyLock(
        const std::vector<std::string>& instructions
    )
    {
        std::ostringstream stream;
        for (const auto& instruction : instructions) {
            if (!instruction.empty() && instruction[0] != '#') {
                stream << instruction;
            }
        }
        const std::string hex = stream.str();

        nlohmann::ordered_json lockData;
        try {
            lockData["asm"] = tbc::script_decoder::hex_to_asm(hex);
        } catch (const std::exception& error) {
            // Preserve LegacyV1 behavior: a disassembly failure does not alter
            // the byte string and historically produced an empty asm field.
            LOG_WARNING("Failed to convert hex to asm: ", error.what());
            lockData["asm"] = "";
        }
        lockData["hex"] = hex;
        return lockData;
    }

    static bool buildLockData(
        const std::shared_ptr<tbc::BytecodeArtifact>& artifact,
        const std::shared_ptr<LegacyBytecode>& legacyBytecode,
        nlohmann::ordered_json& lockData
    )
    {
        if (artifact) {
            if (artifact->lockingScript.empty()) {
                reportExportError(
                    "bytecode artifact is empty during export",
                    ErrorCategory::INTERNAL,
                    "Ensure the contract emits locking script bytecode"
                );
                return false;
            }

            if (artifact->format == tbc::ArtifactFormat::CanonicalV2) {
                return buildCanonicalLock(*artifact, lockData);
            }

            const auto preserved =
                tbc::LegacyBytecodeAdapter::exportPreserving(*artifact);
            lockData = buildLegacyLock(preserved.first);
            return true;
        }

        if (!legacyBytecode) {
            reportExportError(
                "bytecode data not found during export",
                ErrorCategory::INTERNAL,
                "Ensure bytecode generation completed before exporting results"
            );
            return false;
        }
        if (legacyBytecode->first.empty()) {
            reportExportError(
                "bytecode data is empty during export",
                ErrorCategory::INTERNAL,
                "Ensure the contract emits locking script bytecode"
            );
            return false;
        }

        lockData = buildLegacyLock(legacyBytecode->first);
        return true;
    }

    static bool writeAtomically(
        const std::string& resultFileName,
        const std::string& serialized
    )
    {
        const std::filesystem::path target(resultFileName);
        std::filesystem::path temporary = target;
        temporary += ".tmp";

        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::out | std::ios::trunc
            );
            if (!output.is_open()) {
                reportExportError(
                    "failed to open '" + temporary.string() +
                        "' for writing",
                    ErrorCategory::IO,
                    "Check the output directory and file permissions"
                );
                return false;
            }
            output << serialized;
            output.close();
            if (!output) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                reportExportError(
                    "failed while writing '" + temporary.string() + "'",
                    ErrorCategory::IO,
                    "Check available disk space and file permissions"
                );
                return false;
            }
        }

        std::error_code renameError;
        std::filesystem::rename(temporary, target, renameError);
        if (renameError) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            reportExportError(
                "failed to publish '" + target.string() + "': " +
                    renameError.message(),
                ErrorCategory::IO,
                "Check output file permissions"
            );
            return false;
        }
        return true;
    }
};

#endif // EXPORT_RESULTS_H
