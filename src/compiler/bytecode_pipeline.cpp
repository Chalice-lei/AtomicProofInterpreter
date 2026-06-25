#include "bytecode_pipeline.h"

#include <filesystem>
#include <memory>

#include "../ast_to_bytecode_pass.h"
#include "../bytecode_finalize_pass.h"
#include "../bytecode_peephole_pass.h"
#include "../export_results_pass.h"
#include "../lexer_pass.h"
#include "../parser_pass.h"
#include "../pass/pass_context_keys.h"
#include "../pass/pass_macros.h"
#include "../pass/pass_manager.h"

namespace apc_compiler
{

PassContext runBytecodePipeline(
    const std::string& sourceFile,
    const std::string& sourceCode,
    const BytecodePipelineOptions& options
)
{
    PassContext pipelineData;
    const std::string filenameWithoutExt =
        options.codeFileName.empty()
            ? std::filesystem::path(sourceFile).stem().string()
            : options.codeFileName;

    pipelineData.set(
        apc_pipeline::key::kCodeFileName,
        std::make_shared<std::string>(filenameWithoutExt)
    );
    pipelineData.set(
        apc_pipeline::key::kSourceFilePath,
        std::make_shared<std::string>(sourceFile)
    );
    pipelineData.set(
        apc_pipeline::key::kSourceCode,
        std::make_shared<std::string>(sourceCode)
    );
    pipelineData.set(
        apc_pipeline::key::kAllowSubscopeAltstack,
        std::make_shared<bool>(options.allowSubscopeAltstack)
    );
    pipelineData.set(
        apc_pipeline::key::kEnableDebug,
        std::make_shared<bool>(options.enableDebug)
    );
    pipelineData.set(
        apc_pipeline::key::kSuppressDebugFile,
        std::make_shared<bool>(options.suppressDebugFile)
    );

    if (!options.debugOutputFile.empty()) {
        pipelineData.set(
            apc_pipeline::key::kDebugOutputFile,
            std::make_shared<std::string>(options.debugOutputFile)
        );
    }

    PassManager pm;
    REGISTER_PASS(pm, LexerPass);
    REGISTER_PASS(pm, ParserPass);
    REGISTER_PASS(pm, ASTToBytecodePass);
    REGISTER_PASS(pm, BytecodePeepholePass);
    REGISTER_PASS(pm, BytecodeFinalizePass);
    if (options.exportResults) {
        REGISTER_PASS(pm, ExportResultsPass);
    }

    pm.enablePass("LexerPass");
    pm.enablePass("ParserPass");
    pm.enablePass("ASTToBytecodePass");
    pm.enablePass("BytecodePeepholePass");
    pm.enablePass("BytecodeFinalizePass");
    if (options.exportResults) {
        pm.enablePass("ExportResultsPass");
    }

    pm.run(pipelineData);
    return pipelineData;
}

} // namespace apc_compiler
