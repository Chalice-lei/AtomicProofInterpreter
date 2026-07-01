#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "src/compiler/bytecode_pipeline.h"
#include "src/compiler/frontend_pipeline.h"
#include "src/compiler_repl/repl_frontend.h"
#include "src/config/config_manager.h"
#include "src/error/error_manager.h"
#include "src/log/logger.h"
#include "src/pass/pass_context.h"
#include "src/interpreter/ast_interpreter.h"
#include "src/interpreter/ast_self_test.h"
#include "src/interpreter/function_selection.h"
#include "src/interpreter/parameter_schema.h"
#include "src/interpreter/runtime_argument.h"
#include "src/interpreter/runtime_self_test.h"
#include "src/interpreter/transaction_context.h"
#include "src/repl/repl_shell.h"
#include "src/util/help.h"

#ifdef ENABLE_DEBUGGER
#include <nlohmann/json.hpp>

#include "src/bytecode/script_decoder.h"
#include "src/debugger/breakpoint/breakpoint_manager.h"
#include "src/debugger/core/debugger_core.h"
#include "src/debugger/info/debug_info.h"
#include "src/debugger/interface/cli_debugger.h"
#include "src/debugger/protocol/live_debug_server.h"
#include "src/debugger/vm/bvm_simulator.h"
#include "src/debugger/vm/stack_argument.h"
#include "src/interpreter/bytecode_runner.h"
#endif

std::map<std::string, LogLevel> logLevelMap =
    {{"debug", LogLevel::DEBUG},
     {"info", LogLevel::INFO},
     {"warning", LogLevel::WARNING},
     {"error", LogLevel::ERROR},
     {"critical", LogLevel::CRITICAL},
     {"none", LogLevel::NONE}};

struct CommandLineArgs
{
    std::string filename{""};
    LogLevel logLevel;
    bool allowSubscopeAltstack{false};
    bool enableDebug{false};
    bool debuggerMode{false};
    bool liveDebugServerMode{false};
    bool runBytecode{false};
    bool runAST{false};
    bool replMode{false};
    bool compilerReplMode{false};
    bool runtimeSelfTest{false};
    bool astSelfTest{false};
    std::string debugOutputFile{""};
    std::string functionName{""};
    std::vector<std::string> runArgs;
    std::vector<std::string> paramAssignments;
    std::vector<std::string> selfAssignments;
    std::vector<std::string> bvmAssignments;
    std::string txFile{""};
    std::string stackTraceOutputFile{""};
};

enum class CommandLineMode
{
    None,
    Compile,
    RunBytecode,
    RunAST,
    Debug,
    DebugServer,
    Shell,
    CompilerRepl,
    Test
};

static std::string toLowerCopy(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
    );
    return value;
}

static bool hasCtExtension(const std::string& filename)
{
    return filename.length() >= 3 &&
           filename.substr(filename.length() - 3) == ".ct";
}

