#ifndef BUILTIN_RUNTIME_H
#define BUILTIN_RUNTIME_H

#include <string>
#include <vector>

#include "../ast/ast.h"
#include "runtime_value.h"

namespace apc_interpreter
{

class BuiltinRuntime
{
public:
    static bool isBuiltinFunction(const std::string& name);

    static RuntimeValue callFunction(
        const std::string& name,
        const std::vector<RuntimeValue>& args,
        SourceLocation location = SourceLocation()
    );
};

} // namespace apc_interpreter

#endif // BUILTIN_RUNTIME_H
