#include "cli_debugger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "../inspector/expression_evaluator.h"
#include "../inspector/variable_inspector.h"
#include "../vm/stack_state.h"
#include "../../util/string_utils.h"

namespace apc_debug
{

CLIDebugger::CLIDebugger(
    std::shared_ptr<BVMSimulator> vm,
    std::shared_ptr<BreakpointManager> bpMgr,
    std::shared_ptr<DebugInfo> debugInfo
)
    : m_vm(vm), m_bpMgr(bpMgr), m_debugInfo(debugInfo),
      m_prompt("(apc-debug) "), m_running(true), m_showSourceOnStop(true),
      m_language(Language::Chinese), m_historyIndex(0)
{
    m_vm->setEventCallback([this](VMEvent event, const std::string& msg) {
        if (event == VMEvent::BREAKPOINT_HIT || event == VMEvent::PAUSED ||
            event == VMEvent::STEPPED) {
            std::cout << "\n" << msg << std::endl;
            if (m_showSourceOnStop) {
                showCurrentLocation();
            }
        } else if (event == VMEvent::FINISHED) {
            if (m_language == Language::English) {
                std::cout << "\nProgram execution finished." << std::endl;
            } else {
                std::cout << "\n程序执行完成。" << std::endl;
            }
        } else if (event == VMEvent::ERROR) {
            if (m_language == Language::English) {
                std::cout << "\nError: " << msg << std::endl;
            } else {
                std::cout << "\n错误: " << msg << std::endl;
            }
        }
    });
}

void CLIDebugger::run()
{
    std::cout << "=====================================" << std::endl;
    std::cout << "  UTXO_Compiler Debugger v1.0" << std::endl;
    std::cout << "=====================================" << std::endl;

    if (!m_debugInfo->sourceFilename.empty()) {
        if (m_language == Language::English) {
            std::cout << "Load: " << m_debugInfo->sourceFilename << std::endl;
        } else {
            std::cout << "加载: " << m_debugInfo->sourceFilename << std::endl;
        }
        loadSourceFile(m_debugInfo->sourceFilename);
    }

    if (m_language == Language::English) {
        std::cout << "Type 'help' to see available commands." << std::endl;
    } else {
        std::cout << "输入 'help' 查看可用命令。" << std::endl;
    }
    std::cout << std::endl;

    replLoop();
}

void CLIDebugger::replLoop()
{
    while (m_running) {
        std::cout << m_prompt;
        std::string command = readCommand();

        if (command.empty()) {
            continue;
        }

        m_commandHistory.push_back(command);
        if (m_commandHistory.size() > 100) {
            m_commandHistory.erase(m_commandHistory.begin());
        }
        m_historyIndex = m_commandHistory.size();

        handleCommand(command);
    }
}

std::string CLIDebugger::readCommand()
{
    std::string line;
    if (!std::getline(std::cin, line)) {
        m_running = false;
        return "";
    }

    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = line.find_last_not_of(" \t");
    return line.substr(start, end - start + 1);
}

void CLIDebugger::handleCommand(const std::string& command)
{
    std::istringstream iss(command);
    std::string cmd;
    iss >> cmd;

    std::string argsStr;
    std::getline(iss, argsStr);
    auto args = parseArgs(argsStr);

    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

    auto cmdMap = buildCommandMap();

    auto it = cmdMap.find(cmd);
    if (it != cmdMap.end()) {
        it->second(args);
    } else {
        if (m_language == Language::English) {
            std::cout << "Unknown command: " << cmd << std::endl;
            std::cout << "Type 'help' to see available commands." << std::endl;
        } else {
            std::cout << "未知命令: " << cmd << std::endl;
            std::cout << "输入 'help' 查看可用命令。" << std::endl;
        }
    }
}

void CLIDebugger::cmdRun(const std::vector<std::string>& /*args*/)
{
    VMState state = m_vm->getState();
    if (state == VMState::PAUSED || state == VMState::STEP_MODE) {
        // 已在断点/单步处，从当前位置继续，不重置
        if (m_language == Language::English) {
            std::cout << "Continue execution..." << std::endl;
        } else {
            std::cout << "继续执行..." << std::endl;
        }
        m_vm->resume();
    } else {
        if (state == VMState::ERROR) {
            if (m_language == Language::English) {
                std::cout
                    << "VM encountered an error last time, restart execution..."
                    << std::endl;
            } else {
                std::cout << "VM 上次执行出错，重新开始执行..." << std::endl;
            }
        } else {
            if (m_language == Language::English) {
                std::cout << "Start execution..." << std::endl;
            } else {
                std::cout << "开始执行..." << std::endl;
            }
        }
        m_vm->start();
        m_vm->run();
    }
}

void CLIDebugger::cmdReset(const std::vector<std::string>& /*args*/)
{
    if (m_language == Language::English) {
        std::cout << "Reset VM state..." << std::endl;
    } else {
        std::cout << "重置虚拟机状态..." << std::endl;
    }
    m_vm->reset();
    if (m_language == Language::English) {
        std::cout << "VM has been reset to the initial state." << std::endl;
        std::cout << "Use 'run' or 'step' to start execution." << std::endl;
    } else {
        std::cout << "虚拟机已重置到初始状态。" << std::endl;
        std::cout << "使用 'run' 或 'step' 开始执行。" << std::endl;
    }
}

void CLIDebugger::cmdContinue(const std::vector<std::string>& /*args*/)
{
    if (m_vm->getState() == VMState::PAUSED) {
        if (m_language == Language::English) {
            std::cout << "Continue execution..." << std::endl;
        } else {
            std::cout << "继续执行..." << std::endl;
        }
        m_vm->resume();
    } else {
        if (m_language == Language::English) {
            std::cout << "VM is not paused, cannot continue." << std::endl;
        } else {
            std::cout << "VM未暂停，无法继续。" << std::endl;
        }
    }
}

void CLIDebugger::cmdStepIn(const std::vector<std::string>& /*args*/)
{
    if (m_vm->getState() == VMState::PAUSED ||
        m_vm->getState() == VMState::READY) {
        if (m_language == Language::English) {
            std::cout << "Step in..." << std::endl;
        } else {
            std::cout << "单步进入..." << std::endl;
        }
        m_vm->stepIn();
    } else {
        if (m_language == Language::English) {
            std::cout << "VM is not ready, cannot step." << std::endl;
        } else {
            std::cout << "VM未就绪，无法单步执行。" << std::endl;
        }
    }
}

void CLIDebugger::cmdStepOver(const std::vector<std::string>& /*args*/)
{
    if (m_vm->getState() == VMState::PAUSED ||
        m_vm->getState() == VMState::READY) {
        if (m_language == Language::English) {
            std::cout << "Step over..." << std::endl;
        } else {
            std::cout << "单步跳过..." << std::endl;
        }
        m_vm->stepOver();
    } else {
        if (m_language == Language::English) {
            std::cout << "VM is not ready, cannot step." << std::endl;
        } else {
            std::cout << "VM未就绪，无法单步执行。" << std::endl;
        }
    }
}

void CLIDebugger::cmdStepOut(const std::vector<std::string>& /*args*/)
{
    if (m_vm->getState() == VMState::PAUSED ||
        m_vm->getState() == VMState::READY) {
        if (m_language == Language::English) {
            std::cout << "Step out..." << std::endl;
        } else {
            std::cout << "跳出函数..." << std::endl;
        }
        m_vm->stepOut();
    } else {
        if (m_language == Language::English) {
            std::cout << "VM is not ready, cannot step out." << std::endl;
        } else {
            std::cout << "VM未就绪，无法跳出函数。" << std::endl;
        }
    }
}

void CLIDebugger::cmdPause(const std::vector<std::string>& /*args*/)
{
    if (m_vm->getState() == VMState::RUNNING) {
        if (m_language == Language::English) {
            std::cout << "Pause execution..." << std::endl;
        } else {
            std::cout << "暂停执行..." << std::endl;
        }
        m_vm->pause();
    } else {
        if (m_language == Language::English) {
            std::cout << "VM is not running, cannot pause." << std::endl;
        } else {
            std::cout << "VM未运行，无法暂停。" << std::endl;
        }
    }
}

void CLIDebugger::cmdBreak(const std::vector<std::string>& args)
{
    if (args.empty()) {
        if (m_language == Language::English) {
            std::cout << "Usage: break <line> or break <function_name>"
                      << std::endl;
        } else {
            std::cout << "用法: break <行号> 或 break <函数名>" << std::endl;
        }
        return;
    }

    std::string location = args[0];

    // 先按行号解析，失败则当函数名
    try {
        size_t line = std::stoull(location);
        size_t requestedLine = line;
        size_t id =
            m_bpMgr->addLineBreakpoint(m_debugInfo->sourceFilename, line);
        if (id == 0) {
            auto nearestPCs = m_debugInfo
                                  ? m_debugInfo->findNearestValidSourceLine(
                                        m_debugInfo->sourceFilename,
                                        line,
                                        2
                                    )
                                  : std::vector<size_t>();
            if (!nearestPCs.empty()) {
                auto nearestLoc =
                    m_debugInfo->getSourceLocation(nearestPCs.front());
                if (nearestLoc.isValid() && nearestLoc.line != line) {
                    line = nearestLoc.line;
                    id = m_bpMgr->addLineBreakpoint(
                        m_debugInfo->sourceFilename, line
                    );
                }
            }
        }

        if (id == 0) {
            if (m_language == Language::English) {
                std::cout << "Cannot set breakpoint at "
                          << m_debugInfo->sourceFilename << ":" << requestedLine
                          << " (line has no executable bytecode)." << std::endl;
            } else {
                std::cout << "无法在 " << m_debugInfo->sourceFilename << ":"
                          << requestedLine
                          << " 设置断点（该行没有可执行字节码）。" << std::endl;
            }
            return;
        }
        m_bpMgr->resolveBreakpoints();
        if (m_language == Language::English) {
            std::cout << "Breakpoint " << id << " set at "
                      << m_debugInfo->sourceFilename << ":" << line;
            if (line != requestedLine) {
                std::cout << " (nearest executable line for "
                          << requestedLine << ")";
            }
            std::cout << std::endl;
        } else {
            std::cout << "断点 " << id << " 设置在 "
                      << m_debugInfo->sourceFilename << ":" << line;
            if (line != requestedLine) {
                std::cout << "（" << requestedLine << " 的最近可执行行）";
            }
            std::cout << std::endl;
        }
    } catch (...) {
        size_t id = m_bpMgr->addFunctionBreakpoint(location);
        if (id == 0) {
            if (m_language == Language::English) {
                std::cout << "Cannot set function breakpoint: function '"
                          << location << "' does not exist." << std::endl;
            } else {
                std::cout << "无法设置函数断点：函数 '" << location
                          << "' 不存在。" << std::endl;
            }
            return;
        }
        if (m_language == Language::English) {
            std::cout << "Function breakpoint " << id
                      << " set at function: " << location << std::endl;
        } else {
            std::cout << "函数断点 " << id << " 设置在函数: " << location
                      << std::endl;
        }
    }
}

void CLIDebugger::cmdDelete(const std::vector<std::string>& args)
{
    if (args.empty()) {
        if (m_language == Language::English) {
            std::cout << "Usage: delete <breakpoint_id>" << std::endl;
        } else {
            std::cout << "用法: delete <断点ID>" << std::endl;
        }
        return;
    }

    try {
        size_t id = std::stoull(args[0]);
        if (m_bpMgr->removeBreakpoint(id)) {
            if (m_language == Language::English) {
                std::cout << "Breakpoint " << id << " deleted." << std::endl;
            } else {
                std::cout << "断点 " << id << " 已删除。" << std::endl;
            }
        } else {
            if (m_language == Language::English) {
                std::cout << "Breakpoint " << id << " does not exist."
                          << std::endl;
            } else {
                std::cout << "断点 " << id << " 不存在。" << std::endl;
            }
        }
    } catch (...) {
        if (m_language == Language::English) {
            std::cout << "Invalid breakpoint ID: " << args[0] << std::endl;
        } else {
            std::cout << "无效的断点ID: " << args[0] << std::endl;
        }
    }
}

void CLIDebugger::cmdDisable(const std::vector<std::string>& args)
{
    if (args.empty()) {
        if (m_language == Language::English) {
            std::cout << "Usage: disable <breakpoint_id>" << std::endl;
        } else {
            std::cout << "用法: disable <断点ID>" << std::endl;
        }
        return;
    }

    try {
        size_t id = std::stoull(args[0]);
        m_bpMgr->disableBreakpoint(id);
        if (m_language == Language::English) {
            std::cout << "Breakpoint " << id << " disabled." << std::endl;
        } else {
            std::cout << "断点 " << id << " 已禁用。" << std::endl;
        }
    } catch (...) {
        if (m_language == Language::English) {
            std::cout << "Invalid breakpoint ID: " << args[0] << std::endl;
        } else {
            std::cout << "无效的断点ID: " << args[0] << std::endl;
        }
    }
}

void CLIDebugger::cmdEnable(const std::vector<std::string>& args)
{
    if (args.empty()) {
        if (m_language == Language::English) {
            std::cout << "Usage: enable <breakpoint_id>" << std::endl;
        } else {
            std::cout << "用法: enable <断点ID>" << std::endl;
        }
        return;
    }

    try {
        size_t id = std::stoull(args[0]);
        m_bpMgr->enableBreakpoint(id);
        if (m_language == Language::English) {
            std::cout << "Breakpoint " << id << " enabled." << std::endl;
        } else {
            std::cout << "断点 " << id << " 已启用。" << std::endl;
        }
    } catch (...) {
        if (m_language == Language::English) {
            std::cout << "Invalid breakpoint ID: " << args[0] << std::endl;
        } else {
            std::cout << "无效的断点ID: " << args[0] << std::endl;
        }
    }
}

void CLIDebugger::cmdInfoBreakpoints(const std::vector<std::string>& /*args*/)
{
    showBreakpoints();
}

void CLIDebugger::cmdList(const std::vector<std::string>& args)
{
    size_t line = 0;

    if (args.empty()) {
        auto loc = m_vm->getCurrentLocation();
        line = loc.line;
    } else {
        try {
            line = std::stoull(args[0]);
        } catch (...) {
            if (m_language == Language::English) {
                std::cout << "Invalid line number: " << args[0] << std::endl;
            } else {
                std::cout << "无效的行号: " << args[0] << std::endl;
            }
            return;
        }
    }

    showSourceContext(line, 10);
}

void CLIDebugger::cmdPrint(const std::vector<std::string>& args)
{
    if (args.empty()) {
        if (m_language == Language::English) {
            std::cout
                << "Usage: print <variable_name> or print <expression>"
                << std::endl;
        } else {
            std::cout << "用法: print <变量名> 或 print <表达式>" << std::endl;
        }
        return;
    }

    std::string expr = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
        expr += " " + args[i];
    }