static std::string trimAsciiWhitespace(std::string value)
{
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

CommandLineArgs parseCommandLineArgs(int argc, char* argv[])
{
    CommandLineArgs args;

#ifndef NDEBUG
    args.logLevel = LogLevel::DEBUG;
#else
    args.logLevel = LogLevel::WARNING;
#endif

    if (argc < 2) {
        Help::showUsage(argv[0]);
        exit(1);
    }

    CommandLineMode mode = CommandLineMode::None;
    bool debugInfoOptionSeen = false;

    auto showErrorAndExit = [&](const std::string& message) {
        std::cerr << "Error: " << message << std::endl;
        Help::showUsage(argv[0]);
        exit(1);
    };

    auto requireValue = [&](int index, const std::string& option) {
        if (index + 1 >= argc) {
            showErrorAndExit(option + " requires a value");
        }
        return std::string(argv[index + 1]);
    };

    auto setLogLevel = [&](std::string levelStr) {
        levelStr = toLowerCopy(std::move(levelStr));
        auto it = logLevelMap.find(levelStr);
        if (it != logLevelMap.end()) {
            args.logLevel = it->second;
            return;
        }
        showErrorAndExit("Unrecognized log level '" + levelStr + "'");
    };

    auto activateCommand = [&](CommandLineMode newMode, const std::string& name) {
        (void)name;
        mode = newMode;
        switch (newMode) {
        case CommandLineMode::Compile:
            break;
        case CommandLineMode::RunBytecode:
            args.runBytecode = true;
            args.enableDebug = true;
            break;
        case CommandLineMode::RunAST:
            args.runAST = true;
            break;
        case CommandLineMode::Debug:
            args.debuggerMode = true;
            args.enableDebug = true;
            break;
        case CommandLineMode::DebugServer:
            args.liveDebugServerMode = true;
            args.enableDebug = true;
            break;
        case CommandLineMode::Shell:
            args.replMode = true;
            break;
        case CommandLineMode::CompilerRepl:
            args.compilerReplMode = true;
            break;
        case CommandLineMode::Test:
        case CommandLineMode::None:
            break;
        }
    };

    auto appendPositional = [&](const std::string& value) {
        switch (mode) {
        case CommandLineMode::None:
            showErrorAndExit(
                "Expected command: compile, run, ast, debug, debug-server, shell, compiler-repl, or test"
            );
            return;

        case CommandLineMode::Compile:
            if (args.filename.empty()) {
                args.filename = value;
                return;
            }
            showErrorAndExit("compile received an extra argument '" + value + "'");
            return;

        case CommandLineMode::RunBytecode:
        case CommandLineMode::DebugServer:
        case CommandLineMode::RunAST:
            if (args.filename.empty()) {
                args.filename = value;
                return;
            }
            if (args.functionName.empty()) {
                args.functionName = value;
                return;
            }
            args.runArgs.push_back(value);
            return;

        case CommandLineMode::Debug:
            if (args.filename.empty()) {
                args.filename = value;
                return;
            }
            showErrorAndExit("debug received an extra argument '" + value + "'");
            return;

        case CommandLineMode::Shell:
            if (args.filename.empty()) {
                args.filename = value;
                return;
            }
            showErrorAndExit("shell received an extra argument '" + value + "'");
            return;

        case CommandLineMode::CompilerRepl:
            showErrorAndExit(
                "compiler-repl received an extra argument '" + value + "'"
            );
            return;

        case CommandLineMode::Test: {
            std::string target = toLowerCopy(value);
            if (args.runtimeSelfTest || args.astSelfTest) {
                showErrorAndExit("test received an extra argument '" + value + "'");
            }
            if (target == "runtime") {
                args.runtimeSelfTest = true;
            } else if (target == "ast") {
                args.astSelfTest = true;
            } else {
                showErrorAndExit("test target must be 'runtime' or 'ast'");
            }
            return;
        }
        }
    };

    int i = 1;
    while (i < argc) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            Help::showUsage(argv[0]);
            exit(0);
        }
        else if (arg == "-v" || arg == "--version") {
            Help::showVersion();
            exit(0);
        }
        else if (mode == CommandLineMode::None &&
                 arg == "compile") {
            activateCommand(CommandLineMode::Compile, arg);
            ++i;
        }
        else if (mode == CommandLineMode::None &&
                 arg == "run") {
            activateCommand(CommandLineMode::RunBytecode, arg);
            ++i;
        }
        else if (mode == CommandLineMode::None &&
                 arg == "ast") {
            activateCommand(CommandLineMode::RunAST, arg);
            ++i;
        }
        else if (mode == CommandLineMode::None &&
                 arg == "debug") {
            activateCommand(CommandLineMode::Debug, arg);
            ++i;
        }
        else if (mode == CommandLineMode::None &&
                 arg == "debug-server") {
            activateCommand(CommandLineMode::DebugServer, arg);
            ++i;
        }
        else if (mode == CommandLineMode::None && arg == "shell") {
            activateCommand(CommandLineMode::Shell, arg);
            ++i;
        }
        else if (mode == CommandLineMode::None && arg == "compiler-repl") {
            activateCommand(CommandLineMode::CompilerRepl, arg);
            ++i;
        }
        else if (mode == CommandLineMode::None &&
                 arg == "test") {
            activateCommand(CommandLineMode::Test, arg);
            ++i;
        }
        else if (arg == "-l" || arg == "--log-level") {
            setLogLevel(requireValue(i, arg));
            i += 2;
        }
        else if (arg.rfind("--log-level=", 0) == 0) {
            setLogLevel(arg.substr(std::string("--log-level=").size()));
            ++i;
        }
        else if (arg == "--allow-subscope-altstack" || arg == "--asa") {
            args.allowSubscopeAltstack = true;
            ++i;
        }
        else if (arg == "--param") {
            args.paramAssignments.push_back(requireValue(i, arg));
            i += 2;
        }
        else if (arg.rfind("--param=", 0) == 0) {
            args.paramAssignments.push_back(arg.substr(std::string("--param=").size()));
            ++i;
        }
        else if (arg == "--self") {
            args.selfAssignments.push_back(requireValue(i, arg));
            i += 2;
        }
        else if (arg.rfind("--self=", 0) == 0) {
            args.selfAssignments.push_back(arg.substr(std::string("--self=").size()));
            ++i;
        }
        else if (arg == "--bvm") {
            args.bvmAssignments.push_back(requireValue(i, arg));
            i += 2;
        }
        else if (arg.rfind("--bvm=", 0) == 0) {
            args.bvmAssignments.push_back(arg.substr(std::string("--bvm=").size()));
            ++i;
        }
        else if (arg == "--txfile") {
            args.txFile = requireValue(i, arg);
            i += 2;
        }
        else if (arg.rfind("--txfile=", 0) == 0) {
            args.txFile = arg.substr(std::string("--txfile=").size());
            ++i;
        }
        else if (arg == "--stack-trace-output") {
            args.stackTraceOutputFile = requireValue(i, arg);
            args.enableDebug = true;
            i += 2;
        }
        else if (arg.rfind("--stack-trace-output=", 0) == 0) {
            args.stackTraceOutputFile =
                arg.substr(std::string("--stack-trace-output=").size());
            args.enableDebug = true;
            ++i;
        }
        else if (arg == "-d") {
            debugInfoOptionSeen = true;
            args.enableDebug = true;
            ++i;
        }
        else if (arg == "--debug-output") {
            debugInfoOptionSeen = true;
            args.debugOutputFile = requireValue(i, arg);
            args.enableDebug = true;
            i += 2;
        }
        else if (arg.rfind("--debug-output=", 0) == 0) {
            debugInfoOptionSeen = true;
            args.debugOutputFile =
                arg.substr(std::string("--debug-output=").size());
            args.enableDebug = true;
            ++i;
        }
        else if (arg == "--") {
            ++i;
            while (i < argc) {
                appendPositional(argv[i]);
                ++i;
            }
        }
        else if (arg.rfind("--", 0) == 0) {
            showErrorAndExit("Unrecognized option '" + arg + "'");
        }
        else {
            appendPositional(arg);
            ++i;
        }
    }

    if (mode == CommandLineMode::None) {
        showErrorAndExit(
            "Expected command: compile, run, ast, debug, debug-server, shell, compiler-repl, or test"
        );
    }

    if (mode == CommandLineMode::Test &&
        !(args.runtimeSelfTest || args.astSelfTest)) {
        showErrorAndExit("test requires a target: runtime or ast");
    }

    if (mode == CommandLineMode::Compile) {
        if (args.debuggerMode || args.liveDebugServerMode ||
            args.runBytecode || args.runAST ||
            args.replMode || args.compilerReplMode ||
            args.runtimeSelfTest || args.astSelfTest) {
            showErrorAndExit("compile cannot be combined with another command");
        }
        if (!args.functionName.empty() || !args.runArgs.empty() ||
            !args.paramAssignments.empty() || !args.selfAssignments.empty() ||
            !args.bvmAssignments.empty() || !args.txFile.empty() ||
            !args.stackTraceOutputFile.empty()) {
            showErrorAndExit("compile accepts only --asa, -d, and --debug-output");
        }
    }

    if (mode == CommandLineMode::RunBytecode) {
        if (args.debuggerMode || args.liveDebugServerMode ||
            args.runAST || args.replMode ||
            args.compilerReplMode || args.runtimeSelfTest ||
            args.astSelfTest) {
            showErrorAndExit("run cannot be combined with another command");
        }
        if (!args.paramAssignments.empty() || !args.selfAssignments.empty() ||
            !args.bvmAssignments.empty() || debugInfoOptionSeen) {
            showErrorAndExit(
                "run accepts only --asa, --txfile, --stack-trace-output, "
                "and function arguments"
            );
        }
    }

    if (mode == CommandLineMode::RunAST) {
        if (args.debuggerMode || args.liveDebugServerMode ||
            args.runBytecode || args.replMode ||
            args.compilerReplMode || args.runtimeSelfTest ||
            args.astSelfTest) {
            showErrorAndExit("ast cannot be combined with another command");
        }
        if (debugInfoOptionSeen || !args.stackTraceOutputFile.empty()) {
            showErrorAndExit(
                "ast does not accept --debug-output or --stack-trace-output"
            );
        }
    }

    if (mode == CommandLineMode::Debug) {
        if (args.liveDebugServerMode || args.runBytecode ||
            args.runAST || args.replMode ||
            args.compilerReplMode || args.runtimeSelfTest ||
            args.astSelfTest) {
            showErrorAndExit("debug cannot be combined with another command");
        }
        if (!args.functionName.empty() || !args.runArgs.empty() ||
            !args.paramAssignments.empty() || !args.selfAssignments.empty() ||
            !args.bvmAssignments.empty() || !args.txFile.empty() ||
            !args.stackTraceOutputFile.empty() || debugInfoOptionSeen) {
            showErrorAndExit("debug accepts only --asa");
        }
    }

    if (mode == CommandLineMode::DebugServer) {
        if (args.debuggerMode || args.runBytecode || args.runAST ||
            args.replMode || args.compilerReplMode ||
            args.runtimeSelfTest || args.astSelfTest) {
            showErrorAndExit("debug-server cannot be combined with another command");
        }
        if (!args.paramAssignments.empty() || !args.selfAssignments.empty() ||
            !args.bvmAssignments.empty() || debugInfoOptionSeen ||
            !args.stackTraceOutputFile.empty()) {
            showErrorAndExit(
                "debug-server accepts only --asa, --txfile, and function arguments"
            );
        }
    }

    if (mode == CommandLineMode::Shell) {
        if (args.debuggerMode || args.liveDebugServerMode ||
            args.runBytecode || args.runAST ||
            args.compilerReplMode || args.runtimeSelfTest ||
            args.astSelfTest) {
            showErrorAndExit("shell cannot be combined with another command");
        }
        if (!args.functionName.empty() || !args.runArgs.empty() ||
            !args.paramAssignments.empty() || !args.selfAssignments.empty() ||
            !args.bvmAssignments.empty() || !args.txFile.empty() ||
            !args.stackTraceOutputFile.empty() || debugInfoOptionSeen) {
            showErrorAndExit("shell accepts only an optional .ct file");
        }
    }

    if (mode == CommandLineMode::CompilerRepl) {
        if (args.debuggerMode || args.liveDebugServerMode ||
            args.runBytecode || args.runAST || args.replMode ||
            args.runtimeSelfTest || args.astSelfTest) {
            showErrorAndExit(
                "compiler-repl cannot be combined with another command"
            );
        }
        if (!args.filename.empty() || !args.functionName.empty() ||
            !args.runArgs.empty() || !args.paramAssignments.empty() ||
            !args.selfAssignments.empty() || !args.bvmAssignments.empty() ||
            !args.txFile.empty() || !args.debugOutputFile.empty() ||
            !args.stackTraceOutputFile.empty() || debugInfoOptionSeen) {
            showErrorAndExit("compiler-repl accepts only global options");
        }
    }

    if (mode == CommandLineMode::Test) {
        if (args.debuggerMode || args.liveDebugServerMode ||
            args.runBytecode || args.runAST ||
            args.replMode || args.compilerReplMode ||
            !args.filename.empty() || !args.functionName.empty() ||
            !args.runArgs.empty() || !args.paramAssignments.empty() ||
            !args.selfAssignments.empty() || !args.bvmAssignments.empty() ||
            !args.txFile.empty() || !args.debugOutputFile.empty() ||
            !args.stackTraceOutputFile.empty() || debugInfoOptionSeen) {
            showErrorAndExit("test accepts only the target: runtime or ast");
        }
    }

    if (args.runtimeSelfTest || args.astSelfTest) {
        return args;
    }

    if (args.compilerReplMode) {
        return args;
    }

    if (args.replMode) {
        if (!args.filename.empty() &&
            !hasCtExtension(args.filename)) {
            std::cerr << "Error: REPL startup file must be in .ct format, current file: '"
                      << args.filename << "'" << std::endl;
            Help::showUsage(argv[0]);
            exit(1);
        }
        return args;
    }

    if (args.filename.empty()) {
        std::cerr << "Error: No input file specified" << std::endl;
        Help::showUsage(argv[0]);
        exit(1);
    }

    if (!hasCtExtension(args.filename)) {
        std::cerr << "Error: File must be in .ct format, current file: '"
                  << args.filename << "'" << std::endl;
        Help::showUsage(argv[0]);
        exit(1);
    }

    return args;
}

