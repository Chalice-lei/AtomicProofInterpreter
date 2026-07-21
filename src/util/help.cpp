#include "help.h"

#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include "../config/compiler_info.h"

namespace Help
{

// 不依赖日志系统直接执行 git 命令
std::string executeGitCommandStandalone(const std::string& command)
{
    try {
        std::array<char, 128> buffer;
        std::string result;

        std::string fullCommand = command + " 2>/dev/null";

        std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(fullCommand.c_str(),
                                                         "r"),
                                                   pclose);

        if (!pipe) {
            return "";
        }

        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }

        if (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }

        return result;

    } catch (const std::exception& e) {
        return "";
    }
}

std::string getGitCommitHash()
{
    std::string hash = executeGitCommandStandalone("git rev-parse HEAD");
    if (hash.empty()) {
        hash = executeGitCommandStandalone("git rev-parse --short HEAD");
    }
    return hash.empty() ? "unknown" : hash;
}

std::string getGitBranch()
{
    std::string branch = executeGitCommandStandalone(
        "git rev-parse --abbrev-ref HEAD");
    return branch.empty() ? "unknown" : branch;
}

void showVersion()
{
    std::cout << "===  UTXO_Compiler - Version History ==="
              << std::endl;
    std::cout << std::endl;

    std::cout << "Compiler Name: " << TBC::CompilerInfo::NAME << std::endl;
    std::cout << "Version: " << TBC::CompilerInfo::VERSION << std::endl;
    std::cout << "Description: " << TBC::CompilerInfo::DESCRIPTION << std::endl;
    std::cout << std::endl;

    std::cout << "Target Architecture: "
              << TBC::CompilerInfo::DEFAULT_TARGET_ARCH << std::endl;
    std::cout << "Build Mode: " << TBC::CompilerInfo::DEFAULT_BUILD_MODE
              << std::endl;
    std::cout << std::endl;

    std::string gitHash = getGitCommitHash();
    std::string gitBranch = getGitBranch();
    if (!gitHash.empty() && gitHash != "unknown") {
        std::cout << "Git Commit: " << gitHash << std::endl;
        std::cout << "Git Branch: " << gitBranch << std::endl;
    } else {
        std::cout << "Git Information: Not available" << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Compiler Capabilities:" << std::endl;
    std::cout << "  AST Validation: "
              << (TBC::CompilerInfo::isFeatureSupported("ast_validation")
                      ? "Supported"
                      : "Not Supported")
              << std::endl;
    std::cout << "  Bytecode Optimization: "
              << (TBC::CompilerInfo::isFeatureSupported("bytecode_optimization")
                      ? "Supported"
                      : "Not Supported")
              << std::endl;
    std::cout << "  Script Verification: "
              << (TBC::CompilerInfo::isFeatureSupported("script_verification")
                      ? "Supported"
                      : "Not Supported")
              << std::endl;

    std::cout << std::endl;
    std::cout << "Compiler Implementation: C++" << __cplusplus / 100 % 100
              << std::endl;
    std::cout << std::endl;
}

void showUsage(const char* programName)
{
    std::cout << "Usage: " << programName
              << " [global-options] <command> [command-options] [arguments]"
              << std::endl;
    std::cout << std::endl;

    std::cout << "Commands:" << std::endl;
    std::cout << "  compile <file.ct>             Compile a contract" << std::endl;
#ifdef ENABLE_DEBUGGER
    std::cout << "  run <file.ct> [function] [args...]"
              << "  Run compiled bytecode" << std::endl;
#endif
    std::cout << "  ast <file.ct> [function[,function2...]] [args...]"
              << "  Interpret supported AST subset" << std::endl;
#ifdef ENABLE_DEBUGGER
    std::cout << "  debug <file.ct>               Start interactive debugger"
              << std::endl;
#endif
    std::cout << "  shell [file.ct]               Start the interactive shell"
              << std::endl;
    std::cout << "  compiler-repl                 Start the compiler REPL"
              << std::endl;
    std::cout << std::endl;

    std::cout << "Global options:" << std::endl;
    std::cout << "  -h, --help                    Show this help message"
              << std::endl;
    std::cout << "  -v, --version                 Show version information"
              << std::endl;
    std::cout
        << "  -l, --log-level <level>       Set log level (default: warning)"
        << std::endl;
    std::cout << "  --allow-subscope-altstack     Allow setAlt/setMain in "
                 "subscopes (if/else, private functions)"
              << std::endl;
    std::cout << "  --asa                         Short form of "
                 "--allow-subscope-altstack"
              << std::endl;
    std::cout << std::endl;

    std::cout << "Command options:" << std::endl;
    std::cout << "  compile: -d                   Enable debug information generation"
              << std::endl;
    std::cout << "  compile: --debug-output <file>"
              << "  Specify debug info output file"
              << std::endl;
#ifdef ENABLE_DEBUGGER
    std::cout << "  run:     --txfile <file>      Load transaction context"
              << std::endl;
    std::cout << "  run:     --stack-trace-output <file>"
              << "  Write stack trace JSON for visualization"
              << std::endl;
#endif
    std::cout << "  ast:     --txfile <file>      Load transaction context"
              << std::endl;
    std::cout << "  ast:     --param <path=value> Set function parameter field"
              << std::endl;
    std::cout << "  ast:     --self <path=value>  Set self.<field>"
              << std::endl;
    std::cout << "  ast:     --bvm <path=value>   Set BVM.<field>"
              << std::endl;
    std::cout << "                                  Dot paths and array indexes are supported"
              << std::endl;
    std::cout << std::endl;

    std::cout << "Developer diagnostics:" << std::endl;
    std::cout << "  test runtime                  Run interpreter runtime self-test"
              << std::endl;
    std::cout << "  test ast                      Run minimal AST interpreter self-test"
              << std::endl;
    std::cout << "                                  Intended for CI and implementation checks;"
              << std::endl;
    std::cout << "                                  no .ct file is required" << std::endl;
    std::cout << std::endl;

    std::cout << "Supported log levels:" << std::endl;
    std::cout << "  debug      - Debug information (most verbose)" << std::endl;
    std::cout << "  info       - General information" << std::endl;
    std::cout << "  warning    - Warning messages (default)" << std::endl;
    std::cout << "  error      - Error messages" << std::endl;
    std::cout << "  critical   - Critical errors" << std::endl;
    std::cout << "  none       - Disable logging" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout
        << "  " << programName
        << " --version                               # Show version information"
        << std::endl;
    std::cout << "  " << programName
              << " compile code.ct                         # Compile with default settings"
              << std::endl;
    std::cout << "  " << programName
              << " -l info compile code.ct                 # Compile with info log level"
              << std::endl;
    std::cout << "  " << programName
              << " --asa compile code.ct                   # Allow "
                 "setAlt/setMain in subscopes"
              << std::endl;
    std::cout
        << "  " << programName
        << " compile code.ct -d --debug-output code.debug"
        << "  # Emit debug info"
        << std::endl;
#ifdef ENABLE_DEBUGGER
    std::cout
        << "  " << programName
        << " run code.ct main 1"
        << "  # Run bytecode function"
        << std::endl;
    std::cout
        << "  " << programName
        << " run code.ct main 1 --stack-trace-output stack_trace.json"
        << "  # Export visual stack trace"
        << std::endl;
#endif
    std::cout
        << "  " << programName
        << " ast code.ct main 1"
        << "       # Interpret AST subset"
        << std::endl;
    std::cout
        << "  " << programName
        << " ast code.ct verify --self pubKeyHash=0x..."
        << "  # Interpret with self field"
        << std::endl;
    std::cout
        << "  " << programName
        << " ast code.ct main --txfile tx.txt"
        << "  # Interpret with transaction context"
        << std::endl;
#ifdef ENABLE_DEBUGGER
    std::cout
        << "  " << programName
        << " debug code.ct                           # Start debugger"
        << std::endl;
#endif
    std::cout
        << "  " << programName
        << " shell [code.ct]                         # Start shell"
        << std::endl;
    std::cout
        << "  " << programName
        << " compiler-repl                          # Start compiler REPL"
        << std::endl;
}

} // namespace Help
