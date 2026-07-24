#include "debugger_core.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "../../bytecode/bytecode_instruction_utils.h"
#include "../../bytecode/script_decoder.h"
#include "../../compiler/compiler_driver.h"

namespace apc_debug
{
namespace
{
bool decodeFinalBytecode(
    const std::string& hexBytecode,
    std::vector<std::string>& instructions,
    std::string* errorMessage
)
{
    instructions.clear();
    if (!tbc::bytecode_instruction::isPureHexStrictEven(hexBytecode)) {
        if (errorMessage) {
            *errorMessage = "最终字节码不是非空、偶数长度的纯十六进制串";
        }
        return false;
    }

    const auto bytes = tbc::script_decoder::hex_to_bytes(hexBytecode);
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto decoded = tbc::script_decoder::decode_instruction(
            bytes.data() + offset, bytes.size() - offset
        );
        if (!decoded.valid || decoded.bytes_consumed == 0 ||
            offset + decoded.bytes_consumed > bytes.size()) {
            if (errorMessage) {
                *errorMessage = "最终字节码在字节偏移 " +
                                std::to_string(offset) + " 处指令不完整";
            }
            return false;
        }

        std::string raw = hexBytecode.substr(
            offset * 2, decoded.bytes_consumed * 2
        );
        std::transform(raw.begin(), raw.end(), raw.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        instructions.push_back(std::move(raw));
        offset += decoded.bytes_consumed;
    }

    return !instructions.empty();
}
} // namespace

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

    std::string validationError;
    if (!validateDebugInfo(
            result.debugInfo, result.hexBytecode, &validationError
        )) {
        result.errorMessage = "调试信息校验失败: " + validationError;
        return result;
    }
    result.success = true;
    return result;
}

bool DebuggerCore::validateDebugInfo(
    std::shared_ptr<DebugInfo> debugInfo,
    const std::string& hexBytecode,
    std::string* errorMessage
)
{
    if (!debugInfo) {
        if (errorMessage) {
            *errorMessage = "调试信息为空";
        }
        return false;
    }

    std::vector<std::string> instructions;
    if (!decodeFinalBytecode(hexBytecode, instructions, errorMessage)) {
        return false;
    }

    return debugInfo->validate(instructions, errorMessage);
}

} // namespace apc_debug