    auto evaluator = m_vm->getExpressionEvaluator();
    auto result =
        evaluator->evaluate(expr, m_vm->getMainStack(), m_vm->getPC());

    if (result.success) {
        std::cout << expr << " = " << result.value << " (" << result.type << ")"
                  << std::endl;
    } else {
        if (m_language == Language::English) {
            std::cout << "Evaluation failed: " << result.errorMessage
                      << std::endl;
        } else {
            std::cout << "求值失败: " << result.errorMessage << std::endl;
        }
    }
}

void CLIDebugger::cmdLocals(const std::vector<std::string>& /*args*/)
{
    auto varInspector = m_vm->getVariableInspector();
    auto localVars =
        varInspector->getLocalVariables(m_vm->getMainStack(), m_vm->getPC());

    // 退化到全部变量，避免显示 "局部变量 (0 个)" 造成误导
    if (localVars.empty()) {
        localVars =
            varInspector->getAllVariables(m_vm->getMainStack(), m_vm->getPC());
    }

    if (m_language == Language::English) {
        showVariables(localVars, "Local variables");
    } else {
        showVariables(localVars, "局部变量");
    }
}

void CLIDebugger::cmdBacktrace(const std::vector<std::string>& /*args*/)
{
    showCallStack();
}

void CLIDebugger::cmdInfo(const std::vector<std::string>& args)
{
    if (args.empty()) {
        if (m_language == Language::English) {
            std::cout << "Usage: info <type>" << std::endl;
            std::cout << "Types: breakpoints, stack, variables, scope"
                      << std::endl;
        } else {
            std::cout << "用法: info <类型>" << std::endl;
            std::cout << "类型: breakpoints, stack, variables, scope"
                      << std::endl;
        }
        return;
    }

    std::string type = args[0];
    std::transform(type.begin(), type.end(), type.begin(), ::tolower);

    if (type == "breakpoints" || type == "bp") {
        showBreakpoints();
    } else if (type == "stack") {
        showStackState();
    } else if (type == "variables" || type == "vars") {
        auto varInspector = m_vm->getVariableInspector();
        auto allVars =
            varInspector->getAllVariables(m_vm->getMainStack(), m_vm->getPC());
        if (m_language == Language::English) {
            showVariables(allVars, "All variables");
        } else {
            showVariables(allVars, "所有变量");
        }
    } else if (type == "scope") {
        auto scopeInspector = m_vm->getScopeInspector();
        std::string desc = scopeInspector->getScopeDescription(m_vm->getPC());
        if (m_language == Language::English) {
            std::cout << "Current scope: " << desc << std::endl;
        } else {
            std::cout << "当前作用域: " << desc << std::endl;
        }
    } else {
        if (m_language == Language::English) {
            std::cout << "Unknown info type: " << type << std::endl;
        } else {
            std::cout << "未知的信息类型: " << type << std::endl;
        }
    }
}

