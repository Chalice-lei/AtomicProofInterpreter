#ifndef STACK_ARGUMENT_H
#define STACK_ARGUMENT_H

#include <string>

#include "stack_state.h"

namespace apc_debug
{

enum class StackArgumentStatus {
    Parsed,
    DefaultEmpty,
    InvalidAddress,
    InvalidHex,
    InvalidNumber
};

struct StackArgumentParseResult
{
    StackElement value;
    StackArgumentStatus status = StackArgumentStatus::Parsed;
    bool convertedAddress = false;
};

StackElement defaultStackArgumentValue(const std::string& typeName);
StackArgumentParseResult parseStackArgumentValueDetailed(
    const std::string& rawInput,
    const std::string& typeName
);
StackElement parseStackArgumentValue(
    const std::string& rawInput,
    const std::string& typeName
);

} // namespace apc_debug

#endif // STACK_ARGUMENT_H
