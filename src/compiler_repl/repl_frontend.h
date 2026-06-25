#ifndef REPL_FRONTEND_H
#define REPL_FRONTEND_H

#include <optional>
#include <string>

#include "cell_compiler.h"
#include "magic_commands.h"
#include "repl_session.h"

namespace apc::repl
{

class ReplFrontend
{
public:
    int run();

private:
    std::optional<std::string> readCell(int inputIndex) const;
    bool shouldContinueInput(const std::string& currentText) const;
    void appendHistory(const std::string& cellText) const;
    bool isExitCommand(const std::string& text) const;

private:
    ReplSession m_session;
    CellCompiler m_cellCompiler;
    MagicCommands m_magicCommands;
};

} // namespace apc::repl

#endif // REPL_FRONTEND_H