void CLIDebugger::cmdBytecode(const std::vector<std::string>& args)
{
    size_t context = 0;
    if (!args.empty()) {
        try {
            context = std::stoull(args[0]);
        } catch (...) {
            if (m_language == Language::English) {
                std::cout << "Invalid argument: " << args[0] << std::endl;
                std::cout << "Usage: bytecode [context_size]" << std::endl;
            } else {
                std::cout << "无效的参数: " << args[0] << std::endl;
                std::cout << "用法: bytecode [上下文条数]" << std::endl;
            }
            return;
        }
    }
    showBytecode(context);
}

void CLIDebugger::cmdHelp(const std::vector<std::string>& /*args*/)
{
    if (m_language == Language::English) {
        std::cout << "\nAvailable commands:\n" << std::endl;
        std::cout << "Execution control:" << std::endl;
        std::cout << "  run, r              - Start/restart execution"
                  << std::endl;
        std::cout << "  reset               - Reset VM to initial state"
                  << std::endl;
        std::cout << "  continue, c         - Continue execution" << std::endl;
        std::cout << "  step, s             - Step in" << std::endl;
        std::cout << "  next, n             - Step over" << std::endl;
        std::cout << "  finish, f           - Step out" << std::endl;
        std::cout << "  pause               - Pause execution" << std::endl;
        std::cout << std::endl;
        std::cout << "Breakpoint management:" << std::endl;
        std::cout << "  break, b <line>     - Set breakpoint at line"
                  << std::endl;
        std::cout << "  break, b <func>     - Set breakpoint at function entry"
                  << std::endl;
        std::cout << "  delete, d <ID>      - Delete breakpoint" << std::endl;
        std::cout << "  disable <ID>        - Disable breakpoint" << std::endl;
        std::cout << "  enable <ID>         - Enable breakpoint" << std::endl;
        std::cout << "  info breakpoints    - Show all breakpoints"
                  << std::endl;
        std::cout << std::endl;
        std::cout << "Inspect information:" << std::endl;
        std::cout << "  list, l [line]      - Show source" << std::endl;
        std::cout << "  stack [args]        - Show stack state" << std::endl;
        std::cout << "  backtrace, bt       - Show call stack" << std::endl;
        std::cout << "  bytecode, bc [N]    - Show bytecode (optional context)"
                  << std::endl;
        std::cout << std::endl;
        std::cout << "Transaction data:" << std::endl;
        std::cout << "  settxfile <file>    - Load transaction data from file"
                  << std::endl;
        std::cout << "  showtx              - Show current transaction data"
                  << std::endl;
        std::cout << std::endl;
        std::cout << "Other:" << std::endl;
        std::cout << "  help, h             - Show this help" << std::endl;
        std::cout << "  quit, q             - Exit debugger" << std::endl;
        std::cout << "  clear               - Clear screen" << std::endl;
        std::cout << "  lang [zh|en]        - Switch CLI language"
                  << std::endl;
        std::cout << std::endl;

        std::cout << "stack arguments:" << std::endl;
        std::cout << "  stack                     - Show main/alt stacks "
                     "(hex, full)"
                  << std::endl;
        std::cout << "  stack main|alt|both       - Select stacks to show"
                  << std::endl;
        std::cout << "  stack --near <H> [C]      - Show data near height H "
                     "(H=0 bottom; C is context, default 5)"
                  << std::endl;
        std::cout << "  stack --range <A:B>       - Show height range [A,B] "
                     "(inclusive; 0 is bottom)"
                  << std::endl;
    } else {
        std::cout << "\n可用命令:\n" << std::endl;
        std::cout << "执行控制:" << std::endl;
        std::cout << "  run, r              - 开始/重新开始执行" << std::endl;
        std::cout << "  reset               - 重置虚拟机到初始状态" << std::endl;
        std::cout << "  continue, c         - 继续执行" << std::endl;
        std::cout << "  step, s             - 单步进入" << std::endl;
        std::cout << "  next, n             - 单步跳过" << std::endl;
        std::cout << "  finish, f           - 跳出函数" << std::endl;
        std::cout << "  pause               - 暂停执行" << std::endl;
        std::cout << std::endl;
        std::cout << "断点管理:" << std::endl;
        std::cout << "  break, b <行号>     - 在指定行设置断点" << std::endl;
        std::cout << "  break, b <函数名>   - 在函数入口设置断点" << std::endl;
        std::cout << "  delete, d <ID>      - 删除断点" << std::endl;
        std::cout << "  disable <ID>        - 禁用断点" << std::endl;
        std::cout << "  enable <ID>         - 启用断点" << std::endl;
        std::cout << "  info breakpoints    - 显示所有断点" << std::endl;
        std::cout << std::endl;
        std::cout << "查看信息:" << std::endl;
        std::cout << "  list, l [行号]      - 显示源码" << std::endl;
        std::cout << "  stack [参数]        - 显示栈状态" << std::endl;
        std::cout << "  backtrace, bt       - 显示调用栈" << std::endl;
        std::cout << "  bytecode, bc [N]    - 显示字节码（可选上下文条数）"
                  << std::endl;
        std::cout << std::endl;
        std::cout << "交易数据管理:" << std::endl;
        std::cout << "  settxfile <文件>    - 从文件加载交易数据" << std::endl;
        std::cout << "  showtx              - 显示当前交易数据" << std::endl;
        std::cout << std::endl;
        std::cout << "其他:" << std::endl;
        std::cout << "  help, h             - 显示此帮助" << std::endl;
        std::cout << "  quit, q             - 退出调试器" << std::endl;
        std::cout << "  clear               - 清屏" << std::endl;
        std::cout << "  lang [zh|en]        - 切换 CLI 语言" << std::endl;
        std::cout << std::endl;

        std::cout << "stack 参数:" << std::endl;
        std::cout
            << "  stack                     - 显示主栈/副栈（默认十六进制，全量）"
            << std::endl;
        std::cout << "  stack main|alt|both       - 选择显示主栈/副栈/两者"
                  << std::endl;
        std::cout << "  stack --near <H> [C]      - 显示指定高度 H "
                     "附近的数据（H=0 为栈底；"
                     "可选 C 为上下文条数，默认 5）"
                  << std::endl;
        std::cout << "  stack --range <A:B>       - 显示高度范围 "
                     "[A,B]（含端点；A/B 都是高度，"
                     "0 为栈底）"
                  << std::endl;
    }
}

