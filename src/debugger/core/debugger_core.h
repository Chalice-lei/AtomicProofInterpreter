#ifndef DEBUGGER_CORE_H
#define DEBUGGER_CORE_H

#include <memory>
#include <string>
#include <nlohmann/json.hpp>

#include "../info/debug_info.h"

namespace apc_debug
{

/**
 * @brief 调试器核心：源码编译为字节码后在模拟器上解释执行，
 * 所有调试能力依赖源码行号 ↔ 字节码映射。
 */
class DebuggerCore
{
public:
    struct CompileResult
    {
        bool success = false;
        std::string errorMessage;

        std::string hexBytecode;
        std::vector<std::string> bytecodeInstructions;

        std::shared_ptr<DebugInfo> debugInfo;

        // ABI 等元数据
        nlohmann::json jsonData;
    };

    // allowSubscopeAltstack: 是否允许子作用域使用副栈
    static CompileResult compileSource(
        const std::string& sourceFile,
        const std::string& sourceCode,
        bool allowSubscopeAltstack = false
    );

    static bool validateDebugInfo(
        std::shared_ptr<DebugInfo> debugInfo,
        const std::string& hexBytecode
    );
};

} // namespace apc_debug

#endif // DEBUGGER_CORE_H
