#include "compiler/static_integer_evaluator.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

using compiler::StaticIntegerEvaluator;
using compiler::StaticIntegerResult;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::unique_ptr<ExprNode> number(std::string value)
{
    return std::make_unique<LiteralNode>(
        LiteralNode::Type::Number, std::move(value)
    );
}

std::unique_ptr<ExprNode> identifier(std::string name)
{
    return std::make_unique<IdentifierNode>(std::move(name));
}

std::unique_ptr<ExprNode> unary(
    std::string operation,
    std::unique_ptr<ExprNode> rhs
)
{
    return std::make_unique<OpNode>(
        std::move(operation), nullptr, std::move(rhs)
    );
}

std::unique_ptr<ExprNode> binary(
    std::string operation,
    std::unique_ptr<ExprNode> lhs,
    std::unique_ptr<ExprNode> rhs
)
{
    return std::make_unique<OpNode>(
        std::move(operation), std::move(lhs), std::move(rhs)
    );
}

std::unique_ptr<ExprNode> clone(std::unique_ptr<ExprNode> object)
{
    return std::make_unique<MethodCallNode>(
        std::move(object),
        "Clone",
        std::vector<std::unique_ptr<ExprNode>>{}
    );
}

void expectKnown(
    const std::string& name,
    const ExprNode& expression,
    int64_t expected,
    const StaticIntegerEvaluator::Bindings& bindings = {}
)
{
    const StaticIntegerResult result =
        StaticIntegerEvaluator::evaluate(expression, bindings);
    require(result.isKnown(), name + ": expected Known result");
    require(result.value == expected, name + ": unexpected value");
    require(result.diagnostic.empty(), name + ": Known result has diagnostic");
}

void expectUnknown(
    const std::string& name,
    const ExprNode& expression,
    const StaticIntegerEvaluator::Bindings& bindings = {}
)
{
    const StaticIntegerResult result =
        StaticIntegerEvaluator::evaluate(expression, bindings);
    require(result.isUnknown(), name + ": expected Unknown result");
    require(result.diagnostic.empty(), name + ": Unknown result has diagnostic");
}

void expectError(
    const std::string& name,
    const ExprNode& expression,
    const StaticIntegerEvaluator::Bindings& bindings = {}
)
{
    const StaticIntegerResult result =
        StaticIntegerEvaluator::evaluate(expression, bindings);
    require(result.isError(), name + ": expected Error result");
    require(!result.diagnostic.empty(), name + ": Error result lacks diagnostic");
}

void testLeavesAndBindings()
{
    auto literal = number("9223372036854775807");
    expectKnown("maximum literal", *literal, std::numeric_limits<int64_t>::max());

    auto invalid = number("12x");
    expectError("malformed numeric literal", *invalid);

    auto tooLarge = number("9223372036854775808");
    expectError("out-of-range numeric literal", *tooLarge);

    LiteralNode text(LiteralNode::Type::String, "12");
    expectUnknown("non-number literal", text);

    auto missing = identifier("missing");
    expectUnknown("unbound identifier", *missing);

    auto boundClone = clone(clone(identifier("limit")));
    expectKnown(
        "bound Clone chain",
        *boundClone,
        41,
        StaticIntegerEvaluator::Bindings{{"limit", 41}}
    );

    CallNode unsupportedCall("Value", {});
    expectUnknown("unsupported call", unsupportedCall);

    auto customIdentifier = identifier("backendFixed");
    const auto custom = StaticIntegerEvaluator::evaluate(
        *customIdentifier,
        [](const IdentifierNode& id) {
            return id.name == "backendFixed"
                       ? StaticIntegerResult::known(73)
                       : StaticIntegerResult::unknown();
        }
    );
    require(custom.isKnown() && custom.value == 73, "custom resolver failed");
}

void testUnaryOperators()
{
    auto negate = unary("-", number("17"));
    expectKnown("unary negation", *negate, -17);

    auto minimum = unary("-", number("-9223372036854775808"));
    expectError("unary negation overflow", *minimum);

    auto notZero = unary("!", number("0"));
    expectKnown("logical not zero", *notZero, 1);

    auto notNonzero = unary("!", number("-9"));
    expectKnown("logical not nonzero", *notNonzero, 0);

    auto unsupported = unary("~", number("1"));
    expectUnknown("unsupported unary operator", *unsupported);
}

void testCheckedArithmetic()
{
    auto add = binary("+", number("20"), number("22"));
    expectKnown("addition", *add, 42);
    auto addOverflow = binary(
        "+", number("9223372036854775807"), number("1")
    );
    expectError("addition overflow", *addOverflow);

    auto subtract = binary("-", number("20"), number("22"));
    expectKnown("subtraction", *subtract, -2);
    auto subtractOverflow = binary(
        "-", number("-9223372036854775808"), number("1")
    );
    expectError("subtraction overflow", *subtractOverflow);

    auto multiply = binary("*", number("-7"), number("6"));
    expectKnown("multiplication", *multiply, -42);
    auto multiplyOverflow = binary(
        "*", number("-9223372036854775808"), number("-1")
    );
    expectError("multiplication overflow", *multiplyOverflow);

    auto divide = binary("/", number("-43"), number("2"));
    expectKnown("division", *divide, -21);
    auto divideZero = binary("/", number("1"), number("0"));
    expectError("division by zero", *divideZero);
    auto divideOverflow = binary(
        "/", number("-9223372036854775808"), number("-1")
    );
    expectError("division overflow", *divideOverflow);

    auto modulo = binary("%", number("-43"), number("2"));
    expectKnown("modulo", *modulo, -1);
    auto moduloZero = binary("%", number("1"), number("0"));
    expectError("modulo by zero", *moduloZero);
    auto moduloOverflow = binary(
        "%", number("-9223372036854775808"), number("-1")
    );
    expectError("modulo overflow", *moduloOverflow);

    auto unknownWithError = binary(
        "+",
        identifier("runtime"),
        binary("/", number("1"), number("0"))
    );
    expectError("error takes precedence over unknown", *unknownWithError);
}

void testComparisonsAndLogic()
{
    const struct
    {
        const char* operation;
        int64_t lhs;
        int64_t rhs;
        int64_t expected;
    } cases[] = {
        {"==", 4, 4, 1},
        {"!=", 4, 5, 1},
        {"<", -1, 0, 1},
        {">", 9, 2, 1},
        {"<=", 2, 2, 1},
        {">=", 1, 2, 0},
        {"&&", -2, 7, 1},
        {"&&", 0, 7, 0},
        {"||", 0, -3, 1},
        {"||", 0, 0, 0},
    };

    for (const auto& test : cases) {
        auto expression = binary(
            test.operation,
            number(std::to_string(test.lhs)),
            number(std::to_string(test.rhs))
        );
        expectKnown(
            std::string("binary ") + test.operation,
            *expression,
            test.expected
        );
    }

    auto unsupported = binary("&", number("1"), number("1"));
    expectUnknown("unsupported binary operator", *unsupported);
}

} // namespace

int main()
{
    try {
        testLeavesAndBindings();
        testUnaryOperators();
        testCheckedArithmetic();
        testComparisonsAndLogic();
        std::cout << "static_integer_evaluator_test: all checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "static_integer_evaluator_test: " << error.what() << '\n';
        return 1;
    }
}
