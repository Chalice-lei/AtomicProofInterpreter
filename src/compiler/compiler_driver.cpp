#include "compiler_driver.h"

#include <filesystem>
#include <sstream>

#include "../bytecode/script_decoder.h"
#include "../error/error_manager.h"
#include "../pass/pass_context_keys.h"
#include "bytecode_pipeline.h"
#include "compiler_result_json.h"

namespace apc
{
namespace
{

std::string defaultCodeFileName(const std::string& sourceFile)
{
    std::filesystem::path path(sourceFile);
    std::string stem = path.stem().string();
    return stem.empty() ? "repl_cell" : stem;
}

} // namespace

CompilerResult CompilerDriver::compileSource(
    const std::string& sourceFile,
    const std::string& sourceCode,
    const CompilerOptions& options
)
{
    CompilerResult result;

    ErrorManager::getInstance().clear();
    ErrorManager::getInstance().setSourceContent(sourceFile, sourceCode);
    ErrorManager::getInstance().setColorOutput(options.colorDiagnostics);
    ErrorManager::getInstance().setShowContext(options.showDiagnosticContext);

    try {
        apc_compiler::BytecodePipelineOptions pipelineOptions;
        pipelineOptions.allowSubscopeAltstack =
            options.allowSubscopeAltstack;
        pipelineOptions.enableDebug = options.enableDebug;
        pipelineOptions.exportResults = options.exportResults;
        pipelineOptions.codeFileName = options.codeFileName.empty()
                                           ? defaultCodeFileName(sourceFile)
                                           : options.codeFileName;
        pipelineOptions.debugOutputFile = options.debugOutputFile;
        pipelineOptions.suppressDebugFile =
            options.enableDebug && options.debugOutputFile.empty();
        // CompilerDriver feeds the debugger, REPL and in-process VM, all of
        // which require concrete legacy instruction atoms today. The CLI
        // export pipeline may opt into CanonicalV2 independently.
        pipelineOptions.artifactFormat = "legacy_v1";

        PassContext pipelineData = apc_compiler::runBytecodePipeline(
            sourceFile, sourceCode, pipelineOptions
        );

        if (ErrorManager::getInstance().hasErrors()) {
            result.errorMessage = "compilation failed";
            return result;
        }

        auto bytecodePtr =
            pipelineData.tryGet<apc_pipeline::BytecodeOutput>(
                apc_pipeline::key::kBytecode
            );
        if (!bytecodePtr || bytecodePtr->first.empty()) {
            result.errorMessage = "compilation failed: no bytecode generated";
            return result;
        }

        result.rawInstructions = bytecodePtr->first;

        std::ostringstream hexOss;
        for (const auto& instr : result.rawInstructions) {
            if (!instr.empty() && instr[0] != '#') {
                hexOss << instr;
            }
        }
        result.hexBytecode = hexOss.str();
        result.asmInstructions = hexToAsmInstructions(result.hexBytecode);

        if (auto abi =
                pipelineData.tryGet<nlohmann::ordered_json>(
                    apc_pipeline::key::kAbi
                )) {
            result.abi = *abi;
        }
        if (auto unlock =
                pipelineData.tryGet<nlohmann::ordered_json>(
                    apc_pipeline::key::kUnlock
                )) {
            result.unlock = *unlock;
        }
        if (auto constructorParams =
                pipelineData.tryGet<nlohmann::ordered_json>(
                    apc_pipeline::key::kConstructorParams
                )) {
            result.constructorParams = *constructorParams;
        }
        if (auto structs =
                pipelineData.tryGet<nlohmann::ordered_json>(
                    apc_pipeline::key::kStructs
                )) {
            result.structs = *structs;
        }
        if (auto functions =
                pipelineData.tryGet<nlohmann::ordered_json>(
                    apc_pipeline::key::kAllFunctions
                )) {
            result.functions = *functions;
        }

        CompilerResultJsonSections jsonSections;
        jsonSections.structs = result.structs;
        jsonSections.abi = result.abi;
        jsonSections.unlock = result.unlock;
        jsonSections.constructorParams = result.constructorParams;
        jsonSections.functions = result.functions;
        result.jsonData = buildCompilerResultJson(
            jsonSections, CompilerLockJson{result.hexBytecode, std::nullopt}
        );

#ifdef ENABLE_DEBUGGER
        if (auto debugInfo =
                pipelineData.tryGet<apc_debug::DebugInfo>(
                    apc_pipeline::key::kDebugInfo
                )) {
            result.debugInfo = debugInfo;
        }
#endif

        result.success = true;
        return result;
    } catch (const std::exception& e) {
        result.errorMessage = e.what();
        return result;
    }
}

std::vector<std::string> CompilerDriver::hexToAsmInstructions(
    const std::string& hexBytecode
)
{
    std::vector<std::string> instructions;

    try {
        std::string asmString = tbc::script_decoder::hex_to_asm(hexBytecode);
        std::istringstream asmIss(asmString);
        std::string token;
        while (asmIss >> token) {
            if (!token.empty()) {
                instructions.push_back(token);
            }
        }
    } catch (...) {
        instructions.clear();
    }

    return instructions;
}

} // namespace apc
