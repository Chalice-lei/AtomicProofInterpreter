#include "static_integer_evaluator.h"

#include <charconv>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace compiler
{
namespace
{

StaticIntegerResult parseIntegerLiteral(const LiteralNode& literal)
{
    if (literal.type != LiteralNode::Type::Number) {
        return StaticIntegerResult::unknown();
    }

    int64_t value = 0;
    const std::string_view text(literal.value);
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec == std::errc::result_out_of_range) {
        return StaticIntegerResult::error(
            "integer literal is outside the int64 range"
        );
    }
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return StaticIntegerResult::error("invalid integer literal");
    }
    return StaticIntegerResult::known(value);
}

StaticIntegerResult checkedAdd(int64_t lhs, int64_t rhs)
{
    constexpr int64_t min = std::numeric_limits<int64_t>::min();
    constexpr int64_t max = std::numeric_limits<int64_t>::max();
    if ((rhs > 0 && lhs > max - rhs) ||
        (rhs < 0 && lhs < min - rhs)) {
        return StaticIntegerResult::error("integer addition overflow");
    }
    return StaticIntegerResult::known(lhs + rhs);
}

StaticIntegerResult checkedSubtract(int64_t lhs, int64_t rhs)
{
    constexpr int64_t min = std::numeric_limits<int64_t>::min();
    constexpr int64_t max = std::numeric_limits<int64_t>::max();
    if ((rhs < 0 && lhs > max + rhs) ||
        (rhs > 0 && lhs < min + rhs)) {
        return StaticIntegerResult::error("integer subtraction overflow");
    }
    return StaticIntegerResult::known(lhs - rhs);
}

StaticIntegerResult checkedMultiply(int64_t lhs, int64_t rhs)
{
    constexpr int64_t min = std::numeric_limits<int64_t>::min();
    constexpr int64_t max = std::numeric_limits<int64_t>::max();

    if (lhs == 0 || rhs == 0) {
        return StaticIntegerResult::known(0);
    }
    if ((lhs == -1 && rhs == min) || (rhs == -1 && lhs == min)) {
        return StaticIntegerResult::error("integer multiplication overflow");
    }
    if (lhs > 0) {
        if ((rhs > 0 && lhs > max / rhs) ||
            (rhs < 0 && rhs < min / lhs)) {
            return StaticIntegerResult::error(
                "integer multiplication overflow"
            );
        }
    } else if ((rhs > 0 && lhs < min / rhs) ||
               (rhs < 0 && lhs < max / rhs)) {
        return StaticIntegerResult::error("integer multiplication overflow");
    }
    return StaticIntegerResult::known(lhs * rhs);
}

StaticIntegerResult checkedDivide(int64_t lhs, int64_t rhs)
{
    constexpr int64_t min = std::numeric_limits<int64_t>::min();
    if (rhs == 0) {
        return StaticIntegerResult::error("integer division by zero");
    }
    if (lhs == min && rhs == -1) {
        return StaticIntegerResult::error("integer division overflow");
    }
    return StaticIntegerResult::known(lhs / rhs);
}

StaticIntegerResult checkedModulo(int64_t lhs, int64_t rhs)
{
    constexpr int64_t min = std::numeric_limits<int64_t>::min();
    if (rhs == 0) {
        return StaticIntegerResult::error("integer modulo by zero");
    }
    // In C++, INT64_MIN % -1 has the same unrepresentable quotient as division
    // and is undefined even though the mathematical remainder is zero.
    if (lhs == min && rhs == -1) {
        return StaticIntegerResult::error("integer modulo overflow");
    }
    return StaticIntegerResult::known(lhs % rhs);
}