#ifdef ENABLE_DEBUGGER
static apc_debug::StackElement
parseOneInput(const std::string& input, const std::string& fieldType)
{
    const auto result =
        apc_debug::parseStackArgumentValueDetailed(input, fieldType);

    switch (result.status) {
        case apc_debug::StackArgumentStatus::Parsed:
            std::cout << "  已设置: " << result.value.toHexString(true);
            if (result.convertedAddress) {
                std::cout << " (address -> pubkeyhash)";
            }
            std::cout << std::endl;
            break;
        case apc_debug::StackArgumentStatus::DefaultEmpty:
            std::cout << "  使用默认值: " << result.value.toHexString(true)
                      << std::endl;
            break;
        case apc_debug::StackArgumentStatus::InvalidAddress:
            std::cerr << "警告: 地址格式无效，使用默认值" << std::endl;
            std::cout << "  使用默认值: " << result.value.toHexString(true)
                      << std::endl;
            break;
        case apc_debug::StackArgumentStatus::InvalidHex:
            std::cerr << "警告: 无效的十六进制格式，使用默认值" << std::endl;
            std::cout << "  使用默认值: " << result.value.toHexString(true)
                      << std::endl;
            break;
        case apc_debug::StackArgumentStatus::InvalidNumber:
            std::cerr << "警告: 无法解析输入，使用默认值" << std::endl;
            std::cout << "  使用默认值: " << result.value.toHexString(true)
                      << std::endl;
            break;
    }
    return result.value;
}

