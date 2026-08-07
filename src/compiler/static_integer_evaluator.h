#ifndef STATIC_INTEGER_EVALUATOR_H
#define STATIC_INTEGER_EVALUATOR_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "../ast/ast.h"

namespace compiler
{

enum class StaticIntegerResultKind {
    Known,
    Unknown,
    Error,
};

struct StaticIntegerResult
{
    StaticIntegerResultKind kind{StaticIntegerResultKind::Unknown};
    int64_t value{0};
    std::string diagnostic;

    static StaticIntegerResult known(int64_t value);
    static StaticIntegerResult unknown();
    static StaticIntegerResult error(std::string diagnostic);

    bool isKnown() const
    {
        return kind == StaticIntegerResultKind::Known;
    }

    bool isUnknown() const
    {
        return kind == StaticIntegerResultKind::Unknown;
    }

    bool isError() const
    {
        return kind == StaticIntegerResultKind::Error;
    }
};

class StaticIntegerEvaluator
{
public:
    using Bindings = std::map<std::string, int64_t>;
    using IdentifierResolver =
        std::function<StaticIntegerResult(const IdentifierNode&)>;

    // Literal-only expressions can use this overload. Unknown identifiers and
    // unsupported AST shapes return Unknown rather than producing diagnostics.
    static StaticIntegerResult evaluate(const ExprNode& expression);

    // Convenient for pre-analysis, where compile-time integer bindings already
    // live in a name-to-value map.
    static StaticIntegerResult evaluate(
        const ExprNode& expression,
        const Bindings& bindings
    );

    // Backends can resolve identifiers from Scope/fixed storage while retaining
    // the evaluator's checked arithmetic and tri-state result semantics.
    static StaticIntegerResult evaluate(
        const ExprNode& expression,
        const IdentifierResolver& resolveIdentifier
    );
};

} // namespace compiler

#endif // STATIC_INTEGER_EVALUATOR_H
