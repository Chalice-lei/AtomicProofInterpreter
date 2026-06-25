#ifndef DEBUG_INFO_SAVE_H
#define DEBUG_INFO_SAVE_H

#ifdef ENABLE_DEBUGGER

#include <memory>
#include <string>

#include "../../log/logger.h"
#include "../../pass/pass_context.h"
#include "../../pass/pass_context_keys.h"
#include "debug_info.h"

namespace apc_debug
{

struct DebugInfoSaveOptions
{
    bool useDefaultOutputFile = false;
    bool respectSuppressFlag = false;
    bool logSuccessStats = false;
};

inline bool isDebugInfoSaveSuppressed(PassContext& data)
{
    auto suppress =
        data.tryGet<bool>(apc_pipeline::key::kSuppressDebugFile);
    return suppress && *suppress;
}

inline std::string resolveDebugInfoOutputFile(
    PassContext& data,
    bool useDefaultOutputFile
)
{
    auto outputFile =
        data.tryGet<std::string>(apc_pipeline::key::kDebugOutputFile);
    if (outputFile && !outputFile->empty()) {
        return *outputFile;
    }

    if (!useDefaultOutputFile) {
        return "";
    }

    auto codeFileName =
        data.tryGet<std::string>(apc_pipeline::key::kCodeFileName);
    if (codeFileName && !codeFileName->empty()) {
        return *codeFileName + ".debug";
    }

    return "output.debug";
}

inline void saveDebugInfoIfRequested(
    PassContext& data,
    const std::shared_ptr<DebugInfo>& debugInfo,
    const DebugInfoSaveOptions& options = {}
)
{
    if (!debugInfo) {
        return;
    }

    if (options.respectSuppressFlag && isDebugInfoSaveSuppressed(data)) {
        LOG_INFO("调试信息已保存在内存中，跳过 .debug 文件写出");
        return;
    }

    std::string outputFile =
        resolveDebugInfoOutputFile(data, options.useDefaultOutputFile);
    if (outputFile.empty()) {
        return;
    }

    if (debugInfo->save(outputFile)) {
        if (options.logSuccessStats) {
            LOG_INFO("调试信息已保存到: " + outputFile);
            LOG_INFO("调试信息统计:");
            LOG_INFO(
                "  函数数量: " +
                std::to_string(debugInfo->getTotalFunctions())
            );
            LOG_INFO(
                "  变量数量: " +
                std::to_string(debugInfo->getTotalVariables())
            );
            LOG_INFO(
                "  指令数量: " +
                std::to_string(debugInfo->getTotalInstructions())
            );
        }
    } else {
        LOG_WARNING("调试信息保存失败: " + outputFile);
    }
}

} // namespace apc_debug

#endif // ENABLE_DEBUGGER

#endif // DEBUG_INFO_SAVE_H
