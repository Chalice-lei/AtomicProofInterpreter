#ifndef REPL_SESSION_H
#define REPL_SESSION_H

#include <string>
#include <vector>

#include "../ast/ast.h"
#include "../interpreter/ast_interpreter.h"

namespace apc_repl
{

struct ReplCellResult
{
    bool success = false;
    std::string errorMessage;
    std::string message;
    bool hasOutput = false;
    std::vector<apc_interpreter::RuntimeValue> outputValues;
};

class ReplSession
{
public:
    ReplSession();

    ReplCellResult executeCell(const std::string& source);
    ReplCellResult loadFile(const std::string& filename);
    void reset();
    std::vector<std::string> completionWords() const;
    std::vector<std::string> userNames() const;
    const std::string& lastLoadedFile() const
    {
        return m_lastLoadedFile;
    }

private:
    std::string wrapDefinitionCell(const std::string& source) const;
    std::string wrapStatementCell(const std::string& source) const;
    ReplCellResult executeDefinitionCell(const std::string& source);
    ReplCellResult executeStatementCell(const std::string& source);

    std::shared_ptr<ContractNode> parseWrappedSource(
        const std::string& filename,
        const std::string& wrappedSource,
        ReplCellResult& result
    ) const;
    FunctionNode* findCellFunction(ContractNode& contract) const;
    void mergeMembers(ContractNode& parsedContract);

    ContractNode m_contract;
    apc_interpreter::ASTInterpreter m_interpreter;
    std::string m_lastLoadedFile;
    int m_cellCounter = 0;
};

bool looksLikeDefinitionCell(const std::string& source);

} // namespace apc_repl

#endif // REPL_SESSION_H