void CLIDebugger::cmdQuit(const std::vector<std::string>& /*args*/)
{
    if (m_language == Language::English) {
        std::cout << "Exit debugger." << std::endl;
    } else {
        std::cout << "退出调试器。" << std::endl;
    }
    m_running = false;
}

void CLIDebugger::cmdClear(const std::vector<std::string>& /*args*/)
{
    for (int i = 0; i < 50; ++i) {
        std::cout << std::endl;
    }
}

void CLIDebugger::cmdLanguage(const std::vector<std::string>& args)
{
    if (args.empty()) {
        if (m_language == Language::English) {
            std::cout << "Current CLI language: English" << std::endl;
            std::cout << "Usage: lang zh|en" << std::endl;
        } else {
            std::cout << "当前 CLI 语言: 中文" << std::endl;
            std::cout << "用法: lang zh|en" << std::endl;
        }
        return;
    }

    std::string code = args[0];
    std::transform(code.begin(), code.end(), code.begin(), ::tolower);

    if (code == "en" || code == "english") {
        m_language = Language::English;
        std::cout << "CLI language switched to English." << std::endl;
    } else if (code == "zh" || code == "cn" || code == "zh-cn" ||
               code == "chinese") {
        m_language = Language::Chinese;
        std::cout << "CLI 语言已切换为中文。" << std::endl;
    } else {
        if (m_language == Language::English) {
            std::cout << "Unsupported language code: " << args[0] << std::endl;
            std::cout << "Use 'zh' or 'en'." << std::endl;
        } else {
            std::cout << "不支持的语言代码: " << args[0] << std::endl;
            std::cout << "请使用 'zh' 或 'en'。" << std::endl;
        }
    }
}

namespace
{
    using apc::util::toLower;
    using apc::util::trim;

