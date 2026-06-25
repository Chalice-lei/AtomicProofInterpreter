#include "repl_shell.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "repl_session.h"
#include "../util/string_utils.h"

#ifdef APC_ENABLE_READLINE
extern "C" {
char* readline(const char* prompt);
void add_history(const char* line);

extern int rl_attempted_completion_over;
typedef char* rl_compentry_func_t(const char*, int);
typedef char** rl_completion_func_t(const char*, int, int);
extern rl_completion_func_t* rl_attempted_completion_function;
char** rl_completion_matches(const char* text, rl_compentry_func_t* entry_func);
}
#endif

namespace apc_repl
{
namespace
{

#ifdef APC_ENABLE_READLINE
std::vector<std::string> g_completionWords;

char* completionGenerator(const char* text, int state)
{
    static size_t index = 0;
    static std::string prefix;

    if (state == 0) {
        index = 0;
        prefix = text ? text : "";
    }

    while (index < g_completionWords.size()) {
        const std::string candidate = g_completionWords[index++];
        if (candidate.rfind(prefix, 0) == 0) {
            return ::strdup(candidate.c_str());
        }
    }
    return nullptr;
}

char** attemptedCompletion(const char* text, int, int)
{
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, completionGenerator);
}
#endif

using apc::util::trim;

bool isExitCommand(const std::string& command)
{
    return command == "exit" || command == "quit" || command == "exit()" ||
           command == "quit()" || command == "%exit" || command == "%quit";
}

std::string normalizeCommand(std::string command)
{
    command = trim(std::move(command));
    if (!command.empty() && command.front() == '%') {
        command.erase(command.begin());
    }
    return command;
}

std::vector<std::string> splitCommandLine(const std::string& command)
{
    std::vector<std::string> parts;
    std::string current;
    char quote = '\0';
    bool escaped = false;

    for (char ch : command) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            } else {
                current.push_back(ch);
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        parts.push_back(std::move(current));
    }
    return parts;
}

bool isBlank(const std::string& line)
{
    return trim(line).empty();
}

std::string rtrim(std::string value)
{
    const auto end = value.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) {
        return "";
    }
    value.erase(end + 1);
    return value;
}

bool hasBlockIntro(const std::string& source)
{
    std::istringstream input(source);
    std::string line;
    while (std::getline(input, line)) {
        const std::string stripped = rtrim(line);
        if (!stripped.empty() && stripped.back() == ':') {
            return true;
        }
    }
    return false;
}

bool delimitersBalanced(const std::string& source)
{
    std::vector<char> stack;
    bool inString = false;
    bool escaped = false;

    for (char ch : source) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{') {
            stack.push_back(ch);
            continue;
        }
        if (ch == ')' || ch == ']' || ch == '}') {
            if (stack.empty()) {
                return false;
            }
            const char open = stack.back();
            if ((open == '(' && ch != ')') || (open == '[' && ch != ']') ||
                (open == '{' && ch != '}')) {
                return false;
            }
            stack.pop_back();
        }
    }

    return !inString && stack.empty();
}

bool shouldExecuteCell(const std::string& buffer, const std::string& lastLine)
{
    if (!delimitersBalanced(buffer)) {
        return false;
    }
    if (hasBlockIntro(buffer)) {
        return isBlank(lastLine);
    }
    return true;
}

std::string formatValue(const apc_interpreter::RuntimeValue& value)
{
    return value.toDisplayString();
}

void printOutput(
    std::ostream& output,
    int executionCount,
    const std::vector<apc_interpreter::RuntimeValue>& values
)
{
    if (values.empty()) {
        return;
    }
    if (values.size() == 1) {
        output << "Out[" << executionCount << "]: "
               << formatValue(values.front()) << '\n';
        return;
    }

    output << "Out[" << executionCount << "]:\n";
    for (size_t i = 0; i < values.size(); ++i) {
        output << "  [" << i << "] " << formatValue(values[i]) << '\n';
    }
}

void printHelp(std::ostream& output)
{
    output << "AtomicProof interactive shell\n";
    output << "Commands: exit, quit, help, history, clear\n";
    output << "Magic commands: %run <file.ct>, %load <file.ct>, %debug [file.ct], %reset, %who\n";
    output << "Enter expressions or statements directly. Finish def/Struct/if/for "
              "blocks with an empty line.\n";
}

void printHistory(
    std::ostream& output,
    const std::vector<std::string>& cellHistory
)
{
    for (size_t i = 0; i < cellHistory.size(); ++i) {
        std::istringstream lines(cellHistory[i]);
        std::string line;
        bool first = true;
        while (std::getline(lines, line)) {
            if (first) {
                output << i + 1 << ": " << line << '\n';
                first = false;
            } else {
                output << "   " << line << '\n';
            }
        }
    }
}

void printNames(std::ostream& output, const std::vector<std::string>& names)
{
    if (names.empty()) {
        output << "(no user names)\n";
        return;
    }

    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            output << "  ";
        }
        output << names[i];
    }
    output << '\n';
}

class LineReader
{
public:
    explicit LineReader(bool interactive) : m_interactive(interactive)
    {
#ifdef APC_ENABLE_READLINE
        if (m_interactive) {
            rl_attempted_completion_function = attemptedCompletion;
        }
#endif
    }

    void setCompletionWords(std::vector<std::string> words)
    {
#ifdef APC_ENABLE_READLINE
        g_completionWords = std::move(words);
#else
        (void)words;
#endif
    }

