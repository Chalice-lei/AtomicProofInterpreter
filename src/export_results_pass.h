#ifndef EXPORT_RESULTS_H
#define EXPORT_RESULTS_H

#include <ctime>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>

#include "bytecode/script_decoder.h"
#include "compiler/compiler_result_json.h"
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

        if (!data.contains<apc_pipeline::BytecodeOutput>(
                apc_pipeline::key::kBytecode
            )) {
            LOG_ERROR(
                "ExportResultsPass::execute - Can't find bytcode data in "
                "PassContext"
            );
            INTERNAL_ERROR(
                "bytecode data not found during export",
                SourceLocation("", 0, 0),
                "Ensure bytecode generation completed before exporting results"
            );
            return;
        }

        auto bytcodePtr = data.get<apc_pipeline::BytecodeOutput>(
            apc_pipeline::key::kBytecode
        );
        if (!bytcodePtr || bytcodePtr->first.empty()) {
            LOG_ERROR("ExportResultsPass::execute - bytcode data is empty");
            INTERNAL_ERROR(
                "bytecode data is empty during export",
                SourceLocation("", 0, 0),
                "Ensure the contract emits locking script bytecode"
            );
            return;
        }

        try {
            std::vector<std::string>& bytcode = bytcodePtr->first;

            std::string resultFileName{"clean_script.json"};
            auto codefileNamePtr =
                data.get<std::string>(apc_pipeline::key::kCodeFileName);
            if (codefileNamePtr && !codefileNamePtr->empty()) {
                resultFileName = *codefileNamePtr + ".json";
            }

            // ordered_json 保证字段顺序
            nlohmann::ordered_json metadata;
            std::ostringstream oss;

            // 收集 hex, 过滤 # 注释行
            for (const auto& instr : bytcode) {
                if (!instr.empty() && instr[0] != '#') {
                    oss << instr;
                }
            }

            std::string hexString = oss.str();

            // 1. metadata
            std::string sourceFileName = codefileNamePtr ? *codefileNamePtr
                                                         : "unknown";
            if (ConfigManager::getInstance().isInitialized()) {
                metadata =
                    ConfigManager::getInstance().generateMetadata(sourceFileName);
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

                metadata = nlohmann::ordered_json{
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

            apc::CompilerResultJsonSections jsonSections;
            jsonSections.metadata = metadata;

            // 2. structs (置于 ABI 前)
            jsonSections.structs = getJsonSection(
                data,
                apc_pipeline::key::kStructs,
                "Struct definitions",
                false
            );

            // 3. abi
            jsonSections.abi = getJsonSection(
                data,
                apc_pipeline::key::kAbi,
                "ABI",
                true
            );

            // 4. lock (asm + hex)
            std::optional<std::string> asmCode;
            try {
                asmCode = tbc::script_decoder::hex_to_asm(
                    hexString
                );
            } catch (const std::exception& e) {
                LOG_WARNING("Failed to convert hex to asm: ", e.what());
                asmCode = "";
            }

            // 5. unlock
            jsonSections.unlock = getJsonSection(
                data,
                apc_pipeline::key::kUnlock,
                "Unlock",
                true
            );

            // 6. constructorParams
            jsonSections.constructorParams = getJsonSection(
                data,
                apc_pipeline::key::kConstructorParams,
                "ConstructorParams",
                true
            );

            // 7. functions: 全部函数信息 (含私有函数, 供调试器使用)
            jsonSections.functions = getJsonSection(
                data,
                apc_pipeline::key::kAllFunctions,
                "All functions",
                true
            );

            nlohmann::ordered_json resultJson = apc::buildCompilerResultJson(
                jsonSections, apc::CompilerLockJson{hexString, asmCode}
            );

            std::ofstream cleanScript(resultFileName);
            if (cleanScript.is_open()) {
                cleanScript << resultJson.dump(2);
                cleanScript.close();
                LOG_INFO("Clean script saved to ", resultFileName);
            } else {
                LOG_ERROR("Failed to open " + resultFileName + " for writing");
            }
        } catch (const std::out_of_range& e) {
            LOG_ERROR(
                "ExportResultsPass::execute - Out of range error during "
                "result export: ",
                e.what(),
                " - This may be due to accessing uninitialized map entries"
            );
        } catch (const std::exception& e) {
            LOG_ERROR(
                "ExportResultsPass::execute - An exception occurred "
                "during result export: ",
                e.what()
            );
        } catch (...) {
            LOG_ERROR(
                "ExportResultsPass::execute - An unknown exception "
                "occurred during result export"
            );
        }
    }

    std::vector<std::string> getDependencies() const override
    {
        return {DependTypeToString(DependType::BytecodeFinalizePass)};
    }

private:
    static nlohmann::ordered_json getJsonSection(
        PassContext& data,
        const std::string& contextKey,
        const std::string& logName,
        bool warnWhenEmpty
    )
    {
        auto section = data.tryGet<nlohmann::ordered_json>(contextKey);
        if (section && !section->empty()) {
            LOG_INFO(logName, " data added to result JSON");
            return *section;
        }

        if (warnWhenEmpty) {
            LOG_WARNING(
                logName,
                " data not added: ",
                section ? "empty object" : "null pointer"
            );
        }
        return nlohmann::ordered_json();
    }
};

#endif // EXPORT_RESULTS_H