    // 支持十进制与 0x 十六进制
    bool parseUint32(const std::string& s, uint32_t& out)
    {
        std::string t = trim(s);
        if (t.empty()) {
            return false;
        }
        unsigned long long val = 0;
        try {
            size_t pos = 0;
            if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
                val = std::stoull(t, &pos, 16);
            } else {
                val = std::stoull(t, &pos, 10);
            }
            if (val > 0xFFFFFFFFUL) {
                return false;
            }
            out = static_cast<uint32_t>(val);
            return true;
        } catch (...) {
            return false;
        }
    }

    void printHexBytes(const std::vector<uint8_t>& bytes)
    {
        for (uint8_t b : bytes) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(b);
        }
        std::cout << std::dec;
    }

    // 4 字节小端十六进制，对齐 OP_PUSH_META 的 memcpy 行为
    void printUint32AsHexBytes(uint32_t value)
    {
        uint8_t bytes[4];
        std::memcpy(bytes, &value, 4);
        for (uint8_t b : bytes) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(b);
        }
        std::cout << std::dec;
    }
} // namespace

void CLIDebugger::cmdSetTxFile(const std::vector<std::string>& args)
{
    if (args.empty()) {
        if (m_language == Language::English) {
            std::cout << "Usage: settxfile <filename>" << std::endl;
            std::cout << "Example: settxfile transaction.txt" << std::endl;
            std::cout << "File format: key-value pairs, each line KEY:VALUE, "
                         "# for inline comments."
                      << std::endl;
            std::cout
                << "Supported keys: version, locktime, inputCount, outputCount, "
                   "inputsHash, unlockingInput, outputsHash"
                << std::endl;
        } else {
            std::cout << "用法: settxfile <文件名>" << std::endl;
            std::cout << "示例: settxfile transaction.txt" << std::endl;
            std::cout << "文件格式为键值对，每行 KEY:VALUE，# 为行内注释。"
                      << std::endl;
            std::cout << "支持的键: version, locktime, inputCount, outputCount, "
                         "inputsHash, unlockingInput, outputsHash"
                      << std::endl;
        }
        return;
    }

    std::string filename = args[0];

    std::ifstream file(filename);
    if (!file.is_open()) {
        if (m_language == Language::English) {
            std::cout << "Error: failed to open file '" << filename << "'"
                      << std::endl;
        } else {
            std::cout << "错误: 无法打开文件 '" << filename << "'" << std::endl;
        }
        return;
    }

    TransactionData txData;
    txData.version = 1;
    txData.lockTime = 0;

    std::string line;
    int lineNum = 0;
    while (std::getline(file, line)) {
        ++lineNum;
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            if (m_language == Language::English) {
                std::cout << "Warning: line " << lineNum
                          << " has invalid format, ignored" << std::endl;
            } else {
                std::cout << "警告: 第 " << lineNum << " 行格式无效，已忽略"
                          << std::endl;
            }
            continue;
        }

        std::string key = toLower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));

        if (key == "version") {
            uint32_t v = 0;
            if (!parseUint32(value, v)) {
                if (m_language == Language::English) {
                    std::cout << "Error: line " << lineNum
                              << " has invalid version value" << std::endl;
                } else {
                    std::cout << "错误: 第 " << lineNum << " 行 version 值无效"
                              << std::endl;
                }
                file.close();
                return;
            }
            txData.version = v;
        } else if (key == "locktime") {
            uint32_t v = 0;
            if (!parseUint32(value, v)) {
                if (m_language == Language::English) {
                    std::cout << "Error: line " << lineNum
                              << " has invalid locktime value" << std::endl;
                } else {
                    std::cout << "错误: 第 " << lineNum
                              << " 行 locktime 值无效" << std::endl;
                }
                file.close();
                return;
            }
            txData.lockTime = v;
        } else if (key == "inputcount") {
            uint32_t n = 0;
            if (!parseUint32(value, n)) {
                if (m_language == Language::English) {
                    std::cout << "Error: line " << lineNum
                              << " has invalid inputCount value" << std::endl;
                } else {
                    std::cout << "错误: 第 " << lineNum
                              << " 行 inputCount 值无效" << std::endl;
                }
                file.close();
                return;
            }
            txData.inputCount = n;
        } else if (key == "outputcount") {
            uint32_t n = 0;
            if (!parseUint32(value, n)) {
                if (m_language == Language::English) {
                    std::cout << "Error: line " << lineNum
                              << " has invalid outputCount value" << std::endl;
                } else {
                    std::cout << "错误: 第 " << lineNum
                              << " 行 outputCount 值无效" << std::endl;
                }
                file.close();
                return;
            }
            txData.outputCount = n;
        } else if (key == "inputshash") {
            txData.inputsHash = StackElement::fromHexLiteral(value).data;
            if (txData.inputsHash.size() != 32 && !txData.inputsHash.empty()) {
                if (m_language == Language::English) {
                    std::cout << "Warning: line " << lineNum
                              << " inputsHash is usually 32 bytes" << std::endl;
                } else {
                    std::cout << "警告: 第 " << lineNum
                              << " 行 inputsHash 通常为 32 字节" << std::endl;
                }
            }
        } else if (key == "unlockinginput") {
            txData.unlockingInput = StackElement::fromHexLiteral(value).data;
        } else if (key == "outputshash") {
            txData.outputsHash = StackElement::fromHexLiteral(value).data;
            if (txData.outputsHash.size() != 32 &&
                !txData.outputsHash.empty()) {
                if (m_language == Language::English) {
                    std::cout << "Warning: line " << lineNum
                              << " outputsHash is usually 32 bytes"
                              << std::endl;
                } else {
                    std::cout << "警告: 第 " << lineNum
                              << " 行 outputsHash 通常为 32 字节" << std::endl;
                }
            }
        } else {
            if (m_language == Language::English) {
                std::cout << "Warning: line " << lineNum << " unknown key '"
                          << key << "', ignored" << std::endl;
            } else {
                std::cout << "警告: 第 " << lineNum << " 行未知键 '" << key
                          << "'，已忽略" << std::endl;
            }
        }
    }
    file.close();

    m_vm->setTransactionData(txData);
    if (m_language == Language::English) {
        std::cout << "Transaction data loaded." << std::endl;
        std::cout << "Use 'showtx' to view transaction details." << std::endl;
    } else {
        std::cout << "交易数据已加载" << std::endl;
        std::cout << "使用 'showtx' 查看交易详情" << std::endl;
    }
}

