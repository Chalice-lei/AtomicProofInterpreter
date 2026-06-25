#ifndef LEXER_PASS_H
#define LEXER_PASS_H

#include <string>

#include "error/error_manager.h"
#include "lexer/import_resolver.h"
#include "lexer/lexer.h"
#include "log/logger.h"
#include "pass/pass.h"
#include "pass/pass_context.h"
#include "pass/pass_context_keys.h"
#include "pass/pass_macros.h"

class LexerPass : public Pass
{
    DECLARE_PASS(LexerPass)
public:
    void execute(PassContext& data) override
    {
        auto source = data.get<std::string>(apc_pipeline::key::kSourceCode);
        auto sourcePath =
            data.get<std::string>(apc_pipeline::key::kSourceFilePath);

        // 内联 import
        ImportResolver resolver;
        ExpandedSource expanded =
            resolver.resolveWithMap(*source, sourcePath ? *sourcePath : "");
        LOG_DEBUG("import-expanded source:\n", expanded.code);

        ErrorManager::getInstance().setSourceMap(expanded.sourceMap);
        for (const auto& [filename, content] : expanded.sourceContents) {
            ErrorManager::getInstance().setSourceContent(filename, content);
        }

        Lexer lexer(expanded.code, "", &expanded.sourceMap);
        auto tokens = std::make_shared<std::vector<Token>>(lexer.tokenize());

#ifndef NDEBUG
        int i = 0;
        for (const auto& token : *tokens) {
            LOG_DEBUG("Token ",
                      i,
                      "\t: ",
                      TokenTypeToString.at(token.type),
                      "(",
                      token.value,
                      ")",
                      "\tlen:",
                      token.length,
                      "\tpos:",
                      token.position.line,
                      ",",
                      token.position.column);
            ++i;
        }
#else
#endif
        data.set(apc_pipeline::key::kTokens, tokens);
        data.set(
            apc_pipeline::key::kSourceMap,
            std::make_shared<SourceMap>(expanded.sourceMap)
        );
    }
};

#endif // LEXER_PASS_H