// structsJson 用于结构体按字段提示, 可空.
bool promptForFunctionParameters(
    std::shared_ptr<apc_debug::DebugInfo> debugInfo,
    apc_debug::StackState& mainStack,
    const nlohmann::json* structsJson,
    size_t* outSelectedStartPC,
    size_t* outSelectedEndPC,
    bool useEnglishUI
)
{
    if (!debugInfo || debugInfo->functions.empty()) {
        if (useEnglishUI) {
            std::cout << "Warning: no function info found (debug_info empty)"
                      << std::endl;
        } else {
            std::cout << "警告: 未找到函数信息（debug_info 为空或无函数）"
                      << std::endl;
        }
        return false;
    }

    if (outSelectedStartPC) {
        *outSelectedStartPC = 0;
    }
    if (outSelectedEndPC) {
        *outSelectedEndPC = 0;
    }

    nlohmann::json functionsArray = nlohmann::json::array();
    for (const auto& [name, func] : debugInfo->functions) {
        nlohmann::json funcObj;
        funcObj["name"] = func.name;
        funcObj["type"] = func.isPublic ? "public" : "private";
        nlohmann::json paramsArray = nlohmann::json::array();
        for (const auto& p : func.parameters) {
            nlohmann::json paramObj;
            paramObj["name"] = p.name;
            paramObj["type"] = p.type;
            paramsArray.push_back(paramObj);
        }
        funcObj["params"] = paramsArray;
        functionsArray.push_back(funcObj);
    }
    const auto& functions = functionsArray;
    if (functions.empty()) {
        if (useEnglishUI) {
            std::cout << "Warning: no functions available for debugging"
                      << std::endl;
        } else {
            std::cout << "警告: 没有可调试的函数" << std::endl;
        }
        return false;
    }

    const nlohmann::json* structsArray = (structsJson && structsJson->is_array()
                                         )
                                             ? structsJson
                                             : nullptr;

    std::cout << "\n=====================================" << std::endl;
    if (useEnglishUI) {
        std::cout << "Functions available for debugging:" << std::endl;
    } else {
        std::cout << "可调试的函数列表:" << std::endl;
    }
    std::cout << "=====================================" << std::endl;

    for (size_t i = 0; i < functions.size(); ++i) {
        const auto& func = functions[i];
        std::string funcName = func.value("name", "未知函数");
        std::string funcType = func.value("type", "unknown");
        int paramCount = func.contains("params") ? func["params"].size() : 0;

        if (useEnglishUI) {
            std::cout << "[" << (i + 1) << "] " << funcName << " (" << funcType
                      << ", " << paramCount << " param(s))" << std::endl;
        } else {
            std::cout << "[" << (i + 1) << "] " << funcName << " (" << funcType
                      << ", " << paramCount << " 个参数)" << std::endl;
        }
    }

    if (useEnglishUI) {
        std::cout << "\nPlease select function to debug (enter index 1-"
                  << functions.size()
                  << ", or press Enter to skip): ";
    } else {
        std::cout << "\n请选择要调试的函数 (输入编号 1-" << functions.size()
                  << ", 或直接回车跳过): ";
    }
    std::string choice;
    std::getline(std::cin, choice);
    choice = trimAsciiWhitespace(choice);

    size_t selectedIndex = 0;
    while (true) {
        // 空输入: 为所有公有函数依次输入参数
        if (choice.empty()) {
            if (useEnglishUI) {
                std::cout << "\nNo specific function selected, will debug the "
                             "entire file."
                          << std::endl;
                std::cout
                    << "You need to provide initial parameters for all public "
                       "functions."
                    << std::endl;
            } else {
                std::cout << "\n未选择特定函数，将调试整个代码文件。" << std::endl;
                std::cout << "需要为所有公有函数提供参数初始值。" << std::endl;
            }

            std::vector<size_t> publicFunctionIndices;
            for (size_t i = 0; i < functions.size(); ++i) {
                const auto& func = functions[i];
                std::string funcType = func.value("type", "unknown");
                if (funcType == "public") {
                    publicFunctionIndices.push_back(i);
                }
            }

            if (publicFunctionIndices.empty()) {
                if (useEnglishUI) {
                    std::cout
                        << "No public functions found, skipping parameter "
                           "input."
                        << std::endl;
                } else {
                    std::cout << "没有找到公有函数，跳过参数输入。" << std::endl;
                }
                return false;
            }

            std::cout << "\n=====================================" << std::endl;
            if (useEnglishUI) {
                std::cout << "Found " << publicFunctionIndices.size()
                          << " public function(s)" << std::endl;
            } else {
                std::cout << "找到 " << publicFunctionIndices.size()
                          << " 个公有函数" << std::endl;
            }
            std::cout << "=====================================" << std::endl;

            std::vector<apc_debug::StackElement> allParamElements;

            for (size_t funcIdx : publicFunctionIndices) {
                const auto& func = functions[funcIdx];
                std::string funcName = func.value("name", "未知函数");

                if (useEnglishUI) {
                    std::cout << "\n----- Function: " << funcName << " -----"
                              << std::endl;
                } else {
                    std::cout << "\n----- 函数: " << funcName << " -----"
                              << std::endl;
                }

                if (!func.contains("params") || !func["params"].is_array() ||
                    func["params"].empty()) {
                    if (useEnglishUI) {
                        std::cout
                            << "This function has no parameters, skipping."
                            << std::endl;
                    } else {
                        std::cout << "该函数没有参数，跳过。" << std::endl;
                    }
                    continue;
                }

                const auto& params = func["params"];
                if (useEnglishUI) {
                    std::cout << "You need to provide initial values for "
                              << params.size() << " parameter(s):" << std::endl;
                } else {
                    std::cout << "需要为 " << params.size()
                              << " 个参数提供初始值:" << std::endl;
                }

                for (size_t i = 0; i < params.size(); ++i) {
                    const auto& param = params[i];
                    std::string paramName =
                        param.value("name", "参数" + std::to_string(i));
                    std::string paramType = param.value("type", "unknown");

                    bool isStructParam =
                        structsArray &&
                        apc_interpreter::parameter_schema::findStructByName(
                            *structsArray, paramType
                        ) != nullptr;

                    if (isStructParam) {
                        std::vector<std::pair<std::string, std::string>> fields;
                        apc_interpreter::parameter_schema::expandStructFields(
                            *structsArray, paramType, "", fields
                        );
                        if (useEnglishUI) {
                            std::cout << "\nParam " << (i + 1) << "/"
                                      << params.size() << ": " << paramName
                                      << " (struct: " << paramType << ", "
                                      << fields.size() << " field(s))"
                                      << std::endl;
                            std::cout << "Supported input formats:"
                                      << std::endl;
                            std::cout
                                << "  - Integer: 42, -100, 0x1a (hex)"
                                << std::endl;
                            std::cout
                                << "  - Hex bytes: 0x1234abcd" << std::endl;
                            std::cout
                                << "  - String: \"hello\" (quoted)"
                                << std::endl;
                            std::cout
                                << "  - Default: (press Enter to use default)"
                                << std::endl;
                        } else {
                            std::cout << "\n参数 " << (i + 1) << "/"
                                      << params.size() << ": " << paramName
                                      << " (结构体: " << paramType << ", "
                                      << fields.size() << " 个字段)"
                                      << std::endl;
                            std::cout << "支持的输入格式:" << std::endl;
                            std::cout << "  - 整数: 42, -100, 0x1a (十六进制)"
                                      << std::endl;
                            std::cout << "  - 十六进制字节: 0x1234abcd"
                                      << std::endl;
                            std::cout << "  - 字符串: \"hello\" (用引号包围)"
                                      << std::endl;
                            std::cout << "  - 默认值: (直接回车使用类型默认值)"
                                      << std::endl;
                        }

                        for (size_t fi = 0; fi < fields.size(); ++fi) {
                            const std::string& fieldPath = fields[fi].first;
                            const std::string& fieldType = fields[fi].second;
                            if (useEnglishUI) {
                                std::cout
                                    << "\n  Field " << (fi + 1) << "/"
                                    << fields.size() << ": " << fieldPath
                                    << " (type: " << fieldType << ")"
                                    << std::endl;
                                std::cout << "  Enter value: ";
                            } else {
                                std::cout << "\n  字段 " << (fi + 1) << "/"
                                          << fields.size() << ": "
                                          << fieldPath << " (类型: " << fieldType
                                          << ")" << std::endl;
                                std::cout << "  请输入值: ";
                            }

                            std::string input;
                            std::getline(std::cin, input);
                            input = trimAsciiWhitespace(input);
                            allParamElements.push_back(
                                parseOneInput(input, fieldType)
                            );
                        }
                    } else {
                        if (useEnglishUI) {
                            std::cout << "\nParam " << (i + 1) << "/"
                                      << params.size() << ": " << paramName
                                      << " (type: " << paramType << ")"
                                      << std::endl;
                            std::cout << "Supported input formats:"
                                      << std::endl;
                            std::cout
                                << "  - Integer: 42, -100, 0x1a (hex)"
                                << std::endl;
                            std::cout
                                << "  - Hex bytes: 0x1234abcd" << std::endl;
                            std::cout
                                << "  - String: \"hello\" (quoted)"
                                << std::endl;
                            std::cout
                                << "  - Default: (press Enter to use default)"
                                << std::endl;
                            std::cout << "Enter value: ";
                        } else {
                            std::cout << "\n参数 " << (i + 1) << "/"
                                      << params.size() << ": " << paramName
                                      << " (类型: " << paramType << ")"
                                      << std::endl;
                            std::cout << "支持的输入格式:" << std::endl;
                            std::cout
                                << "  - 整数: 42, -100, 0x1a (十六进制)"
                                << std::endl;
                            std::cout
                                << "  - 十六进制字节: 0x1234abcd" << std::endl;
                            std::cout
                                << "  - 字符串: \"hello\" (用引号包围)"
                                << std::endl;
                            std::cout
                                << "  - 默认值: (直接回车使用类型默认值)"
                                << std::endl;
                            std::cout << "请输入值: ";
                        }

                        std::string input;
                        std::getline(std::cin, input);
                        input = trimAsciiWhitespace(input);
                        allParamElements.push_back(
                            parseOneInput(input, paramType)
                        );
                    }
                }
            }

            if (!allParamElements.empty()) {
                for (const auto& elem : allParamElements) {
                    mainStack.push(elem);
                }

                std::cout << "\n====================================="
                          << std::endl;
                if (useEnglishUI) {
                    std::cout << allParamElements.size()
                              << " parameter(s) pushed to main stack"
                              << std::endl;
                    std::cout << "Will debug the entire bytecode by m_pc order"
                              << std::endl;
                } else {
                    std::cout << "已将 " << allParamElements.size()
                              << " 个参数压入主栈" << std::endl;
                    std::cout << "将按 m_pc 顺序调试整个代码文件" << std::endl;
                }
                std::cout << "====================================="
                          << std::endl;

                return true;
            }

            if (useEnglishUI) {
                std::cout
                    << "\nAll public functions have no parameters, skipping "
                       "parameter input."
                    << std::endl;
            } else {
                std::cout << "\n所有公有函数都没有参数，跳过参数输入。"
                          << std::endl;
            }
            return false;
        }

        try {
            selectedIndex = std::stoull(choice);
            if (selectedIndex < 1 || selectedIndex > functions.size()) {
                if (useEnglishUI) {
                    std::cerr << "Error: invalid selection, please enter a "
                                 "number between 1 and "
                              << functions.size()
                              << ", or press Enter to skip" << std::endl;
                    std::cout
                        << "Please re-select function to debug (index 1-"
                        << functions.size() << ", or press Enter): ";
                } else {
                    std::cerr << "错误: 无效的选择，请输入 1-" << functions.size()
                              << " 之间的编号，或直接回车跳过" << std::endl;
                    std::cout << "请重新选择要调试的函数 (输入编号 1-"
                              << functions.size() << ", 或直接回车): ";
                }
                std::getline(std::cin, choice);
                choice = trimAsciiWhitespace(choice);
                continue;
            }
            break;
        } catch (...) {
            if (useEnglishUI) {
                std::cerr
                    << "Error: invalid input, please enter a number or press "
                       "Enter"
                    << std::endl;
                std::cout << "Please re-select function to debug (index 1-"
                          << functions.size() << ", or press Enter): ";
            } else {
                std::cerr << "错误: 无效的输入格式，请输入数字或直接回车"
                          << std::endl;
                std::cout << "请重新选择要调试的函数 (输入编号 1-"
                          << functions.size() << ", 或直接回车): ";
            }
            std::getline(std::cin, choice);
            choice = trimAsciiWhitespace(choice);
        }
    }

    const auto& selectedFunc = functions[selectedIndex - 1];
    std::string funcName = selectedFunc.value("name", "未知函数");
    std::string funcType = selectedFunc.value("type", "unknown");

    // 记录所选函数的字节码范围, 用于限定 VM 执行区间.
    if (debugInfo) {
        auto it = debugInfo->functions.find(funcName);
        if (it != debugInfo->functions.end()) {
            const auto& finfo = it->second;
            if (outSelectedStartPC) {
                *outSelectedStartPC = finfo.startPC;
            }
            if (outSelectedEndPC) {
                *outSelectedEndPC = finfo.endPC;
            }
        }
    }

    if (!selectedFunc.contains("params") ||
        !selectedFunc["params"].is_array()) {
        if (useEnglishUI) {
            std::cout << "\nFunction " << funcName
                      << " has no parameters, no initial values needed."
                      << std::endl;
        } else {
            std::cout << "\n函数 " << funcName << " 没有参数，无需输入初始值。"
                      << std::endl;
        }
        return false;
    }

    const auto& params = selectedFunc["params"];
    if (params.empty()) {
        if (useEnglishUI) {
            std::cout << "\nFunction " << funcName
                      << " has no parameters, no initial values needed."
                      << std::endl;
        } else {
            std::cout << "\n函数 " << funcName << " 没有参数，无需输入初始值。"
                      << std::endl;
        }
        return false;
    }

    std::cout << "\n=====================================" << std::endl;
    if (useEnglishUI) {
        std::cout << "Debug function: " << funcName << " (" << funcType << ")"
                  << std::endl;
        std::cout
            << "You need to provide initial values for the following params:"
            << std::endl;
    } else {
        std::cout << "调试函数: " << funcName << " (" << funcType << ")"
                  << std::endl;
        std::cout << "需要为以下参数提供初始值:" << std::endl;
    }
    std::cout << "=====================================" << std::endl;

    std::vector<apc_debug::StackElement> paramElements;
    for (size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        std::string paramName = param.value("name", "参数" + std::to_string(i));
        std::string paramType = param.value("type", "unknown");

        bool isStructParam = structsArray &&
                             apc_interpreter::parameter_schema::findStructByName(
                                 *structsArray, paramType
                             ) !=
                                 nullptr;

        if (isStructParam) {
            std::vector<std::pair<std::string, std::string>> fields;
            apc_interpreter::parameter_schema::expandStructFields(
                *structsArray, paramType, "", fields
            );
            if (useEnglishUI) {
                std::cout << "\nParam " << (i + 1) << "/" << params.size()
                          << ": " << paramName << " (struct: " << paramType
                          << ", " << fields.size() << " field(s))" << std::endl;
                std::cout << "Supported input formats:" << std::endl;
                std::cout << "  - Integer: 42, -100, 0x1a (hex)" << std::endl;
                std::cout << "  - Hex bytes: 0x1234abcd" << std::endl;
                std::cout << "  - String: \"hello\" (quoted)" << std::endl;
                std::cout
                    << "  - Default: (press Enter to use default)"
                    << std::endl;
            } else {
                std::cout << "\n参数 " << (i + 1) << "/" << params.size() << ": "
                          << paramName << " (结构体: " << paramType << ", "
                          << fields.size() << " 个字段)" << std::endl;
                std::cout << "支持的输入格式:" << std::endl;
                std::cout << "  - 整数: 42, -100, 0x1a (十六进制)" << std::endl;
                std::cout << "  - 十六进制字节: 0x1234abcd" << std::endl;
                std::cout << "  - 字符串: \"hello\" (用引号包围)" << std::endl;
                std::cout << "  - 默认值: (直接回车使用类型默认值)" << std::endl;
            }

            for (size_t fi = 0; fi < fields.size(); ++fi) {
                const std::string& fieldPath = fields[fi].first;
                const std::string& fieldType = fields[fi].second;
                if (useEnglishUI) {
                    std::cout << "\n  Field " << (fi + 1) << "/"
                              << fields.size() << ": " << fieldPath
                              << " (type: " << fieldType << ")" << std::endl;
                    std::cout << "  Enter value: ";
                } else {
                    std::cout << "\n  字段 " << (fi + 1) << "/" << fields.size()
                              << ": " << fieldPath << " (类型: " << fieldType
                              << ")" << std::endl;
                    std::cout << "  请输入值: ";
                }

                std::string input;
                std::getline(std::cin, input);
                input = trimAsciiWhitespace(input);
                paramElements.push_back(parseOneInput(input, fieldType));
            }
        } else {
            if (useEnglishUI) {
                std::cout << "\nParam " << (i + 1) << "/" << params.size()
                          << ": " << paramName << " (type: " << paramType << ")"
                          << std::endl;
                std::cout << "Supported input formats:" << std::endl;
                std::cout << "  - Integer: 42, -100, 0x1a (hex)" << std::endl;
                std::cout << "  - Hex bytes: 0x1234abcd" << std::endl;
                std::cout << "  - String: \"hello\" (quoted)" << std::endl;
                std::cout
                    << "  - Default: (press Enter to use default)"
                    << std::endl;
                std::cout << "Enter value: ";
            } else {
                std::cout << "\n参数 " << (i + 1) << "/" << params.size() << ": "
                          << paramName << " (类型: " << paramType << ")"
                          << std::endl;
                std::cout << "支持的输入格式:" << std::endl;
                std::cout << "  - 整数: 42, -100, 0x1a (十六进制)" << std::endl;
                std::cout << "  - 十六进制字节: 0x1234abcd" << std::endl;
                std::cout << "  - 字符串: \"hello\" (用引号包围)" << std::endl;
                std::cout << "  - 默认值: (直接回车使用类型默认值)" << std::endl;
                std::cout << "请输入值: ";
            }

            std::string input;
            std::getline(std::cin, input);
            input = trimAsciiWhitespace(input);
            paramElements.push_back(parseOneInput(input, paramType));
        }
    }

    // 第一个参数在栈底; 结构体按字段顺序压栈.
    for (const auto& elem : paramElements) {
        mainStack.push(elem);
    }

    std::cout << "\n=====================================" << std::endl;
    if (useEnglishUI) {
        std::cout << paramElements.size()
                  << " parameter(s) pushed to main stack" << std::endl;
    } else {
        std::cout << "已将 " << paramElements.size() << " 个参数压入主栈"
                  << std::endl;
    }
    std::cout << "=====================================" << std::endl;

    return true;
}

