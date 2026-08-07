#include "range_plan.h"

#include <bit>
#include <limits>
#include <string>

namespace apc::compiler
{
namespace
{

constexpr uint64_t signBit = uint64_t{1} << 63;

// Transform signed values into an unsigned domain which preserves ordering.
// This makes distances spanning zero representable without signed overflow.
uint64_t ordered(int64_t value)
{
    return std::bit_cast<uint64_t>(value) ^ signBit;
}

int64_t fromOrdered(uint64_t value)
{
    return std::bit_cast<int64_t>(value ^ signBit);
}

uint64_t stepMagnitude(int64_t step)
{
    if (step >= 0)
        return static_cast<uint64_t>(step);

    // Negating INT64_MIN is undefined.  Offset before negation instead.
    return static_cast<uint64_t>(-(step + 1)) + 1;
}

RangeError makeError(
    RangeErrorCode code,
    std::string message,
    std::optional<uint64_t> limit = std::nullopt,
    std::optional<uint64_t> observed = std::nullopt
)
{
    return RangeError{code, std::move(message), limit, observed};
}

} // namespace

std::optional<RangeError> RangeExpansionBudget::consume(uint64_t amount)
{
    if (amount > remaining()) {
        return makeError(
            RangeErrorCode::BudgetExceeded,
            "range expansion budget exceeded: requested "
                + std::to_string(amount) + " with "
                + std::to_string(remaining()) + " remaining",
            m_limit,
            amount
        );
    }

    m_consumed += amount;
    return std::nullopt;
}

RangePlan::BuildResult RangePlan::build(
    int64_t start,
    int64_t stop,
    int64_t step,
    RangeLimits limits
)
{
    if (step == 0) {
        return BuildResult(makeError(
            RangeErrorCode::ZeroStep,
            "range step must not be zero"
        ));
    }

    const bool progresses = step > 0 ? start < stop : start > stop;
    uint64_t count = 0;
    if (progresses) {
        const uint64_t distance = step > 0
            ? ordered(stop) - ordered(start)
            : ordered(start) - ordered(stop);
        const uint64_t magnitude = stepMagnitude(step);
        const uint64_t quotient = distance / magnitude;
        const bool hasRemainder = distance % magnitude != 0;

        if (hasRemainder
            && quotient == std::numeric_limits<uint64_t>::max()) {
            return BuildResult(makeError(
                RangeErrorCode::CountOverflow,
                "range iteration count does not fit in uint64_t"
            ));
        }
        count = quotient + static_cast<uint64_t>(hasRemainder);
    }

    if (limits.maxIterations && count > *limits.maxIterations) {
        return BuildResult(makeError(
            RangeErrorCode::IterationLimitExceeded,
            "range iteration count " + std::to_string(count)
                + " exceeds limit "
                + std::to_string(*limits.maxIterations),
            limits.maxIterations,
            count
        ));
    }

    return BuildResult(RangePlan(start, stop, step, count));
}

RangeValueResult RangePlan::valueAt(uint64_t index) const
{
    if (index >= m_count) {
        return makeError(
            RangeErrorCode::IndexOutOfRange,
            "range index " + std::to_string(index)
                + " is outside count " + std::to_string(m_count),
            m_count,
            index
        );
    }

    const uint64_t magnitude = stepMagnitude(m_step);
    if (index != 0
        && magnitude > std::numeric_limits<uint64_t>::max() / index) {
        return makeError(
            RangeErrorCode::ValueOverflow,
            "range value offset does not fit in uint64_t",
            std::nullopt,
            index
        );
    }
    const uint64_t offset = magnitude * index;
    const uint64_t orderedStart = ordered(m_start);

    if (m_step > 0) {
        if (offset > std::numeric_limits<uint64_t>::max() - orderedStart) {
            return makeError(
                RangeErrorCode::ValueOverflow,
                "range value exceeds INT64_MAX",
                std::nullopt,
                index
            );
        }
        return fromOrdered(orderedStart + offset);
    }

    if (offset > orderedStart) {
        return makeError(
            RangeErrorCode::ValueOverflow,
            "range value is below INT64_MIN",
            std::nullopt,
            index
        );
    }
    return fromOrdered(orderedStart - offset);
}

} // namespace apc::compiler
