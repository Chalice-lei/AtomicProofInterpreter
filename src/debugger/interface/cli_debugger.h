#ifndef CLI_DEBUGGER_H
#define CLI_DEBUGGER_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../breakpoint/breakpoint.h"
#include "../breakpoint/breakpoint_manager.h"
#include "../info/debug_info.h"
#include "../inspector/variable_inspector.h"
#include "../vm/bvm_simulator.h"

namespace apc_debug
{

/**
 * @brief CLI 调试器：REPL、命令分发、源码/变量/调用栈显示。
 */
class CLIDebugger
{
public:
    enum class Language
    {
        Chinese,
        English
    };

    CLIDebugger(
        std::shared_ptr<BVMSimulator> vm,
        std::shared_ptr<BreakpointManager> bpMgr,
        std::shared_ptr<DebugInfo> debugInfo
    );

    ~CLIDebugger() = default;

    void run();

    void setPrompt(const std::string& prompt)
    {
        m_prompt = prompt;
    }

    void setLanguage(Language lang)
    {
        m_language = lang;
    }

    Language getLanguage() const
    {
        return m_language;
    }

    void setSourceFile(const std::string& filename);
    bool loadSourceFile(const std::string& filename);

private:
    struct StackViewOptions
    {
        enum class Target { MAIN, ALT, BOTH };
        enum class ViewMode { ALL, NEAR_DEPTH, DEPTH_RANGE };

        Target target = Target::BOTH;
        ViewMode mode = ViewMode::ALL;

        // index=0 表示栈底，越大越靠近栈顶
        // NEAR_DEPTH: nearDepth 中心高度，nearContext 上下文条数
        size_t nearDepth = 0;
        size_t nearContext = 0;

        // DEPTH_RANGE: 含端点
        size_t rangeStartDepth = 0;
        size_t rangeEndDepth = 0;
    };

    void replLoop();
    std::string readCommand();
    void handleCommand(const std::string& command);

    void cmdRun(const std::vector<std::string>& args);
    void cmdReset(const std::vector<std::string>& args);
    void cmdContinue(const std::vector<std::string>& args);
    void cmdStepIn(const std::vector<std::string>& args);
    void cmdStepOver(const std::vector<std::string>& args);
    void cmdStepOut(const std::vector<std::string>& args);
    void cmdPause(const std::vector<std::string>& args);

    void cmdBreak(const std::vector<std::string>& args);
    void cmdDelete(const std::vector<std::string>& args);
    void cmdDisable(const std::vector<std::string>& args);
    void cmdEnable(const std::vector<std::string>& args);
    void cmdInfoBreakpoints(const std::vector<std::string>& args);

    void cmdList(const std::vector<std::string>& args);
    void cmdPrint(const std::vector<std::string>& args);
    void cmdLocals(const std::vector<std::string>& args);
    void cmdStack(const std::vector<std::string>& args);
    void cmdBacktrace(const std::vector<std::string>& args);
    void cmdInfo(const std::vector<std::string>& args);
    void cmdBytecode(const std::vector<std::string>& args);

    void cmdSetTxFile(const std::vector<std::string>& args);
    void cmdShowTx(const std::vector<std::string>& args);

    void cmdHelp(const std::vector<std::string>& args);
    void cmdQuit(const std::vector<std::string>& args);
    void cmdClear(const std::vector<std::string>& args);
    void cmdLanguage(const std::vector<std::string>& args);

    void showCurrentLocation();
    void showSourceContext(size_t line, size_t contextLines = 5);
    void showVariables(
        const std::vector<VariableValue>& vars,
        const std::string& title = "Variables"
    );
    void showCallStack();
    void showStackState();
    void showStackState(const StackViewOptions& opt);
    void showBreakpoints();
    void showBytecode(size_t context = 0);

    std::vector<std::string> parseArgs(const std::string& args);

    // 含别名的命令分发表
    using CommandHandler = std::function<void(const std::vector<std::string>&)>;
    std::map<std::string, CommandHandler> buildCommandMap();

    bool readSourceLines(const std::string& filename);

    std::shared_ptr<BVMSimulator> m_vm;
    std::shared_ptr<BreakpointManager> m_bpMgr;
    std::shared_ptr<DebugInfo> m_debugInfo;

    std::string m_prompt;
    std::string m_sourceFilename;
    std::vector<std::string> m_sourceLines; // 索引 0 即行号 1

    bool m_running;
    bool m_showSourceOnStop;

    Language m_language{Language::Chinese};

    std::vector<std::string> m_commandHistory;
    size_t m_historyIndex;
};

} // namespace apc_debug

#endif // CLI_DEBUGGER_H
