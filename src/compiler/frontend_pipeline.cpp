#include "frontend_pipeline.h"

#include <exception>

#include "../error/error_manager.h"
#include "../lexer_pass.h"
#include "../log/logger.h"
#include "../parser_pass.h"
#include "../pass/pass_context_keys.h"
#include "../pass/pass_macros.h"
#include "../pass/pass_manager.h"
#include "library_merger.h"

namespace apc_frontend
{

FrontendResult compileFrontendToAst(
    const std::string& sourceFile,
    const std::string& sourceCode,
    const FrontendOptions& options
)
{
    FrontendResult result;

    try {
        PassContext pipelineData;
        pipelineData.set(
            apc_pipeline::key::kSourceFilePath,
            std::make_shared<std::string>(sourceFile)
        );
        pipelineData.set(
            apc_pipeline::key::kSourceCode,
            std::make_shared<std::string>(sourceCode)
        );

        PassManager pm;
        REGISTER_PASS(pm, LexerPass);
        REGISTER_PASS(pm, ParserPass);
        pm.enablePass("LexerPass");
        pm.enablePass("ParserPass");
        pm.run(pipelineData);

        if (ErrorManager::getInstance().hasErrors()) {
            result.errorMessage = "front-end analysis failed";
            result.context = pipelineData;
            return result;
        }

        auto ast = pipelineData.tryGet<ContractNode>(apc_pipeline::key::kAst);
        if (!ast) {
            result.errorMessage = "parser did not produce a contract AST";
            result.context = pipelineData;
            return result;
        }

        if (options.mergeLibraries) {
            result.mergedLibraryMembers =
                LibraryMerger::mergeIntoContract(*ast);
            LOG_DEBUG(
                "FrontendPipeline - merged " +
                std::to_string(result.mergedLibraryMembers) +
                " library member(s)"
            );
        }

        result.ast = ast;
        result.context = pipelineData;
        result.success = true;
        return result;
    } catch (const std::exception& e) {
        result.errorMessage =
            "front-end analysis exception: " + std::string(e.what());
        return result;
    }
}

} // namespace apc_frontend
