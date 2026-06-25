#ifndef BYTECODE_RUNNER_H
#define BYTECODE_RUNNER_H

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace apc_interpreter
{

struct StackValueView
{
    std::string hex;
    std::string intValue;
};

struct BytecodeRunOptions
{
    using CheckSigCallback = std::function<bool(
        const std::vector<uint8_t>& sig,
        const std::vector<uint8_t>& pubkey,
        const std::vector<uint8_t>& sighash
    )>;

    bool allowSubscopeAltstack = false;
    std::string functionName;
    std::vector<std::string> args;
    std::string txFile;
    std::string stackTraceOutputFile;
    CheckSigCallback checkSigCallback;
};

struct BytecodeRunResult
{
    bool success = false;
    bool compileSuccess = false;

    std::string status;
    std::string errorMessage;
    std::vector<std::string> warnings;

    std::string functionName;
    std::size_t startPC = 0;
    std::size_t endPC = 0;
    std::size_t pc = 0;

    std::size_t totalInstructions = 0;
    std::size_t executedInstructions = 0;
    std::size_t maxStackSize = 0;
    std::size_t maxCallDepth = 0;

    std::string hexBytecode;
    std::vector<std::string> bytecodeInstructions;
    std::vector<StackValueView> mainStack;
    std::vector<StackValueView> altStack;

    bool stackTraceWritten = false;
    std::string stackTraceOutputFile;
};

class BytecodeRunner
{
public:
    BytecodeRunResult runSource(
        const std::string& sourceFile,
        const std::string& sourceCode,
        const BytecodeRunOptions& options
    ) const;
};

} // namespace apc_interpreter

#endif // BYTECODE_RUNNER_H
