#ifndef LIVE_DEBUG_SERVER_H
#define LIVE_DEBUG_SERVER_H

#include <iosfwd>
#include <string>
#include <vector>

namespace apc_debug
{

struct LiveDebugServerOptions
{
    std::string sourceFile;
    std::string sourceCode;
    std::string functionName;
    std::vector<std::string> args;
    std::string txFile;
    bool allowSubscopeAltstack = false;
};

int runLiveDebugServer(
    std::istream& input,
    std::ostream& output,
    std::ostream& error,
    const LiveDebugServerOptions& options
);

} // namespace apc_debug

#endif // LIVE_DEBUG_SERVER_H
