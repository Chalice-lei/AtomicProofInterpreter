#include "debugger_core.h"

#include "../../compiler/compiler_driver.h"

namespace apc_debug
{

DebuggerCore::CompileResult DebuggerCore::compileSource(
    const std::string& sourceFile,
    const std::string& sourceCode,
    bool allowSubscopeAltstack
)
{
    CompileResult result;

    apc::CompilerOptions options;
    options.allowSubscopeAltstack = allowSubscopeAltstack;
    options.enableDebug = true;
    options.exportResults = false;
    options.colorDiagnostics = false;
    options.showDiagnosticContext = false;

    auto compileResult =
        apc::CompilerDriver::compileSource(sourceFile, sourceCode, options);
    if (!compileResult.success) {
        result.errorMessage = "编译失败: " + compileResult.errorMessage;
        return result;
    }

    result.hexBytecode = compileResult.hexBytecode;
    result.bytecodeInstructions = compileResult.asmInstructions;
    result.debugInfo = compileResult.debugInfo;
    result.jsonData = compileResult.jsonData;

    if (!result.debugInfo) {
        result.debugInfo = std::make_shared<DebugInfo>();
        result.debugInfo->sourceFilename = sourceFile;
    }

    validateDebugInfo(result.debugInfo, result.hexBytecode);
    result.success = true;
    return result;
}

bool DebuggerCore::validateDebugInfo(
    std::shared_ptr<DebugInfo> debugInfo,
    const std::string& hexBytecode
)
{
    if (!debugInfo) {
        return false;
    }

    if (!debugInfo->validate()) {
        return false;
    }

    // TODO: 验证 PC 映射合理性
    (void)hexBytecode;

    return true;
}

} // namespace apc_debug
