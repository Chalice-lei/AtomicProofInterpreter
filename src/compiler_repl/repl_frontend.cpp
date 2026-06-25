#include "repl_frontend.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "../util/string_utils.h"

namespace apc::repl
{
namespace
{
using apc::util::startsWith;
using apc::util::trim;

std::string firstLine(const std::string& text)
{
    std::istringstream iss(text);
    std::string line;
    std::getline(iss, line);
    return trim(line);
}

int bracketBalance(const std::string& text)
{
    int balance = 0;
    bool inString = false;
    bool escaped = false;

    for (char c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
        } else if (c == '(' || c == '[' || c == '{') {
            ++balance;
        } else if (c == ')' || c == ']' || c == '}') {
            --balance;
        }
    }

    return balance;
}

bool endsWithColon(const std::string& line)
{
    std::string value = trim(line);
    return !value.empty() && value.back() == ':';
}
} // namespace

int ReplFrontend::run()
{
    std::cout << "AtomicProof Compiler REPL" << std::endl;
    std::cout << "Type %help for commands, exit to quit." << std::endl;

    while (true) {
        int inputIndex = m_session.nextInputIndex();
        auto cellOpt = readCell(inputIndex);
        if (!cellOpt.has_value()) {
            std::cout << std::endl;
            break;
        }

        std::string cell = *cellOpt;
        std::string text = trim(cell);
        if (text.empty()) {
            continue;
        }

        appendHistory(cell);

        if (isExitCommand(text)) {
            break;
        }

        if (m_magicCommands.isMagic(text)) {
            m_magicCommands.handle(text, m_session, m_cellCompiler);
            m_session.advanceInputIndex();
            continue;
        }

        auto result =
            m_cellCompiler.executeCell(m_session, inputIndex, cell);
        if (!result.success) {
            std::cout << "Error: " << result.errorMessage << std::endl;
            m_session.advanceInputIndex();
            continue;
        }

        if (result.hasOutput) {
            std::cout << "Out[" << inputIndex << "]: " << result.output
                      << std::endl;
        }

        m_session.advanceInputIndex();
    }

    return 0;
}

std::optional<std::string> ReplFrontend::readCell(int inputIndex) const
{
    std::cout << "In [" << inputIndex << "]: ";
    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }

    std::string text = line;
    if (trim(text).empty()) {
        return text;
    }

    if (m_magicCommands.isMagic(text) || isExitCommand(trim(text))) {
        return text;
    }

    while (shouldContinueInput(text)) {
        std::cout << "   ...: ";
        std::string nextLine;
        if (!std::getline(std::cin, nextLine)) {
            break;
        }

        if (trim(nextLine).empty() && bracketBalance(text) <= 0) {
            break;
        }

        text += "\n";
        text += nextLine;
    }

    return text;
}

bool ReplFrontend::shouldContinueInput(const std::string& currentText) const
{
    std::string first = firstLine(currentText);
    bool blockHeader = startsWith(first, "def ") ||
                       startsWith(first, "Struct ") ||
                       startsWith(first, "if ") ||
                       startsWith(first, "for ") ||
                       endsWithColon(first);

    if (blockHeader) {
        return true;
    }

    if (bracketBalance(currentText) > 0) {
        return true;
    }

    std::string value = trim(currentText);
    return !value.empty() && value.back() == '\\';
}

void ReplFrontend::appendHistory(const std::string& cellText) const
{
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        return;
    }

    std::string path = std::string(home) + "/.apc_repl_history";
    std::ofstream history(path, std::ios::app);
    if (!history) {
        return;
    }

    history << cellText << "\n";
}

bool ReplFrontend::isExitCommand(const std::string& text) const
{
    std::string value = trim(text);
    return value == "exit" || value == "quit" || value == "%exit" ||
           value == "%quit";
}

} // namespace apc::repl
