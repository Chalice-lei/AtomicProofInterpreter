#ifndef COMPILER_DRIVER_H
#define COMPILER_DRIVER_H

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef ENABLE_DEBUGGER
#include "../debugger/info/debug_info.h"
#endif

namespace apc
{

struct CompilerOptions
{
    bool allowSubscopeAltstack = false;
    bool enableDebug = false;
    bool exportResults = false;
    bool colorDiagnostics = true;
    bool showDiagnosticContext = true;

    std::string codeFileName;
    std::string debugOutputFile;
};

struct CompilerResult
{
    bool success = false;
    std::string errorMessage;

    std::string hexBytecode;
    std::vector<std::string> rawInstructions;
    std::vector<std::string> asmInstructions;

    nlohmann::ordered_json abi;
    nlohmann::ordered_json unlock;
    nlohmann::ordered_json constructorParams;
    nlohmann::ordered_json structs;
    nlohmann::ordered_json functions;
    nlohmann::ordered_json jsonData;

#ifdef ENABLE_DEBUGGER
    std::shared_ptr<apc_debug::DebugInfo> debugInfo;
#endif
};

class CompilerDriver
{
public:
    static CompilerResult compileSource(
        const std::string& sourceFile,
        const std::string& sourceCode,
        const CompilerOptions& options = {}
    );

    static std::vector<std::string> hexToAsmInstructions(
        const std::string& hexBytecode
    );
};

} // namespace apc

#endif // COMPILER_DRIVER_H
