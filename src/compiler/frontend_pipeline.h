#ifndef FRONTEND_PIPELINE_H
#define FRONTEND_PIPELINE_H

#include <cstddef>
#include <memory>
#include <string>

#include "../ast/ast.h"
#include "../pass/pass_context.h"

namespace apc_frontend
{

struct FrontendOptions
{
    bool mergeLibraries = true;
};

struct FrontendResult
{
    bool success = false;
    std::string errorMessage;
    std::shared_ptr<ContractNode> ast;
    std::size_t mergedLibraryMembers = 0;
    PassContext context;
};

FrontendResult compileFrontendToAst(
    const std::string& sourceFile,
    const std::string& sourceCode,
    const FrontendOptions& options = {}
);

} // namespace apc_frontend

#endif // FRONTEND_PIPELINE_H