    std::optional<std::string>
    readLine(const std::string& prompt, std::istream& input, std::ostream& output)
    {
#ifdef APC_ENABLE_READLINE
        if (m_interactive) {
            char* raw = ::readline(prompt.c_str());
            if (!raw) {
                return std::nullopt;
            }
            std::string line(raw);
            std::free(raw);
            return line;
        }
#endif
        output << prompt;
        output.flush();
        std::string line;
        if (!std::getline(input, line)) {
            return std::nullopt;
        }
        return line;
    }

    void addHistory(const std::string& line)
    {
#ifdef APC_ENABLE_READLINE
        if (m_interactive && !trim(line).empty()) {
            ::add_history(line.c_str());
        }
#else
        (void)line;
#endif
    }

private:
    bool m_interactive;
};

bool isInteractiveInput(std::istream& input)
{
#ifdef _WIN32
    (void)input;
    return false;
#else
    return &input == &std::cin && ::isatty(STDIN_FILENO) != 0;
#endif
}

} // namespace

int runReplShell(
    std::istream& input,
    std::ostream& output,
    std::ostream& error,
    const std::string& startupFile,
    DebugLauncher debugLauncher
)
{
    ReplSession session;
    LineReader reader(isInteractiveInput(input));
    std::vector<std::string> cellHistory;
    std::string buffer;
    int executionCount = 1;

    output << "AtomicProof Interpreter REPL. Type help for help, exit to quit.\n";

    if (!startupFile.empty()) {
        ReplCellResult startupResult = session.loadFile(startupFile);
        if (!startupResult.success) {
            error << "Startup error: " << startupResult.errorMessage << '\n';
            return 1;
        }
        if (!startupResult.message.empty()) {
            output << startupResult.message << '\n';
        }
    }

    while (true) {
        reader.setCompletionWords(session.completionWords());
        const std::string prompt =
            buffer.empty()
                ? "In [" + std::to_string(executionCount) + "]: "
                : "   ...: ";

        std::optional<std::string> maybeLine =
            reader.readLine(prompt, input, output);
        if (!maybeLine.has_value()) {
            if (!buffer.empty()) {
                ReplCellResult result = session.executeCell(buffer);
                cellHistory.push_back(rtrim(buffer));
                if (result.success && result.hasOutput) {
                    printOutput(output, executionCount, result.outputValues);
                } else if (!result.success) {
                    error << "Error[" << executionCount << "]: "
                          << result.errorMessage << '\n';
                }
            }
            output << '\n';
            break;
        }

        std::string line = *maybeLine;
        reader.addHistory(line);

        if (buffer.empty()) {
            const std::string command = normalizeCommand(line);
            const auto commandParts = splitCommandLine(command);
            if (isExitCommand(command)) {
                output << "Bye.\n";
                return 0;
            }
            if (command == "help") {
                printHelp(output);
                continue;
            }
            if (command == "history") {
                printHistory(output, cellHistory);
                continue;
            }
            if (command == "clear") {
                output << "\033[2J\033[H";
                continue;
            }
            if (!commandParts.empty() &&
                (commandParts[0] == "run" || commandParts[0] == "load")) {
                if (commandParts.size() < 2) {
                    error << "Usage: %" << commandParts[0] << " <file.ct>\n";
                    continue;
                }
                ReplCellResult result = session.loadFile(commandParts[1]);
                if (result.success) {
                    if (!result.message.empty()) {
                        output << result.message << '\n';
                    }
                } else {
                    error << "Error: " << result.errorMessage << '\n';
                }
                continue;
            }
            if (!commandParts.empty() && commandParts[0] == "debug") {
                std::string debugFile;
                if (commandParts.size() >= 2) {
                    debugFile = commandParts[1];
                } else {
                    debugFile = session.lastLoadedFile();
                }

                if (debugFile.empty()) {
                    error << "Usage: %debug <file.ct> "
                          << "(or load a file first with %run)\n";
                    continue;
                }
                if (!debugLauncher) {
                    error << "Debugger support is not available in this build.\n";
                    continue;
                }

                output << "Entering debugger for " << debugFile << '\n';
                const int debugStatus = debugLauncher(debugFile);
                if (debugStatus == 0) {
                    output << "Debugger exited.\n";
                } else {
                    error << "Debugger exited with status " << debugStatus
                          << ".\n";
                }
                continue;
            }
            if (command == "reset") {
                session.reset();
                cellHistory.clear();
                buffer.clear();
                executionCount = 1;
                output << "Session reset.\n";
                continue;
            }
            if (command == "who") {
                printNames(output, session.userNames());
                continue;
            }
            if (trim(line).empty()) {
                continue;
            }
        }

        buffer += line;
        buffer += '\n';

        if (!shouldExecuteCell(buffer, line)) {
            continue;
        }

        ReplCellResult result = session.executeCell(buffer);
        cellHistory.push_back(rtrim(buffer));
        if (result.success && !result.message.empty()) {
            output << result.message << '\n';
        }
        if (result.success && result.hasOutput) {
            printOutput(output, executionCount, result.outputValues);
        } else if (!result.success) {
            error << "Error[" << executionCount << "]: " << result.errorMessage
                  << '\n';
        }

        buffer.clear();
        ++executionCount;
    }

    return 0;
}

} // namespace apc_repl