void CLIDebugger::cmdShowTx(const std::vector<std::string>& /*args*/)
{
    const TransactionData* txData = m_vm->getTransactionData();

    if (!txData) {
        if (m_language == Language::English) {
            std::cout << "No transaction data loaded." << std::endl;
            std::cout << "Use 'settxfile <filename>' to load transaction data."
                      << std::endl;
        } else {
            std::cout << "未加载交易数据" << std::endl;
            std::cout << "使用 'settxfile <文件名>' 加载交易数据" << std::endl;
        }
        return;
    }

    // 先换行，避免与调试器提示混在一行
    std::cout << std::endl;

    std::cout << "version: ";
    printUint32AsHexBytes(txData->version);
    std::cout << std::endl;

    std::cout << "locktime: ";
    printUint32AsHexBytes(txData->lockTime);
    std::cout << std::endl;

    std::cout << "inputCount: ";
    printUint32AsHexBytes(txData->inputCount);
    std::cout << std::endl;

    std::cout << "outputCount: ";
    printUint32AsHexBytes(txData->outputCount);
    std::cout << std::endl;

    std::cout << "inputsHash: ";
    printHexBytes(txData->inputsHash);
    std::cout << std::endl;

    std::cout << "unlockingInput: ";
    printHexBytes(txData->unlockingInput);
    std::cout << std::endl;

    std::cout << "outputsHash: ";
    printHexBytes(txData->outputsHash);
    std::cout << std::endl;
}

void CLIDebugger::showCurrentLocation()
{
    auto loc = m_vm->getCurrentLocation();
    size_t pc = m_vm->getPC();

    if (m_language == Language::English) {
        std::cout << "\nCurrent location: " << loc.toString() << std::endl;
        std::cout << "PC: " << pc << std::endl;
    } else {
        std::cout << "\n当前位置: " << loc.toString() << std::endl;
        std::cout << "PC: " << pc << std::endl;
    }

    if (m_debugInfo && pc < m_vm->getBytecode().size()) {
        const auto& instInfo = m_debugInfo->instructions.find(pc);
        if (instInfo != m_debugInfo->instructions.end()) {
            std::cout << "指令: " << instInfo->second.opcode;
            if (!instInfo->second.operand.empty()) {
                std::cout << " " << instInfo->second.operand;
            }
            std::cout << std::endl;
        }
    }

    showSourceContext(loc.line);
}

void CLIDebugger::showSourceContext(size_t line, size_t contextLines)
{
    if (m_sourceLines.empty()) {
        if (m_language == Language::English) {
            std::cout << "(source not loaded)" << std::endl;
        } else {
            std::cout << "(源码未加载)" << std::endl;
        }
        return;
    }

    size_t startLine = (line > contextLines) ? (line - contextLines) : 1;
    size_t endLine = std::min(
        line + contextLines, static_cast<size_t>(m_sourceLines.size())
    );

    for (size_t i = startLine; i <= endLine; ++i) {
        std::string marker = (i == line) ? "=> " : "   ";
        std::cout << marker << std::setw(4) << i << " | ";

        if (i <= m_sourceLines.size()) {
            std::cout << m_sourceLines[i - 1] << std::endl;
        } else {
            std::cout << std::endl;
        }
    }
}

void CLIDebugger::showVariables(
    const std::vector<VariableValue>& vars,
    const std::string& title
)
{
    if (m_language == Language::English) {
        std::cout << "\n" << title << " (" << vars.size() << "):" << std::endl;
    } else {
        std::cout << "\n" << title << " (" << vars.size() << " 个):" << std::endl;
    }

    if (vars.empty()) {
        if (m_language == Language::English) {
            std::cout << "  (none)" << std::endl;
        } else {
            std::cout << "  (无)" << std::endl;
        }
        return;
    }

    size_t nameWidth = 0;
    size_t typeWidth = 0;
    for (const auto& var : vars) {
        nameWidth = std::max(nameWidth, var.name.length());
        typeWidth = std::max(typeWidth, var.type.length());
    }
    nameWidth = std::max(nameWidth, size_t(10));
    typeWidth = std::max(typeWidth, size_t(10));

    if (m_language == Language::English) {
        std::cout << "  " << std::setw(nameWidth) << std::left << "Name"
                  << "  " << std::setw(typeWidth) << std::left << "Type"
                  << "  Value" << std::endl;
    } else {
        std::cout << "  " << std::setw(nameWidth) << std::left << "名称"
                  << "  " << std::setw(typeWidth) << std::left << "类型"
                  << "  值" << std::endl;
    }
    std::cout << "  " << std::string(nameWidth, '-') << "  "
              << std::string(typeWidth, '-') << "  " << std::string(30, '-')
              << std::endl;

    for (const auto& var : vars) {
        std::cout << "  " << std::setw(nameWidth) << std::left << var.name
                  << "  " << std::setw(typeWidth) << std::left << var.type
                  << "  " << var.value << std::endl;
    }
}

void CLIDebugger::showCallStack()
{
    const auto& callStack = m_vm->getCallStack();

    if (m_language == Language::English) {
        std::cout << "\nCall stack (" << callStack.size() << " frames):"
                  << std::endl;
    } else {
        std::cout << "\n调用栈 (" << callStack.size() << " 层):" << std::endl;
    }

    if (callStack.empty()) {
        if (m_language == Language::English) {
            std::cout << "  (empty)" << std::endl;
        } else {
            std::cout << "  (空)" << std::endl;
        }
        return;
    }

    for (size_t i = 0; i < callStack.size(); ++i) {
        const auto& frame = callStack[i];
        std::cout << "  #" << i << " " << frame.functionName << " at "
                  << frame.callLocation.toString() << std::endl;
    }
}

void CLIDebugger::showStackState()
{
    StackViewOptions opt;
    showStackState(opt);
}

namespace
{
    bool parseSizeT(const std::string& s, size_t& out)
    {
        try {
            size_t pos = 0;
            unsigned long long v = std::stoull(s, &pos, 10);
            if (pos != s.size()) {
                return false;
            }
            out = static_cast<size_t>(v);
            return true;
        } catch (...) {
            return false;
        }
    }

    // "A:B"，两端不可省略
    bool
    parseDepthRange(const std::string& s, size_t& startDepth, size_t& endDepth)
    {
        auto colon = s.find(':');
        if (colon == std::string::npos) {
            return false;
        }

        std::string a = s.substr(0, colon);
        std::string b = s.substr(colon + 1);

        if (a.empty() || b.empty()) {
            return false;
        }
        size_t va = 0;
        size_t vb = 0;
        if (!parseSizeT(a, va) || !parseSizeT(b, vb)) {
            return false;
        }
        startDepth = va;
        endDepth = vb;
        return true;
    }