StaticIntegerResult evaluateExpression(
    const ExprNode& expression,
    const StaticIntegerEvaluator::IdentifierResolver& resolveIdentifier
)
{
    if (const auto* literal = dynamic_cast<const LiteralNode*>(&expression)) {
        return parseIntegerLiteral(*literal);
    }

    if (const auto* identifier =
            dynamic_cast<const IdentifierNode*>(&expression)) {
        return resolveIdentifier ? resolveIdentifier(*identifier)
                                 : StaticIntegerResult::unknown();
    }

    if (const auto* method =
            dynamic_cast<const MethodCallNode*>(&expression)) {
        if (method->methodName == "Clone" && method->args.empty() &&
            method->object) {
            return evaluateExpression(*method->object, resolveIdentifier);
        }
        return StaticIntegerResult::unknown();
    }

    const auto* operation = dynamic_cast<const OpNode*>(&expression);
    if (!operation || !operation->rhs) {
        return StaticIntegerResult::unknown();
    }

    if (!operation->lhs) {
        StaticIntegerResult rhs =
            evaluateExpression(*operation->rhs, resolveIdentifier);
        if (!rhs.isKnown()) {
            return rhs;
        }
        if (operation->op == "-") {
            if (rhs.value == std::numeric_limits<int64_t>::min()) {
                return StaticIntegerResult::error("integer negation overflow");
            }
            return StaticIntegerResult::known(-rhs.value);
        }
        if (operation->op == "!") {
            return StaticIntegerResult::known(rhs.value == 0 ? 1 : 0);
        }
        return StaticIntegerResult::unknown();
    }

    // Arithmetic and logical opcodes evaluate both operands in the current
    // bytecode backend. Preserve an error from either side even when the other
    // side is not statically known.
    StaticIntegerResult lhs =
        evaluateExpression(*operation->lhs, resolveIdentifier);
    StaticIntegerResult rhs =
        evaluateExpression(*operation->rhs, resolveIdentifier);
    if (lhs.isError()) {
        return lhs;
    }
    if (rhs.isError()) {
        return rhs;
    }
    if (!lhs.isKnown() || !rhs.isKnown()) {
        return StaticIntegerResult::unknown();
    }

    const int64_t a = lhs.value;
    const int64_t b = rhs.value;
    if (operation->op == "+") {
        return checkedAdd(a, b);
    }
    if (operation->op == "-") {
        return checkedSubtract(a, b);
    }
    if (operation->op == "*") {
        return checkedMultiply(a, b);
    }
    if (operation->op == "/") {
        return checkedDivide(a, b);
    }
    if (operation->op == "%") {
        return checkedModulo(a, b);
    }
    if (operation->op == "==") {
        return StaticIntegerResult::known(a == b ? 1 : 0);
    }
    if (operation->op == "!=") {
        return StaticIntegerResult::known(a != b ? 1 : 0);
    }
    if (operation->op == "<") {
        return StaticIntegerResult::known(a < b ? 1 : 0);
    }
    if (operation->op == ">") {
        return StaticIntegerResult::known(a > b ? 1 : 0);
    }
    if (operation->op == "<=") {
        return StaticIntegerResult::known(a <= b ? 1 : 0);
    }
    if (operation->op == ">=") {
        return StaticIntegerResult::known(a >= b ? 1 : 0);
    }
    if (operation->op == "&&") {
        return StaticIntegerResult::known(a != 0 && b != 0 ? 1 : 0);
    }
    if (operation->op == "||") {
        return StaticIntegerResult::known(a != 0 || b != 0 ? 1 : 0);
    }
    return StaticIntegerResult::unknown();
}

} // namespace

StaticIntegerResult StaticIntegerResult::known(int64_t value)
{
    return {StaticIntegerResultKind::Known, value, {}};
}

StaticIntegerResult StaticIntegerResult::unknown()
{
    return {StaticIntegerResultKind::Unknown, 0, {}};
}

StaticIntegerResult StaticIntegerResult::error(std::string diagnostic)
{
    return {
        StaticIntegerResultKind::Error, 0, std::move(diagnostic)
    };
}

StaticIntegerResult StaticIntegerEvaluator::evaluate(
    const ExprNode& expression
)
{
    return evaluate(expression, Bindings{});
}

StaticIntegerResult StaticIntegerEvaluator::evaluate(
    const ExprNode& expression,
    const Bindings& bindings
)
{
    return evaluate(
        expression,
        [&bindings](const IdentifierNode& identifier) {
            const auto binding = bindings.find(identifier.name);
            return binding == bindings.end()
                       ? StaticIntegerResult::unknown()
                       : StaticIntegerResult::known(binding->second);
        }
    );
}

StaticIntegerResult StaticIntegerEvaluator::evaluate(
    const ExprNode& expression,
    const IdentifierResolver& resolveIdentifier
)
{
    return evaluateExpression(expression, resolveIdentifier);
}

} // namespace compiler
