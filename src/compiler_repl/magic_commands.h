#ifndef MAGIC_COMMANDS_H
#define MAGIC_COMMANDS_H

#include <string>

#include "cell_compiler.h"
#include "repl_session.h"

namespace apc::repl
{

class MagicCommands
{
public:
    bool isMagic(const std::string& line) const;

    bool handle(
        const std::string& line,
        ReplSession& session,
        const CellCompiler& cellCompiler
    ) const;

    void showHelp() const;

private:
    void cmdWho(const ReplSession& session) const;
    void cmdBytecode(const ReplSession& session) const;
    void cmdCompile(
        const std::string& args,
        const ReplSession& session,
        const CellCompiler& cellCompiler
    ) const;
    void cmdRun(const std::string& args) const;
    void cmdLoad(const std::string& args) const;
    void cmdDebug(const ReplSession& session) const;
    void cmdShell(const std::string& command) const;
};

} // namespace apc::repl

#endif // MAGIC_COMMANDS_H