int startDebugger(
    const std::string& sourceFile,
    const std::string& sourceCode,
    bool allowSubscopeAltstack
)
{
    bool useEnglishUI = false;
    {
        std::cout << "选择 CLI 语言 / Select CLI language [zh/en] (默认 default: zh): ";
        std::string langCode;
        std::getline(std::cin, langCode);
        langCode = trimAsciiWhitespace(langCode);
        std::transform(
            langCode.begin(), langCode.end(), langCode.begin(), ::tolower
        );
        if (langCode == "en" || langCode == "english") {
            useEnglishUI = true;
        } else if (!langCode.empty() &&
                   !(langCode == "zh" || langCode == "cn" ||
                     langCode == "zh-cn" || langCode == "chinese")) {
            std::cout
                << "不支持的语言代码 / Unsupported language code, 使用中文 / use zh."
                << std::endl;
        }
    }

    auto compileResult = apc_debug::DebuggerCore::compileSource(
        sourceFile, sourceCode, allowSubscopeAltstack
    );

    if (!compileResult.success) {
        std::cerr << "编译失败: " << compileResult.errorMessage << std::endl;
        return 1;
    }

    auto vm = std::make_shared<apc_debug::BVMSimulator>(
        compileResult.bytecodeInstructions, compileResult.debugInfo
    );
    auto bpMgr = std::make_shared<apc_debug::BreakpointManager>(
        compileResult.debugInfo
    );
    vm->setBreakpointManager(bpMgr);

    if (compileResult.debugInfo) {
        apc_debug::StackState mainStack;
        apc_debug::StackState altStack;
        size_t selectedStartPC = 0;
        size_t selectedEndPC = 0;

        const nlohmann::json* structsPtr = nullptr;
        if (compileResult.jsonData.contains("structs") &&
            compileResult.jsonData["structs"].is_array()) {
            structsPtr = &compileResult.jsonData["structs"];
        }
        bool hasParams = promptForFunctionParameters(
            compileResult.debugInfo,
            mainStack,
            structsPtr,
            &selectedStartPC,
            &selectedEndPC,
            useEnglishUI
        );

        // 选了具体函数就限定执行区间; endPC 缺省或越界都退化到字节码末尾.
        const size_t bytecodeSize = compileResult.bytecodeInstructions.size();
        if (selectedEndPC == 0 || selectedEndPC > bytecodeSize) {
            selectedEndPC = bytecodeSize;
        }
        if (selectedStartPC < selectedEndPC) {
            try {
                vm->setExecutionRange(selectedStartPC, selectedEndPC);
            } catch (const std::exception& e) {
                std::cerr << "警告: 设置单函数调试范围失败: " << e.what()
                          << "\n将退化为调试整个字节码。" << std::endl;
            }
        }

        if (hasParams) {
            vm->setInitialStacks(mainStack, altStack);
        }
    }

    apc_debug::CLIDebugger cli(vm, bpMgr, compileResult.debugInfo);
    if (useEnglishUI) {
        cli.setLanguage(apc_debug::CLIDebugger::Language::English);
    }

    if (!sourceFile.empty() && std::filesystem::exists(sourceFile)) {
        cli.loadSourceFile(sourceFile);
    }

    try {
        cli.run();
    } catch (const std::exception& e) {
        std::cerr << "调试器错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

static void printStackValues(
    const std::string& title,
    const std::vector<apc_interpreter::StackValueView>& values
)
{
    std::cout << title << " (" << values.size() << ", bottom -> top)"
              << std::endl;
    if (values.empty()) {
        std::cout << "  <empty>" << std::endl;
        return;
    }

    for (size_t i = 0; i < values.size(); ++i) {
        std::cout << "  [" << i << "] " << values[i].hex;
        if (!values[i].intValue.empty()) {
            std::cout << " (int=" << values[i].intValue << ")";
        }
        std::cout << std::endl;
    }
}

static void printBytecodeRunResult(
    const apc_interpreter::BytecodeRunResult& result
)
{
    std::cout << "Bytecode run result" << std::endl;
    std::cout << "  status: " << result.status << std::endl;
    std::cout << "  compile: "
              << (result.compileSuccess ? "success" : "failed") << std::endl;

    if (!result.functionName.empty()) {
        std::cout << "  function: " << result.functionName << " [pc "
                  << result.startPC << ", " << result.endPC << ")"
                  << std::endl;
    }

    std::cout << "  pc: " << result.pc << " / "
              << result.totalInstructions << std::endl;
    std::cout << "  executed: " << result.executedInstructions
              << " instruction(s)" << std::endl;
    std::cout << "  max_stack: " << result.maxStackSize
              << ", max_call_depth: " << result.maxCallDepth << std::endl;

    if (!result.warnings.empty()) {
        std::cout << "  warnings:" << std::endl;
        for (const auto& warning : result.warnings) {
            std::cout << "    - " << warning << std::endl;
        }
    }

    printStackValues("Main stack", result.mainStack);
    printStackValues("Alt stack", result.altStack);

    if (result.stackTraceWritten) {
        std::cout << "Stack trace: " << result.stackTraceOutputFile
                  << std::endl;
    }

    std::cout << "Error: "
              << (result.errorMessage.empty() ? "<none>"
                                              : result.errorMessage)
              << std::endl;
}
#endif

static std::string trimRuntimeText(std::string value)
{
    return trimAsciiWhitespace(std::move(value));
}

struct SourceFileReadResult
{
    bool success = false;
    int exitCode = -1;
    std::string content;
};

static SourceFileReadResult readSourceFile(
    const std::string& filename,
    bool showFileLog
)
{
    SourceFileReadResult result;

    try {
        if (!std::filesystem::exists(filename)) {
            if (showFileLog) {
                LOG_ERROR("File does not exist: " + filename);
                LOG_ERROR(
                    "Current working directory: " +
                    std::filesystem::current_path().string()
                );
            }
            return result;
        }

        if (!std::filesystem::is_regular_file(filename)) {
            if (showFileLog) {
                LOG_ERROR("Not a valid file: " + filename);
            }
            return result;
        }

        auto fileSize = std::filesystem::file_size(filename);
        if (fileSize == 0) {
            if (showFileLog) {
                LOG_WARNING("File is empty: " + filename);
            }
            result.exitCode = 0;
            return result;
        }

        if (showFileLog) {
            LOG_INFO("File size: " + std::to_string(fileSize) + " bytes");
        }
    } catch (const std::filesystem::filesystem_error& ex) {
        if (showFileLog) {
            LOG_ERROR("Filesystem error: " + std::string(ex.what()));
        }
        return result;
    }

    std::ifstream fin(filename, std::ios::in | std::ios::binary);
    if (!fin.is_open()) {
        if (showFileLog) {
            LOG_ERROR("Failed to open file: " + filename);
            LOG_ERROR("Possible reasons: insufficient permissions or file in use");
        }
        return result;
    }

    fin.seekg(0, std::ios::end);
    auto fileSize = fin.tellg();
    if (fileSize <= 0) {
        if (showFileLog) {
            LOG_ERROR("Unable to get file size: " + filename);
        }
        return result;
    }

    result.content.resize(static_cast<size_t>(fileSize));
    fin.seekg(0, std::ios::beg);
    fin.read(&result.content[0], fileSize);

    if (!fin) {
        if (showFileLog) {
            LOG_ERROR("Failed to read file content: " + filename);
        }
        result.content.clear();
        return result;
    }

    if (showFileLog) {
        LOG_INFO(
            "Successfully read file: " +
            std::to_string(result.content.size()) + " bytes"
        );
    }

    result.success = true;
    result.exitCode = 0;
    return result;
}

static std::vector<std::string> splitASTFunctionList(
    const std::string& functionName
)
{
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= functionName.size()) {
        const size_t comma = functionName.find(',', start);
        const size_t end =
            comma == std::string::npos ? functionName.size() : comma;
        std::string name =
            trimRuntimeText(functionName.substr(start, end - start));
        if (!name.empty()) {
            names.push_back(std::move(name));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return names;
}

static void printRuntimeValue(
    const apc_interpreter::RuntimeValue& value,
    size_t index,
    const std::string& indent = "  "
)
{
    std::cout << indent << "[" << index << "] " << value.toDisplayString();
    try {
        std::cout << " hex=" << value.toHexString(true);
    } catch (...) {
    }
    try {
        std::cout << " int=" << value.toScriptNum();
    } catch (...) {
    }
    std::cout << std::endl;
}

static void printASTInterpretResult(
    const apc_interpreter::ASTInterpretResult& result
)
{
    std::cout << "AST interpretation result" << std::endl;
    std::cout << "  status: " << (result.success ? "finished" : "error")
              << std::endl;
    if (!result.functionName.empty()) {
        std::cout << "  function: " << result.functionName << std::endl;
    }
    std::cout << "Return values (" << result.returnValues.size() << ")"
              << std::endl;
    if (result.returnValues.empty()) {
        std::cout << "  <empty>" << std::endl;
    } else {
        for (size_t i = 0; i < result.returnValues.size(); ++i) {
            printRuntimeValue(result.returnValues[i], i);
        }
    }
    if (result.returnValuesByFunction.size() > 1) {
        std::cout << "Per-function return values" << std::endl;
        for (size_t functionIndex = 0;
             functionIndex < result.returnValuesByFunction.size();
             ++functionIndex) {
            const std::string name =
                functionIndex < result.functionNames.size()
                    ? result.functionNames[functionIndex]
                    : ("#" + std::to_string(functionIndex));
            const auto& values =
                result.returnValuesByFunction[functionIndex];
            std::cout << "  " << name << " (" << values.size() << ")"
                      << std::endl;
            if (values.empty()) {
                std::cout << "    <empty>" << std::endl;
            } else {
                for (size_t valueIndex = 0; valueIndex < values.size();
                     ++valueIndex) {
                    printRuntimeValue(values[valueIndex], valueIndex, "    ");
                }
            }
        }
    }
    std::cout << "Error: "
              << (result.errorMessage.empty() ? "<none>"
                                              : result.errorMessage)
              << std::endl;
}

int main(int argc, char* argv[])
{
    CommandLineArgs args = parseCommandLineArgs(argc, argv);

#ifdef EXECUTABLE_NAME
    std::string logFileName = std::string(EXECUTABLE_NAME) + ".log";
#else
    std::string logFileName = "Compiler.log";
#endif

#ifndef NDEBUG
    Logger::GetInstance()
        .Initialize(
            args.logLevel,
            logFileName,
            !(args.debuggerMode || args.liveDebugServerMode ||
              args.runBytecode || args.runAST ||
              args.replMode || args.compilerReplMode ||
              args.runtimeSelfTest || args.astSelfTest)
        );
#else
    Logger::GetInstance().Initialize(args.logLevel, logFileName, false);
#endif

    std::string executableName;
#ifdef EXECUTABLE_NAME
    executableName = EXECUTABLE_NAME;
#else
    executableName = "Compiler";
#endif

    const bool quietConsoleLog =
        args.debuggerMode || args.liveDebugServerMode ||
        args.runBytecode || args.runAST || args.replMode ||
        args.compilerReplMode || args.runtimeSelfTest ||
        args.astSelfTest;

    if (!quietConsoleLog) {
        LOG_INFO("start " + executableName + " \n");
    }

    ConfigManager::getInstance().initialize("user_preferences.json");

    if (!quietConsoleLog) {
        LOG_DEBUG("ConfigManager initialized with three-layer architecture");
    }

    if (args.runtimeSelfTest) {
        const bool ok =
            apc_interpreter::runRuntimeSelfTest(std::cout, std::cerr);
        Logger::GetInstance().Shutdown();
        return ok ? 0 : 1;
    }

    if (args.astSelfTest) {
        const bool ok = apc_interpreter::runASTSelfTest(std::cout, std::cerr);
        Logger::GetInstance().Shutdown();
        return ok ? 0 : 1;
    }

    if (args.compilerReplMode) {
        Logger::GetInstance().Shutdown();
        Logger::GetInstance().Initialize(args.logLevel, logFileName, false);
        apc::repl::ReplFrontend frontend;
        const int replResult = frontend.run();
        Logger::GetInstance().Shutdown();
        return replResult;
    }

    if (args.replMode) {
        Logger::GetInstance().Shutdown();
        Logger::GetInstance().Initialize(args.logLevel, logFileName, false);
#ifdef ENABLE_DEBUGGER
        apc_repl::DebugLauncher debugLauncher =
            [&](const std::string& debugFile) -> int {
            std::ifstream file(debugFile);
            if (!file.is_open()) {
                std::cerr << "Debugger error: failed to open file '"
                          << debugFile << "'" << std::endl;
                return 1;
            }
            std::string sourceCode(
                (std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>()
            );
            return startDebugger(
                debugFile,
                sourceCode,
                args.allowSubscopeAltstack
            );
        };
#else
        apc_repl::DebugLauncher debugLauncher;
#endif
        const int replResult =
            apc_repl::runReplShell(
                std::cin,
                std::cout,
                std::cerr,
                args.filename,
                debugLauncher
            );
        Logger::GetInstance().Shutdown();
        return replResult;
    }

    const bool showFileLog = !(args.debuggerMode || args.liveDebugServerMode);
    SourceFileReadResult sourceFile = readSourceFile(args.filename, showFileLog);
    if (!sourceFile.success) {
        return sourceFile.exitCode;
    }
    std::string tbc_file = std::move(sourceFile.content);

#ifdef ENABLE_DEBUGGER
    if (args.liveDebugServerMode) {
        Logger::GetInstance().Shutdown();
        Logger::GetInstance().Initialize(args.logLevel, logFileName, false);

        apc_debug::LiveDebugServerOptions liveOptions;
        liveOptions.sourceFile = args.filename;
        liveOptions.sourceCode = tbc_file;
        liveOptions.functionName = args.functionName;
        liveOptions.args = args.runArgs;
        liveOptions.txFile = args.txFile;
        liveOptions.allowSubscopeAltstack = args.allowSubscopeAltstack;

        const int result = apc_debug::runLiveDebugServer(
            std::cin,
            std::cout,
            std::cerr,
            liveOptions
        );
        Logger::GetInstance().Shutdown();
        return result;
    }

    if (args.runBytecode) {
        Logger::GetInstance().Shutdown();
        Logger::GetInstance().Initialize(args.logLevel, logFileName, false);

        apc_interpreter::BytecodeRunOptions runOptions;
        runOptions.allowSubscopeAltstack = args.allowSubscopeAltstack;
        runOptions.functionName = args.functionName;
        runOptions.args = args.runArgs;
        runOptions.txFile = args.txFile;
        runOptions.stackTraceOutputFile = args.stackTraceOutputFile;

        apc_interpreter::BytecodeRunner runner;
        auto result = runner.runSource(args.filename, tbc_file, runOptions);
        printBytecodeRunResult(result);

        Logger::GetInstance().Shutdown();
        return result.success ? 0 : 1;
    }
#else
    if (args.liveDebugServerMode) {
        std::cerr << "debug-server requires BUILD_DEBUGGER=ON" << std::endl;
        Logger::GetInstance().Shutdown();
        return 1;
    }

    if (args.runBytecode) {
        std::cerr << "run requires BUILD_DEBUGGER=ON" << std::endl;
        Logger::GetInstance().Shutdown();
        return 1;
    }
#endif

    if (args.runAST) {
        ErrorManager::getInstance().clear();
        ErrorManager::getInstance().setSourceContent(args.filename, tbc_file);
        ErrorManager::getInstance().setColorOutput(false);
        ErrorManager::getInstance().setShowContext(false);

        apc_frontend::FrontendOptions frontendOptions;
        frontendOptions.mergeLibraries = true;
        auto frontendResult = apc_frontend::compileFrontendToAst(
            args.filename, tbc_file, frontendOptions
        );

        if (!frontendResult.success || !frontendResult.ast) {
            std::cerr << "AST front-end failed";
            if (!frontendResult.errorMessage.empty()) {
                std::cerr << ": " << frontendResult.errorMessage;
            }
            std::cerr << std::endl;
            Logger::GetInstance().Shutdown();
            return 1;
        }

        std::vector<std::string> functionNames =
            splitASTFunctionList(args.functionName);
        if (functionNames.empty()) {
            functionNames.push_back(
                apc_interpreter::function_selection::chooseASTFunctionName(
                    *frontendResult.ast,
                    ""
                )
            );
        }

        apc_interpreter::ASTInterpretOptions interpretOptions;
        interpretOptions.functionName = functionNames.front();
        interpretOptions.functionNames = functionNames;
        try {
            std::vector<std::string> combinedParamAssignments;
            std::vector<std::string> combinedSelfAssignments;
            std::vector<std::string> combinedBvmAssignments;

            if (!args.txFile.empty()) {
                auto txAssignments =
                    apc_interpreter::loadRuntimeAssignmentsFromTxFile(args.txFile);
                combinedParamAssignments =
                    std::move(txAssignments.paramAssignments);
                combinedSelfAssignments =
                    std::move(txAssignments.selfAssignments);
                combinedBvmAssignments =
                    std::move(txAssignments.bvmAssignments);

                for (const auto& warning : txAssignments.warnings) {
                    std::cerr << "AST txfile warning: " << warning << std::endl;
                }
            }

            combinedParamAssignments.insert(
                combinedParamAssignments.end(),
                args.paramAssignments.begin(),
                args.paramAssignments.end()
            );
            combinedSelfAssignments.insert(
                combinedSelfAssignments.end(),
                args.selfAssignments.begin(),
                args.selfAssignments.end()
            );
            combinedBvmAssignments.insert(
                combinedBvmAssignments.end(),
                args.bvmAssignments.begin(),
                args.bvmAssignments.end()
            );

            auto namedParamValues =
                apc_interpreter::parseRuntimeFieldAssignments(
                    combinedParamAssignments,
                    "--param/--txfile"
                );

            std::vector<std::vector<apc_interpreter::RuntimeValue>> callArgs;
            callArgs.reserve(functionNames.size());
            for (size_t callIndex = 0; callIndex < functionNames.size();
                 ++callIndex) {
                FunctionNode* function =
                    apc_interpreter::function_selection::findFunction(
                        *frontendResult.ast,
                        functionNames[callIndex]
                    );
                if (!function) {
                    throw std::runtime_error(
                        "function '" + functionNames[callIndex] +
                        "' is not defined"
                    );
                }
                callArgs.push_back(
                    apc_interpreter::buildRuntimeArgsForFunction(
                        function,
                        namedParamValues,
                        args.runArgs,
                        callIndex == 0,
                        *frontendResult.ast
                    )
                );
            }

            if (!callArgs.empty()) {
                interpretOptions.args = callArgs.front();
            }
            interpretOptions.callArgs = std::move(callArgs);
            interpretOptions.selfFields =
                apc_interpreter::parseRuntimeFieldAssignments(
                    combinedSelfAssignments,
                    "--self/--txfile"
                );
            interpretOptions.bvmFields =
                apc_interpreter::canonicalizeBvmFields(
                    apc_interpreter::parseRuntimeFieldAssignments(
                        combinedBvmAssignments,
                        "--bvm/--txfile"
                    )
                );
        } catch (const std::exception& e) {
            std::cerr << "AST runtime field parse failed: " << e.what()
                      << std::endl;
            Logger::GetInstance().Shutdown();
            return 1;
        }

        apc_interpreter::ASTInterpreter interpreter;
        auto result = interpreter.run(*frontendResult.ast, interpretOptions);
        printASTInterpretResult(result);

        Logger::GetInstance().Shutdown();
        return result.success ? 0 : 1;
    }

#ifdef ENABLE_DEBUGGER
    if (args.debuggerMode) {
        // 调试器模式禁用控制台输出, 文件日志仍开启.
        Logger::GetInstance().Shutdown();
        Logger::GetInstance().Initialize(args.logLevel, logFileName, false);

        ErrorManager::getInstance().setSourceContent(args.filename, tbc_file);
        ErrorManager::getInstance().setColorOutput(false);
        ErrorManager::getInstance().setShowContext(false);

        int result =
            startDebugger(args.filename, tbc_file, args.allowSubscopeAltstack);
        Logger::GetInstance().Shutdown();
        return result;
    }
#endif

    // 正常编译流程
    ErrorManager::getInstance().setSourceContent(args.filename, tbc_file);
    ErrorManager::getInstance().setColorOutput(true);
    ErrorManager::getInstance().setShowContext(true);

    PassContext pipelineData;

    try {
        apc_compiler::BytecodePipelineOptions pipelineOptions;
        pipelineOptions.allowSubscopeAltstack = args.allowSubscopeAltstack;
        pipelineOptions.enableDebug = args.enableDebug;
        pipelineOptions.debugOutputFile = args.debugOutputFile;
        pipelineData = apc_compiler::runBytecodePipeline(
            args.filename, tbc_file, pipelineOptions
        );
    } catch (const std::exception& e) {
        LOG_ERROR(
            "Compilation failed with exception: " + std::string(e.what())
        );
        ErrorManager::getInstance().printAllErrors();
        Logger::GetInstance().Shutdown();
        return 1;
    }

    if (ErrorManager::getInstance().hasErrors()) {
        ErrorManager::getInstance().printAllErrors();
        Logger::GetInstance().Shutdown();
        return 1;
    }

    LOG_INFO("Compilation completed successfully");
    if (ErrorManager::getInstance().getWarningCount() > 0) {
        std::cout << "Compilation completed with "
                  << ErrorManager::getInstance().getWarningCount()
                  << " warning(s)" << std::endl;
    }

    Logger::GetInstance().Shutdown();
    return 0;
}
