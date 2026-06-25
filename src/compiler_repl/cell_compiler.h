#ifndef CELL_COMPILER_H
#define CELL_COMPILER_H

#include <cstddef>
#include <string>

#include "repl_session.h"
#include "../compiler/compiler_driver.h"

namespace apc::repl
{

enum class CellKind {
    Empty,
    Import,
    Member,
    Statement,
    Expression,
    Contract
};

struct CellExecutionResult
{
    bool success = false;
    bool hasOutput = false;
    std::string output;
    std::string errorMessage;

    CellKind kind = CellKind::Empty;
    std::string syntheticSource;
    std::string entryFunction;
    CompilerResult compileResult;
};

class CellCompiler
{
public:
    CellExecutionResult executeCell(
        ReplSession& session,
        int inputIndex,
        const std::string& cellText
    ) const;

    CompilerResult compileSession(
        const ReplSession& session,
        bool enableDebug = true
    ) const;

    std::string renderSessionSource(
        const ReplSession& session,
        const std::string& candidate,
        CellKind kind,
        int inputIndex,
        std::string* outEntryFunction = nullptr,
        size_t* outCellStartLine = nullptr,
        size_t* outCellEndLine = nullptr
    ) const;

    CellKind classify(const std::string& cellText) const;

private:
    CellExecutionResult compileSourceForCell(
        const std::string& source,
        const std::string& sourceFile
    ) const;

    std::string runCellRange(
        const CompilerResult& result,
        ReplSession& session,
        const std::string& entryFunction,
        bool hasPCRange,
        size_t pcStart,
        size_t pcEnd,
        bool commitStacks,
        bool& ok,
        std::string& errorMessage
    ) const;

    std::string runEntryFunction(
        const CompilerResult& result,
        const std::string& entryFunction,
        bool& ok,
        std::string& errorMessage
    ) const;
};

const char* cellKindName(CellKind kind);

} // namespace apc::repl

#endif // CELL_COMPILER_H
