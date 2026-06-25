#ifndef RUNTIME_ERROR_H
#define RUNTIME_ERROR_H

#include <stdexcept>
#include <string>

#include "../error/error_types.h"

namespace apc_interpreter
{

enum class RuntimeErrorKind {
    Generic,
    TypeMismatch,
    UndefinedVariable,
    DuplicateVariable,
    OwnershipViolation,
    InvalidFieldAccess,
    InvalidIndexAccess,
    BuiltinError
};

class RuntimeError : public std::runtime_error
{
public:
    RuntimeError(
        RuntimeErrorKind kind,
        const std::string& message,
        SourceLocation location = SourceLocation(),
        std::string suggestion = ""
    );

    RuntimeErrorKind kind() const
    {
        return m_kind;
    }

    const SourceLocation& location() const
    {
        return m_location;
    }

    const std::string& suggestion() const
    {
        return m_suggestion;
    }

    std::string kindName() const;

private:
    RuntimeErrorKind m_kind;
    SourceLocation m_location;
    std::string m_suggestion;
};

} // namespace apc_interpreter

#endif // RUNTIME_ERROR_H