    std::string lowerCopy(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }
} // namespace

void CLIDebugger::cmdStack(const std::vector<std::string>& args)
{
    StackViewOptions opt;

    // 无参数等价于 both + 全量；--near/--range 中高度以栈底为 0
    for (size_t i = 0; i < args.size(); ++i) {
        std::string a = args[i];
        std::string al = lowerCopy(a);

        if (al == "main") {
            opt.target = StackViewOptions::Target::MAIN;
            continue;
        }
        if (al == "alt" || al == "altstack") {
            opt.target = StackViewOptions::Target::ALT;
            continue;
        }
        if (al == "both") {
            opt.target = StackViewOptions::Target::BOTH;
            continue;
        }

        if (a == "--range" || a == "-r") {
            if (i + 1 >= args.size()) {
                if (m_language == Language::English) {
                    std::cout << "Usage: stack --range <A:B>" << std::endl;
                } else {
                    std::cout << "用法: stack --range <A:B>" << std::endl;
                }
                return;
            }
            size_t startD = 0;
            size_t endD = 0;
            if (!parseDepthRange(args[i + 1], startD, endD)) {
                if (m_language == Language::English) {
                    std::cout << "Invalid --range argument: " << args[i + 1]
                              << std::endl;
                } else {
                    std::cout << "无效的 --range 参数: " << args[i + 1]
                              << std::endl;
                }
                return;
            }
            opt.mode = StackViewOptions::ViewMode::DEPTH_RANGE;
            opt.rangeStartDepth = startD;
            opt.rangeEndDepth = endD;
            ++i;
            continue;
        }

        if (a == "--near") {
            if (i + 1 >= args.size()) {
                if (m_language == Language::English) {
                    std::cout << "Usage: stack --near <H> [C]" << std::endl;
                } else {
                    std::cout << "用法: stack --near <H> [C]" << std::endl;
                }
                return;
            }
            size_t h = 0;
            if (!parseSizeT(args[i + 1], h)) {
                if (m_language == Language::English) {
                    std::cout << "Invalid --near argument: " << args[i + 1]
                              << std::endl;
                } else {
                    std::cout << "无效的 --near 参数: " << args[i + 1]
                              << std::endl;
                }
                return;
            }
            size_t c = 5;
            if (i + 2 < args.size()) {
                size_t maybe = 0;
                if (parseSizeT(args[i + 2], maybe)) {
                    c = maybe;
                    ++i;
                }
            }
            opt.mode = StackViewOptions::ViewMode::NEAR_DEPTH;
            opt.nearDepth = h;
            opt.nearContext = c;
            ++i;
            continue;
        }

        if (m_language == Language::English) {
            std::cout << "Unknown stack argument: " << a << std::endl;
            std::cout
                << "Hint: type 'help' to see how to use stack arguments"
                << std::endl;
        } else {
            std::cout << "未知 stack 参数: " << a << std::endl;
            std::cout << "提示: 输入 'help' 查看 stack 参数用法" << std::endl;
        }
        return;
    }

    showStackState(opt);
}

void CLIDebugger::showStackState(const StackViewOptions& opt)
{
    auto printOneStack = [&](const char* title, const StackState& s) {
        if (m_language == Language::English) {
            std::cout << "\n" << title << " (top to bottom):" << std::endl;
        } else {
            std::cout << "\n" << title << " (从顶到底):" << std::endl;
        }

        if (s.empty()) {
            if (m_language == Language::English) {
                std::cout << "  (empty)" << std::endl;
            } else {
                std::cout << "  (空)" << std::endl;
            }
            return;
        }

        // 从栈顶到栈底；index 以栈底为 0
        size_t shown = 0;
        for (size_t depth = 0; depth < s.size(); ++depth) {
            size_t index = s.size() - 1 - depth;

            if (opt.mode == StackViewOptions::ViewMode::NEAR_DEPTH) {
                size_t lo = (opt.nearDepth > opt.nearContext)
                                ? (opt.nearDepth - opt.nearContext)
                                : 0;
                size_t hi = opt.nearDepth + opt.nearContext;
                if (index < lo || index > hi) {
                    continue;
                }
            } else if (opt.mode == StackViewOptions::ViewMode::DEPTH_RANGE) {
                size_t a = opt.rangeStartDepth;
                size_t b = opt.rangeEndDepth;
                if (a > b) {
                    std::swap(a, b);
                }
                if (index < a || index > b) {
                    continue;
                }
            }

            const auto& elem = s.peek(depth);
            std::cout << "  [" << index << "] ";
            std::cout << elem.toHexString(true);
            std::cout << std::endl;
            ++shown;
        }

        if (shown == 0) {
            if (m_language == Language::English) {
                std::cout << "  (no elements in range)" << std::endl;
            } else {
                std::cout << "  (范围内无元素)" << std::endl;
            }
        }
    };

    if (opt.target == StackViewOptions::Target::MAIN ||
        opt.target == StackViewOptions::Target::BOTH) {
        if (m_language == Language::English) {
            printOneStack("Main stack", m_vm->getMainStack());
        } else {
            printOneStack("主栈", m_vm->getMainStack());
        }
    }

    if (opt.target == StackViewOptions::Target::ALT ||
        opt.target == StackViewOptions::Target::BOTH) {
        if (m_language == Language::English) {
            printOneStack("Alt stack", m_vm->getAltStack());
        } else {
            printOneStack("副栈", m_vm->getAltStack());
        }
    }
}

void CLIDebugger::showBreakpoints()
{
    auto allBps = m_bpMgr->getAllBreakpoints();

    if (m_language == Language::English) {
        std::cout << "\nBreakpoints (" << allBps.size() << "):" << std::endl;
    } else {
        std::cout << "\n断点 (" << allBps.size() << " 个):" << std::endl;
    }

    if (allBps.empty()) {
        if (m_language == Language::English) {
            std::cout << "  (none)" << std::endl;
        } else {
            std::cout << "  (无)" << std::endl;
        }
        return;
    }

    for (const auto& bp : allBps) {
        std::cout << "  [" << bp->getId() << "] " << bp->getDescription();
        std::cout << " - " << breakpointStateToString(bp->getState());
        std::cout << " (命中 " << bp->getHitCount() << " 次)" << std::endl;
    }
}

