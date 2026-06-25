#include "runtime_error.h"

namespace apc_interpreter
{

RuntimeError::RuntimeError(
    RuntimeErrorKind kind,
    const std::string& message,
    SourceLocation location,
    std::string suggestion
)
    : std::runtime_error(message),
      m_kind(kind),
      m_location(std::move(location)),
      m_suggestion(std::move(suggestion))
{}

std::string RuntimeError::kindName() const
{
    switch (m_kind) {
        case RuntimeErrorKind::Generic:
            return "generic";
        case RuntimeErrorKind::TypeMismatch:
            return "type_mismatch";
        case RuntimeErrorKind::UndefinedVariable:
            return "undefined_variable";
        case RuntimeErrorKind::DuplicateVariable:
            return "duplicate_variable";
        case RuntimeErrorKind::OwnershipViolation:
            return "ownership_violation";
        case RuntimeErrorKind::InvalidFieldAccess:
            return "invalid_field_access";
        case RuntimeErrorKind::InvalidIndexAccess:
            return "invalid_index_access";
        case RuntimeErrorKind::BuiltinError:
            return "builtin_error";
    }
    return "unknown";
}

} // namespace apc_interpreter
