#ifndef PARSER_PASS_H
#define PARSER_PASS_H

#include <vector>

#include "ast/source_location_mapper.h"
#include "debug/printvisit.h"
#include "include/pass_type.h"
#include "parser/parser.h"
#include "pass/pass.h"
#include "pass/pass_context.h"
#include "pass/pass_context_keys.h"
#include "pass/pass_macros.h"

class ParserPass : public Pass
{
    DECLARE_PASS(ParserPass)
public:
    void execute(PassContext& data) override
    {
        auto tokens = data.get<std::vector<Token>>(apc_pipeline::key::kTokens);

        Parser parser(*tokens);
        auto astPtr = parser.parseContract();

        if (auto sourceMap =
                data.tryGet<SourceMap>(apc_pipeline::key::kSourceMap)) {
            ast_source::applySourceMap(astPtr.get(), *sourceMap);
        }

        debugPrintAst(astPtr);
        data.set(apc_pipeline::key::kAst, astPtr);
    }
    std::vector<std::string> getDependencies() const override
    {
        return {DependTypeToString(DependType::LexerPass)};
    }
};

#endif // PARSER_PASS_H
