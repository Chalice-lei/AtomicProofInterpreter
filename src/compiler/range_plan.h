#ifndef RANGE_PLAN_H
#define RANGE_PLAN_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace apc::compiler
{

enum class RangeErrorCode {
    ZeroStep,
    CountOverflow,
    IterationLimitExceeded,
    IndexOutOfRange,
    ValueOverflow,
    BudgetExceeded
};

struct RangeError
{
    RangeErrorCode code{RangeErrorCode::CountOverflow};
    std::string message;
    std::optional<uint64_t> limit;
    std::optional<uint64_t> observed;
};

struct RangeLimits
{
    // No allocation is proportional to this value.  The limit is only a
    // policy guard for clients which statically expand a RangePlan.
    std::optional<uint64_t> maxIterations;
};

// A shared expansion budget can be consumed one iteration at a time by nested
// loop executors.  RangePlan::build deliberately does not reserve from it:
// callers may terminate a loop body early and should pay only for iterations
// they actually analyze or emit.
class RangeExpansionBudget
{
public:
    explicit RangeExpansionBudget(uint64_t limit) : m_limit(limit)
    {}

    std::optional<RangeError> consume(uint64_t amount = 1);

    uint64_t limit() const
    {
        return m_limit;
    }
    uint64_t consumed() const
    {
        return m_consumed;
    }
    uint64_t remaining() const
    {
        return m_limit - m_consumed;
    }

private:
    uint64_t m_limit{0};
    uint64_t m_consumed{0};
};

using RangeValueResult = std::variant<int64_t, RangeError>;

// A compact, lazy representation of Python-style range(start, stop, step).
// It stores no per-iteration values, so even a plan with UINT64_MAX elements
// has constant memory cost.
class RangePlan
{
public:
    class BuildResult;

    static BuildResult build(
        int64_t start,
        int64_t stop,
        int64_t step,
        RangeLimits limits = {}
    );

    int64_t start() const
    {
        return m_start;
    }
    int64_t stop() const
    {
        return m_stop;
    }
    int64_t step() const
    {
        return m_step;
    }
    uint64_t count() const
    {
        return m_count;
    }
    bool empty() const
    {
        return m_count == 0;
    }

    RangeValueResult valueAt(uint64_t index) const;

private:
    RangePlan(int64_t start, int64_t stop, int64_t step, uint64_t count)
        : m_start(start), m_stop(stop), m_step(step), m_count(count)
    {}

    int64_t m_start{0};
    int64_t m_stop{0};
    int64_t m_step{1};
    uint64_t m_count{0};
};

class RangePlan::BuildResult
{
public:
    explicit BuildResult(RangePlan plan) : m_result(std::move(plan))
    {}
    explicit BuildResult(RangeError error) : m_result(std::move(error))
    {}

    bool hasValue() const
    {
        return std::holds_alternative<RangePlan>(m_result);
    }
    explicit operator bool() const
    {
        return hasValue();
    }

    const RangePlan& value() const
    {
        return std::get<RangePlan>(m_result);
    }
    RangePlan& value()
    {
        return std::get<RangePlan>(m_result);
    }
    const RangeError& error() const
    {
        return std::get<RangeError>(m_result);
    }

private:
    std::variant<RangePlan, RangeError> m_result;
};

} // namespace apc::compiler

#endif // RANGE_PLAN_H