void CLIDebugger::showBytecode(size_t context)
{
    const auto& bytecode = m_vm->getBytecode();
    size_t pc = m_vm->getPC();

    if (bytecode.empty()) {
        std::cout << "\n(当前无字节码)" << std::endl;
        return;
    }

    size_t start = 0;
    size_t end = bytecode.size();

    // 设了执行范围则只显示 [startPC, endPC)
    if (m_vm->hasExecutionRange()) {
        start = std::min(m_vm->getExecutionRangeStart(), bytecode.size());
        end = std::min(m_vm->getExecutionRangeEnd(), bytecode.size());
        if (start > end) {
            start = end;
        }
    }

    if (context > 0) {
        // 仅显示当前 PC 附近，并裁剪到 [start, end)
        size_t ctxStart = 0;
        size_t ctxEnd = bytecode.size();

        if (pc >= context) {
            ctxStart = pc - context;
        }
        ctxEnd = std::min(pc + context + 1, bytecode.size());

        ctxStart = std::max(ctxStart, start);
        ctxEnd = std::min(ctxEnd, end);

        start = ctxStart;
        end = ctxEnd;
    }

    size_t totalShownRange = (end >= start) ? (end - start) : 0;
    if (m_vm->hasExecutionRange()) {
        if (m_language == Language::English) {
            std::cout << "\nBytecode list (function range total "
                      << totalShownRange << ", current PC = " << pc << "):"
                      << std::endl;
        } else {
            std::cout << "\n字节码列表 (函数范围共 " << totalShownRange
                      << " 条，当前PC = " << pc << "):" << std::endl;
        }
    } else {
        if (m_language == Language::English) {
            std::cout << "\nBytecode list (total " << bytecode.size()
                      << ", current PC = " << pc << "):" << std::endl;
        } else {
            std::cout << "\n字节码列表 (共 " << bytecode.size()
                      << " 条，当前PC = " << pc << "):" << std::endl;
        }
    }
    if (m_language == Language::English) {
        std::cout << "Format: PC | Source line | Instruction" << std::endl;
    } else {
        std::cout << "格式: PC | 源码行号 | 指令" << std::endl;
    }
    std::cout << std::string(60, '-') << std::endl;

    for (size_t i = start; i < end; ++i) {
        std::string marker = (i == pc) ? "=> " : "   ";
        std::cout << marker << std::setw(4) << i << " | ";

        if (m_debugInfo) {
            auto loc = m_debugInfo->getSourceLocation(i);
            if (loc.isValid()) {
                std::cout << std::setw(4) << loc.line << " | ";
            } else {
                std::cout << "  -  | ";
            }
        } else {
            std::cout << "  -  | ";
        }

        std::cout << bytecode[i] << std::endl;
    }
}

std::vector<std::string> CLIDebugger::parseArgs(const std::string& args)
{
    std::vector<std::string> result;
    std::istringstream iss(args);
    std::string arg;

    while (iss >> arg) {
        result.push_back(arg);
    }

    return result;
}

std::map<std::string, CLIDebugger::CommandHandler> CLIDebugger::buildCommandMap(
)
{
    std::map<std::string, CommandHandler> map;

    // 执行控制
    map["run"] = [this](const auto& args) { cmdRun(args); };
    map["r"] = [this](const auto& args) { cmdRun(args); };
    map["reset"] = [this](const auto& args) { cmdReset(args); };
    map["continue"] = [this](const auto& args) { cmdContinue(args); };
    map["c"] = [this](const auto& args) { cmdContinue(args); };
    map["step"] = [this](const auto& args) { cmdStepIn(args); };
    map["s"] = [this](const auto& args) { cmdStepIn(args); };
    map["next"] = [this](const auto& args) { cmdStepOver(args); };
    map["n"] = [this](const auto& args) { cmdStepOver(args); };
    map["finish"] = [this](const auto& args) { cmdStepOut(args); };
    map["f"] = [this](const auto& args) { cmdStepOut(args); };
    map["pause"] = [this](const auto& args) { cmdPause(args); };

    // 断点管理
    map["break"] = [this](const auto& args) { cmdBreak(args); };
    map["b"] = [this](const auto& args) { cmdBreak(args); };
    map["delete"] = [this](const auto& args) { cmdDelete(args); };
    map["d"] = [this](const auto& args) { cmdDelete(args); };
    map["disable"] = [this](const auto& args) { cmdDisable(args); };
    map["enable"] = [this](const auto& args) { cmdEnable(args); };
    map["info"] = [this](const auto& args) { cmdInfo(args); };
    map["i"] = [this](const auto& args) { cmdInfo(args); };

    // 查看信息
    map["list"] = [this](const auto& args) { cmdList(args); };
    map["l"] = [this](const auto& args) { cmdList(args); };
    map["print"] = [this](const auto& args) { cmdPrint(args); };
    map["p"] = [this](const auto& args) { cmdPrint(args); };
    map["locals"] = [this](const auto& args) { cmdLocals(args); };
    map["stack"] = [this](const auto& args) { cmdStack(args); };
    map["backtrace"] = [this](const auto& args) { cmdBacktrace(args); };
    map["bt"] = [this](const auto& args) { cmdBacktrace(args); };
    map["bytecode"] = [this](const auto& args) { cmdBytecode(args); };
    map["bc"] = [this](const auto& args) { cmdBytecode(args); };

    // 其他
    map["help"] = [this](const auto& args) { cmdHelp(args); };
    map["h"] = [this](const auto& args) { cmdHelp(args); };
    map["quit"] = [this](const auto& args) { cmdQuit(args); };
    map["q"] = [this](const auto& args) { cmdQuit(args); };
    map["exit"] = [this](const auto& args) { cmdQuit(args); };
    map["clear"] = [this](const auto& args) { cmdClear(args); };
    map["lang"] = [this](const auto& args) { cmdLanguage(args); };
    map["language"] = [this](const auto& args) { cmdLanguage(args); };

    // 交易数据管理
    map["settxfile"] = [this](const auto& args) { cmdSetTxFile(args); };
    map["showtx"] = [this](const auto& args) { cmdShowTx(args); };

    return map;
}

void CLIDebugger::setSourceFile(const std::string& filename)
{
    m_sourceFilename = filename;
    loadSourceFile(filename);
}

bool CLIDebugger::loadSourceFile(const std::string& filename)
{
    return readSourceLines(filename);
}

bool CLIDebugger::readSourceLines(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    m_sourceLines.clear();
    std::string line;
    while (std::getline(file, line)) {
        m_sourceLines.push_back(line);
    }

    file.close();
    return true;
}

} // namespace apc_debug
