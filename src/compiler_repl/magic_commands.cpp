#include "magic_commands.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>

#include "../util/string_utils.h"

#ifdef ENABLE_DEBUGGER
#include "../debugger/breakpoint/breakpoint_manager.h"
#include "../debugger/interface/cli_debugger.h"
#include "../debugger/vm/bvm_simulator.h"
#endif

namespace apc::repl
{
namespace
{
using apc::util::trim;

std::pair<std::string, std::string> splitCommand(const std::string& line)
{
    std::string text = trim(line);
    if (!text.empty() && text[0] == '%') {
        text.erase(text.begin());
    }

    std::istringstream iss(text);
    std::string command;
    iss >> command;

    std::string args;
    std::getline(iss, args);
    return {command, trim(args)};
}

std::string readFile(const std::string& path)
{
    std::ifstream input(path);
    if (!input) {
        return "";
    }
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

#ifdef ENABLE_DEBUGGER
struct PCRange
{
    size_t start = 0;
    size_t end = 0;

    bool valid() const
    {
        return start < end;
    }
};

std::string formatTopStackValue(const apc_debug::StackState& stack)
{
    if (stack.empty()) {
        return "";
    }

    const auto& element = stack.peek(0);
    auto intValue = element.toInt();
    if (intValue.has_value()) {
        return std::to_string(*intValue);
    }
    return element.toHexString(true);
}

std::optional<PCRange> findPCRangeForLines(
    const CompilerResult& result,
    const std::string& entryFunction,
    size_t startLine,
    size_t endLine
)
{
    if (!result.debugInfo || startLine == 0 || endLine == 0 ||
        startLine > endLine || result.asmInstructions.empty()) {
        return std::nullopt;
    }

    size_t functionStart = 0;
    size_t functionEnd = result.asmInstructions.size();
    if (!entryFunction.empty()) {
        auto it = result.debugInfo->functions.find(entryFunction);
        if (it != result.debugInfo->functions.end()) {
            functionStart = it->second.startPC;
            functionEnd = it->second.endPC;
            if (functionEnd == 0 ||
                functionEnd > result.asmInstructions.size()) {
                functionEnd = result.asmInstructions.size();
            }
        }
    }

    size_t minPC = std::numeric_limits<size_t>::max();
    size_t maxPC = 0;
    for (size_t line = startLine; line <= endLine; ++line) {
        for (size_t pc : result.debugInfo->getPCsForLine(line)) {
            if (pc >= result.asmInstructions.size() || pc < functionStart ||
                pc >= functionEnd) {
                continue;
            }
            minPC = std::min(minPC, pc);
            maxPC = std::max(maxPC, pc);
        }
    }

    if (minPC == std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }

    PCRange range{minPC, std::min(maxPC + 1, functionEnd)};
    if (!range.valid()) {
        return std::nullopt;
    }
    return range;
}
#endif
} // namespace

bool MagicCommands::isMagic(const std::string& line) const
{
    std::string text = trim(line);
    return !text.empty() &&
           (text[0] == '%' || text[0] == '!' || text == "?");
}

bool MagicCommands::handle(
    const std::string& line,
    ReplSession& session,
    const CellCompiler& cellCompiler
) const
{
    std::string text = trim(line);
    if (text.empty()) {
        return true;
    }

    if (text[0] == '!') {
        cmdShell(trim(text.substr(1)));
        return true;
    }

    if (text == "?") {
        showHelp();
        return true;
    }

    auto [command, args] = splitCommand(text);
    if (command == "help" || command == "h" || command == "?") {
        showHelp();
    } else if (command == "who") {
        cmdWho(session);
    } else if (command == "bytecode" || command == "bc") {
        cmdBytecode(session);
    } else if (command == "compile") {
        cmdCompile(args, session, cellCompiler);
    } else if (command == "run") {
        cmdRun(args);
    } else if (command == "load") {
        cmdLoad(args);
    } else if (command == "debug") {
        cmdDebug(session);
    } else if (command == "reset") {
        session.reset();
        std::cout << "Session reset." << std::endl;
    } else {
        std::cout << "Unknown magic command: %" << command << std::endl;
        std::cout << "Type %help for available commands." << std::endl;
    }

    return true;
}

void MagicCommands::showHelp() const
{
    std::cout << "\nAtomicProof REPL commands:\n";
    std::cout << "  %run <file.ct>       Compile and execute a .ct file\n";
    std::cout << "  %load <file.ct>      Print file content for pasting/editing\n";
    std::cout << "  %compile [out.json]  Compile current session, optionally save JSON\n";
    std::cout << "  %bytecode            Show bytecode from the last successful compile\n";
    std::cout << "  %debug               Enter debugger for the last compile result\n";
    std::cout << "  %reset               Clear session state\n";
    std::cout << "  %who                 Show session symbols and history counts\n";
    std::cout << "  %help or ?           Show this help\n";
    std::cout << "  !<command>           Run a shell command\n\n";
}

void MagicCommands::cmdWho(const ReplSession& session) const
{
    std::cout << "imports:    " << session.imports().size() << "\n";
    std::cout << "members:    " << session.members().size() << "\n";
    std::cout << "statements: " << session.statements().size() << "\n";
    std::cout << "outputs:    " << session.outputs().size() << "\n";

#ifdef ENABLE_DEBUGGER
    if (session.hasVmState()) {
        std::cout << "main stack: " << session.mainStack().size();
        std::string mainTop = formatTopStackValue(session.mainStack());
        if (!mainTop.empty()) {
            std::cout << " (top: " << mainTop << ")";
        }
        std::cout << "\n";

        std::cout << "alt stack:  " << session.altStack().size();
        std::string altTop = formatTopStackValue(session.altStack());
        if (!altTop.empty()) {
            std::cout << " (top: " << altTop << ")";
        }
        std::cout << "\n";
    } else {
        std::cout << "vm stack:   not initialized\n";
    }
#endif

    if (!session.outputs().empty()) {
        std::cout << "\nOut history:\n";
        for (const auto& [index, value] : session.outputs()) {
            std::cout << "  Out[" << index << "] = " << value << "\n";
        }
    }
}

void MagicCommands::cmdBytecode(const ReplSession& session) const
{
    if (!session.hasLastCompile()) {
        std::cout << "No successful compile yet." << std::endl;
        return;
    }

    const auto& instructions = session.lastCompile().asmInstructions;
    if (instructions.empty()) {
        std::cout << "(bytecode is empty)" << std::endl;
        return;
    }

    for (size_t i = 0; i < instructions.size(); ++i) {
        std::cout << i << ": " << instructions[i] << "\n";
    }
}

void MagicCommands::cmdCompile(
    const std::string& args,
    const ReplSession& session,
    const CellCompiler& cellCompiler
) const
{
    auto result = cellCompiler.compileSession(session, true);
    if (!result.success) {
        std::cout << "Compilation failed: " << result.errorMessage
                  << std::endl;
        return;
    }

    std::cout << "Compilation succeeded. Bytecode bytes: "
              << (result.hexBytecode.size() / 2) << std::endl;

    if (!args.empty()) {
        std::ofstream output(args);
        if (!output) {
            std::cout << "Cannot write file: " << args << std::endl;
            return;
        }
        output << result.jsonData.dump(2) << "\n";
        std::cout << "Wrote " << args << std::endl;
    }
}

void MagicCommands::cmdRun(const std::string& args) const
{
    if (args.empty()) {
        std::cout << "Usage: %run <file.ct>" << std::endl;
        return;
    }

    std::string source = readFile(args);
    if (source.empty()) {
        std::cout << "Cannot read file: " << args << std::endl;
        return;
    }

    CompilerOptions options;
    options.enableDebug = true;
    options.colorDiagnostics = true;
    options.showDiagnosticContext = true;
    options.codeFileName = std::filesystem::path(args).stem().string();
    auto result = CompilerDriver::compileSource(args, source, options);
    if (!result.success) {
        std::cout << "Compilation failed: " << result.errorMessage
                  << std::endl;
        return;
    }

#ifndef ENABLE_DEBUGGER
    std::cout << "Execution requires BUILD_DEBUGGER=ON." << std::endl;
#else
    auto vm = std::make_shared<apc_debug::BVMSimulator>(
        result.asmInstructions, result.debugInfo
    );
    vm->run();
    if (vm->hasError()) {
        std::cout << "Runtime error: " << vm->getLastError() << std::endl;
        return;
    }

    std::string top = formatTopStackValue(vm->getMainStack());
    if (top.empty()) {
        std::cout << "Program finished." << std::endl;
    } else {
        std::cout << "Program finished. Stack top: " << top << std::endl;
    }
#endif
}

void MagicCommands::cmdLoad(const std::string& args) const
{
    if (args.empty()) {
        std::cout << "Usage: %load <file.ct>" << std::endl;
        return;
    }

    std::string source = readFile(args);
    if (source.empty()) {
        std::cout << "Cannot read file: " << args << std::endl;
        return;
    }

    std::cout << source;
    if (!source.empty() && source.back() != '\n') {
        std::cout << "\n";
    }
}

void MagicCommands::cmdDebug(const ReplSession& session) const
{
    if (!session.hasLastCompile()) {
        std::cout << "No successful compile yet." << std::endl;
        return;
    }

#ifndef ENABLE_DEBUGGER
    std::cout << "%debug requires BUILD_DEBUGGER=ON." << std::endl;
#else
    const auto& result = session.lastCompile();
    auto vm = std::make_shared<apc_debug::BVMSimulator>(
        result.asmInstructions, result.debugInfo
    );

    if (session.hasLastInitialVmState()) {
        vm->setInitialStacks(
            session.lastInitialMainStack(),
            session.lastInitialAltStack()
        );
    } else if (session.hasVmState()) {
        vm->setInitialStacks(session.mainStack(), session.altStack());
    }

    bool rangeSet = false;
    if (session.hasLastPCRange() &&
        session.lastPCEnd() <= result.asmInstructions.size() &&
        session.lastPCStart() < session.lastPCEnd()) {
        try {
            vm->setExecutionRange(session.lastPCStart(), session.lastPCEnd());
            rangeSet = true;
        } catch (...) {
        }
    }

    if (!rangeSet && session.hasLastCellRange()) {
        auto range = findPCRangeForLines(
            result, session.lastEntryFunction(), session.lastCellStartLine(),
            session.lastCellEndLine()
        );
        if (range.has_value()) {
            try {
                vm->setExecutionRange(range->start, range->end);
                rangeSet = true;
            } catch (...) {
            }
        }
    }

    if (!rangeSet && !session.lastEntryFunction().empty() &&
        result.debugInfo) {
        auto it = result.debugInfo->functions.find(session.lastEntryFunction());
        if (it != result.debugInfo->functions.end()) {
            size_t startPC = it->second.startPC;
            size_t endPC = it->second.endPC;
            if (endPC == 0 || endPC > result.asmInstructions.size()) {
                endPC = result.asmInstructions.size();
            }
            if (startPC < endPC) {
                try {
                    vm->setExecutionRange(startPC, endPC);
                } catch (...) {
                }
            }
        }
    }

    auto bpMgr = std::make_shared<apc_debug::BreakpointManager>(
        result.debugInfo
    );
    vm->setBreakpointManager(bpMgr);

    apc_debug::CLIDebugger debugger(vm, bpMgr, result.debugInfo);
    debugger.setPrompt("(apc-debug) ");
    debugger.run();
#endif
}

void MagicCommands::cmdShell(const std::string& command) const
{
    if (command.empty()) {
        std::cout << "Usage: !<shell command>" << std::endl;
        return;
    }

    int rc = std::system(command.c_str());
    if (rc != 0) {
        std::cout << "Command exited with status " << rc << std::endl;
    }
}

} // namespace apc::repl
