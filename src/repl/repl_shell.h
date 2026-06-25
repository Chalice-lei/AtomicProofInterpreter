#ifndef REPL_SHELL_H
#define REPL_SHELL_H

#include <functional>
#include <iosfwd>
#include <string>

namespace apc_repl
{

using DebugLauncher = std::function<int(const std::string& filename)>;

int runReplShell(
    std::istream& input,
    std::ostream& output,
    std::ostream& error,
    const std::string& startupFile = "",
    DebugLauncher debugLauncher = {}
);

} // namespace apc_repl

#endif // REPL_SHELL_H
