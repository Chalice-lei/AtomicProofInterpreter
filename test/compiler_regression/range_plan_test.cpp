#include "compiler/range_plan.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace
{

using apc::compiler::RangeErrorCode;
using apc::compiler::RangeExpansionBudget;
using apc::compiler::RangeLimits;
using apc::compiler::RangePlan;
using apc::compiler::RangeValueResult;

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "range_plan_test: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message)
{
    if (!condition)
        fail(message);
}

RangePlan requirePlan(RangePlan::BuildResult result, const std::string& label)
{
    if (!result)
        fail(label + ": unexpected build error: " + result.error().message);
    return result.value();
}

int64_t requireValue(RangeValueResult result, const std::string& label)
{
    if (const auto* value = std::get_if<int64_t>(&result))
        return *value;
    fail(label + ": unexpected value error: "
        + std::get<apc::compiler::RangeError>(result).message);
}

void requireValueError(
    RangeValueResult result,
    RangeErrorCode expected,
    const std::string& label
)
{
    const auto* error = std::get_if<apc::compiler::RangeError>(&result);
    require(error != nullptr, label + ": expected value error");
    require(error->code == expected, label + ": wrong value error code");
}

void testPositiveAndNegativeRanges()
{
    const auto unit = requirePlan(RangePlan::build(0, 5, 1), "unit range");
    require(unit.count() == 5, "unit range count");
    for (uint64_t i = 0; i < unit.count(); ++i)
        require(requireValue(unit.valueAt(i), "unit value")
                == static_cast<int64_t>(i),
            "unit range value");
    requireValueError(
        unit.valueAt(unit.count()),
        RangeErrorCode::IndexOutOfRange,
        "unit end"
    );

    const auto stride = requirePlan(RangePlan::build(1, 10, 3), "stride");
    require(stride.count() == 3, "positive stride count");
    require(requireValue(stride.valueAt(0), "stride[0]") == 1,
        "stride[0]");
    require(requireValue(stride.valueAt(1), "stride[1]") == 4,
        "stride[1]");
    require(requireValue(stride.valueAt(2), "stride[2]") == 7,
        "stride[2]");

    const auto descending = requirePlan(
        RangePlan::build(5, 0, -2), "descending"
    );
    require(descending.count() == 3, "descending count");
    require(requireValue(descending.valueAt(0), "descending[0]") == 5,
        "descending[0]");
    require(requireValue(descending.valueAt(1), "descending[1]") == 3,
        "descending[1]");
    require(requireValue(descending.valueAt(2), "descending[2]") == 1,
        "descending[2]");
}

void testEmptyAndInvalidRanges()
{
    require(requirePlan(RangePlan::build(5, 5, 1), "equal positive").empty(),
        "equal positive should be empty");
    require(requirePlan(RangePlan::build(5, 5, -1), "equal negative").empty(),
        "equal negative should be empty");
    require(requirePlan(RangePlan::build(5, 0, 1), "wrong positive").empty(),
        "wrong-direction positive should be empty");
    const auto wrongNegative = requirePlan(
        RangePlan::build(0, 5, -1), "wrong negative"
    );
    require(wrongNegative.empty(),
        "wrong-direction negative should be empty");
    requireValueError(
        wrongNegative.valueAt(0),
        RangeErrorCode::IndexOutOfRange,
        "empty value"
    );

    const auto zero = RangePlan::build(0, 5, 0);
    require(!zero, "zero step should fail");
    require(zero.error().code == RangeErrorCode::ZeroStep,
        "zero step error code");
}

void testSmallRangeMatrix()
{
    for (int64_t start = -20; start <= 20; ++start) {
        for (int64_t stop = -20; stop <= 20; ++stop) {
            for (int64_t step = -7; step <= 7; ++step) {
                if (step == 0)
                    continue;

                const auto plan = requirePlan(
                    RangePlan::build(start, stop, step),
                    "small range matrix"
                );
                uint64_t expectedCount = 0;
                for (int64_t value = start;
                     step > 0 ? value < stop : value > stop;
                     value += step) {
                    require(requireValue(
                                plan.valueAt(expectedCount),
                                "small range matrix value"
                            ) == value,
                        "small range matrix value mismatch");
                    ++expectedCount;
                }
                require(plan.count() == expectedCount,
                    "small range matrix count mismatch");
            }
        }
    }
}

void testExtremeEndpoints()
{
    constexpr int64_t min = std::numeric_limits<int64_t>::min();
    constexpr int64_t max = std::numeric_limits<int64_t>::max();
    constexpr uint64_t umax = std::numeric_limits<uint64_t>::max();

    const auto ascending = requirePlan(
        RangePlan::build(min, max, 1), "full ascending"
    );
    require(ascending.count() == umax, "full ascending count");
    require(requireValue(ascending.valueAt(0), "full ascending first") == min,
        "full ascending first");
    require(requireValue(ascending.valueAt(umax - 1), "full ascending last")
            == max - 1,
        "full ascending last");
    requireValueError(
        ascending.valueAt(umax),
        RangeErrorCode::IndexOutOfRange,
        "full ascending end"
    );

    const auto descending = requirePlan(
        RangePlan::build(max, min, -1), "full descending"
    );
    require(descending.count() == umax, "full descending count");
    require(requireValue(descending.valueAt(0), "full descending first") == max,
        "full descending first");
    require(requireValue(descending.valueAt(umax - 1), "full descending last")
            == min + 1,
        "full descending last");

    const auto stepTwo = requirePlan(
        RangePlan::build(min, max, 2), "full step two"
    );
    require(stepTwo.count() == uint64_t{1} << 63, "full step-two count");
    require(requireValue(
                stepTwo.valueAt((uint64_t{1} << 63) - 1),
                "full step-two last"
            ) == max - 1,
        "full step-two last");

    const auto minimumStep = requirePlan(
        RangePlan::build(max, min, min), "minimum step"
    );
    require(minimumStep.count() == 2, "minimum step count");
    require(requireValue(minimumStep.valueAt(0), "minimum step first") == max,
        "minimum step first");
    require(requireValue(minimumStep.valueAt(1), "minimum step second") == -1,
        "minimum step second");

    const auto maximumStep = requirePlan(
        RangePlan::build(min, max, max), "maximum step"
    );
    require(maximumStep.count() == 3, "maximum step count");
    require(requireValue(maximumStep.valueAt(0), "maximum step first") == min,
        "maximum step first");
    require(requireValue(maximumStep.valueAt(1), "maximum step second") == -1,
        "maximum step second");
    require(requireValue(maximumStep.valueAt(2), "maximum step third") == max - 1,
        "maximum step third");
}

void testLimitsAndBudget()
{
    const auto rejected = RangePlan::build(0, 5, 1, RangeLimits{4});
    require(!rejected, "iteration limit should reject large plan");
    require(rejected.error().code == RangeErrorCode::IterationLimitExceeded,
        "iteration limit error code");
    require(rejected.error().limit == 4, "iteration limit diagnostic");
    require(rejected.error().observed == 5, "iteration count diagnostic");

    require(RangePlan::build(0, 5, 1, RangeLimits{5}).hasValue(),
        "equal iteration limit should pass");

    constexpr int64_t min = std::numeric_limits<int64_t>::min();
    constexpr int64_t max = std::numeric_limits<int64_t>::max();
    const auto hugeRejected = RangePlan::build(min, max, 1, RangeLimits{100});
    require(!hugeRejected, "huge range should be rejected by limit");
    require(hugeRejected.error().observed
            == std::numeric_limits<uint64_t>::max(),
        "huge iteration count diagnostic");

    RangeExpansionBudget budget(3);
    require(!budget.consume(0), "zero budget consumption");
    require(!budget.consume(1), "first budget consumption");
    require(!budget.consume(2), "remaining budget consumption");
    require(budget.consumed() == 3, "budget consumed count");
    require(budget.remaining() == 0, "budget remaining count");
    const auto exceeded = budget.consume();
    require(exceeded.has_value(), "exhausted budget should fail");
    require(exceeded->code == RangeErrorCode::BudgetExceeded,
        "budget error code");
    require(budget.consumed() == 3,
        "failed budget consumption should not mutate state");

    RangeExpansionBudget maximumBudget(
        std::numeric_limits<uint64_t>::max()
    );
    require(!maximumBudget.consume(std::numeric_limits<uint64_t>::max()),
        "maximum budget consumption");
    require(maximumBudget.consume(1).has_value(),
        "maximum budget overflow guard");
}

} // namespace

int main()
{
    testPositiveAndNegativeRanges();
    testEmptyAndInvalidRanges();
    testSmallRangeMatrix();
    testExtremeEndpoints();
    testLimitsAndBudget();
    return 0;
}
