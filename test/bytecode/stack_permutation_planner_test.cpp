#include "bytecode/stack_permutation_planner.h"
#include "bytecode/bytecode_helper_fun.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

using tbc::StackPermutationPlan;
using tbc::StackPermutationPlanner;
using tbc::StackPlanOp;
using tbc::StackPlanStep;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T>
bool roll(std::vector<T>& stack, size_t depth)
{
    if (depth >= stack.size()) {
        return false;
    }
    T value = stack[depth];
    stack.erase(stack.begin() + static_cast<ptrdiff_t>(depth));
    stack.insert(stack.begin(), std::move(value));
    return true;
}

template <typename T>
bool applyStep(
    std::vector<T>& main,
    std::vector<T>& alt,
    const StackPlanStep& step
)
{
    switch (step.op) {
        case StackPlanOp::Swap:
            if (main.size() < 2)
                return false;
            std::swap(main[0], main[1]);
            return true;
        case StackPlanOp::Rot:
            return roll(main, 2);
        case StackPlanOp::TwoSwap:
            if (main.size() < 4)
                return false;
            std::rotate(main.begin(), main.begin() + 2, main.begin() + 4);
            return true;
        case StackPlanOp::TwoRot:
            if (main.size() < 6)
                return false;
            std::rotate(main.begin(), main.begin() + 4, main.begin() + 6);
            return true;
        case StackPlanOp::Roll:
            return roll(main, step.depth);
        case StackPlanOp::Dup:
            if (main.empty())
                return false;
            main.insert(main.begin(), main[0]);
            return true;
        case StackPlanOp::Over:
            if (main.size() < 2)
                return false;
            main.insert(main.begin(), main[1]);
            return true;
        case StackPlanOp::Pick:
            if (step.depth >= main.size())
                return false;
            main.insert(main.begin(), main[step.depth]);
            return true;
        case StackPlanOp::Drop:
            if (main.empty())
                return false;
            main.erase(main.begin());
            return true;
        case StackPlanOp::Nip:
            if (main.size() < 2)
                return false;
            main.erase(main.begin() + 1);
            return true;
        case StackPlanOp::Tuck:
            if (main.size() < 2)
                return false;
            main.insert(main.begin() + 2, main[0]);
            return true;
        case StackPlanOp::ToAltStack:
            if (main.empty())
                return false;
            alt.insert(alt.begin(), main.front());
            main.erase(main.begin());
            return true;
        case StackPlanOp::FromAltStack:
            if (alt.empty())
                return false;
            main.insert(main.begin(), alt.front());
            alt.erase(alt.begin());
            return true;
    }
    return false;
}

template <typename T>
void applyPlan(
    std::vector<T>& main,
    std::vector<T>& alt,
    const StackPermutationPlan& plan
)
{
    for (const auto& step : plan.steps) {
        require(applyStep(main, alt, step), "plan contains an invalid step");
    }
}

void validatePlanMetrics(const StackPermutationPlan& plan)
{
    size_t bytes = 0;
    size_t instructions = 0;
    size_t maximumDepth = 0;
    for (const auto& step : plan.steps) {
        bytes += step.serializedByteSize();
        instructions += step.emittedInstructionCount();
        maximumDepth = std::max(maximumDepth, step.touchedDepth());
    }
    require(bytes == plan.serializedBytes, "serialized-byte total drifted");
    require(
        instructions == plan.emittedInstructions,
        "instruction total drifted"
    );
    require(
        maximumDepth == plan.maximumTouchedDepth,
        "maximum touched depth drifted"
    );
    require(
        plan.encodedInstructions().size() == plan.emittedInstructions,
        "encoded instruction count drifted"
    );
}

std::vector<StackPlanStep> referenceMoveOperations(size_t size)
{
    std::vector<StackPlanStep> result;
    if (size >= 2)
        result.push_back({StackPlanOp::Swap, 1});
    if (size >= 3)
        result.push_back({StackPlanOp::Rot, 2});
    if (size >= 4)
        result.push_back({StackPlanOp::TwoSwap, 3});
    if (size >= 6)
        result.push_back({StackPlanOp::TwoRot, 5});
    for (size_t depth = 3; depth < size; ++depth)
        result.push_back({StackPlanOp::Roll, depth});
    return result;
}

std::map<std::vector<size_t>, size_t> referenceMoveDistances(size_t size)
{
    using Item = std::pair<size_t, std::vector<size_t>>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    std::map<std::vector<size_t>, size_t> distance;

    std::vector<size_t> identity(size);
    std::iota(identity.begin(), identity.end(), size_t{0});
    distance[identity] = 0;
    queue.push({0, identity});

    const auto operations = referenceMoveOperations(size);
    while (!queue.empty()) {
        auto [cost, state] = queue.top();
        queue.pop();
        if (distance[state] != cost) {
            continue;
        }
        for (const auto& operation : operations) {
            auto next = state;
            std::vector<size_t> unusedAlt;
            require(
                applyStep(next, unusedAlt, operation),
                "reference move rejected a legal operation"
            );
            const size_t nextCost = cost + operation.serializedByteSize();
            auto found = distance.find(next);
            if (found != distance.end() && found->second <= nextCost) {
                continue;
            }
            distance[next] = nextCost;
            queue.push({nextCost, std::move(next)});
        }
    }
    return distance;
}

void testEncodingCosts()
{
    require(
        StackPlanStep{StackPlanOp::Swap, 1}.serializedByteSize() == 1,
        "SWAP must be one byte"
    );
    require(
        StackPlanStep{StackPlanOp::TwoRot, 5}.serializedByteSize() == 1,
        "2ROT must be one byte"
    );
    require(
        StackPlanStep{StackPlanOp::Roll, 3}.serializedByteSize() == 2,
        "ROLL(3) must be OP_3 + OP_ROLL"
    );
    require(
        StackPlanStep{StackPlanOp::Roll, 0}.encodedInstructions().empty(),
        "ROLL(0) must be canonicalized to a no-op"
    );
    require(
        StackPlanStep{StackPlanOp::Roll, 1}.encodedInstructions() ==
            StackPlanStep{StackPlanOp::Swap, 1}.encodedInstructions(),
        "ROLL(1) must be canonicalized to SWAP"
    );
    require(
        StackPlanStep{StackPlanOp::Roll, 2}.encodedInstructions() ==
            StackPlanStep{StackPlanOp::Rot, 2}.encodedInstructions(),
        "ROLL(2) must be canonicalized to ROT"
    );
    require(
        StackPlanStep{StackPlanOp::Pick, 0}.encodedInstructions() ==
            StackPlanStep{StackPlanOp::Dup, 0}.encodedInstructions(),
        "PICK(0) must be canonicalized to DUP"
    );
    require(
        StackPlanStep{StackPlanOp::Pick, 1}.encodedInstructions() ==
            StackPlanStep{StackPlanOp::Over, 1}.encodedInstructions(),
        "PICK(1) must be canonicalized to OVER"
    );
    require(
        StackPlanStep{StackPlanOp::Roll, 16}.serializedByteSize() == 2,
        "ROLL(16) must use OP_16"
    );
    require(
        StackPlanStep{StackPlanOp::Roll, 17}.serializedByteSize() == 3,
        "ROLL(17) must use a two-byte numeric push"
    );
    require(
        StackPlanStep{StackPlanOp::Pick, 17}.serializedByteSize() == 3,
        "PICK(17) must use a two-byte numeric push"
    );
}

void testKnownMovePlans()
{
    auto twoSwap = StackPermutationPlanner::planMoveOnly({2, 3, 0, 1});
    require(twoSwap.has_value(), "2SWAP target must be plannable");
    require(twoSwap->serializedBytes == 1, "2SWAP target must cost one byte");
    require(
        twoSwap->steps.size() == 1 &&
            twoSwap->steps[0].op == StackPlanOp::TwoSwap,
        "2SWAP target must use OP_2SWAP"
    );

    auto twoRot =
        StackPermutationPlanner::planMoveOnly({4, 5, 0, 1, 2, 3});
    require(twoRot.has_value(), "2ROT target must be plannable");
    require(twoRot->serializedBytes == 1, "2ROT target must cost one byte");
    require(
        twoRot->steps.size() == 1 &&
            twoRot->steps[0].op == StackPlanOp::TwoRot,
        "2ROT target must use OP_2ROT"
    );

    auto named = StackPermutationPlanner::planMoveOnly(
        std::vector<uint64_t>{91, 17, 42, 8},
        std::vector<uint64_t>{42, 8, 91, 17}
    );
    require(named.has_value(), "stable slot ids must be accepted");
    require(named->serializedBytes == 1, "named 2SWAP must cost one byte");

    require(
        !StackPermutationPlanner::planMoveOnly({0, 0}).has_value(),
        "duplicate slot ids must be rejected"
    );
    require(
        !StackPermutationPlanner::planMoveOnly(
             std::vector<size_t>(
                 StackPermutationPlanner::MAX_MOVE_WINDOW + 1, 0
             )
         )
             .has_value(),
        "oversized windows must fall back"
    );
}

void testAllMovePermutations()
{
    for (size_t size = 0;
         size <= StackPermutationPlanner::MAX_MOVE_WINDOW;
         ++size) {
        std::vector<size_t> target(size);
        std::iota(target.begin(), target.end(), size_t{0});

        std::optional<std::map<std::vector<size_t>, size_t>> oracle;
        if (size <= 6) {
            oracle = referenceMoveDistances(size);
        }

        do {
            auto plan = StackPermutationPlanner::planMoveOnly(target);
            require(plan.has_value(), "valid permutation was not planned");
            validatePlanMetrics(*plan);

            std::vector<size_t> actual(size);
            std::iota(actual.begin(), actual.end(), size_t{0});
            actual.push_back(1000);
            actual.push_back(1001);
            std::vector<size_t> alt;
            applyPlan(actual, alt, *plan);
            std::vector<size_t> expected = target;
            expected.push_back(1000);
            expected.push_back(1001);
            require(actual == expected, "move-only plan reached wrong layout");
            require(alt.empty(), "move-only plan touched altstack");

            if (oracle.has_value()) {
                require(
                    plan->serializedBytes == oracle->at(target),
                    "move-only plan is not byte-optimal"
                );
            }

            auto repeated = StackPermutationPlanner::planMoveOnly(target);
            require(repeated.has_value(), "cached plan disappeared");
            require(
                repeated->encodedInstructions() == plan->encodedInstructions(),
                "move-only plan is not deterministic"
            );
        } while (std::next_permutation(target.begin(), target.end()));
    }
}

void testCopyAssignments()
{
    for (size_t source = 0;
         source <= StackPermutationPlanner::MAX_COPY_DEPTH;
         ++source) {
        for (size_t target = 0;
             target <= StackPermutationPlanner::MAX_COPY_DEPTH;
             ++target) {
            if (source == target) {
                require(
                    !StackPermutationPlanner::planCopyAssignment(source, target)
                         .has_value(),
                    "self-copy must not be planned"
                );
                continue;
            }

            const size_t legacy =
                StackPermutationPlanner::legacyCopyAssignmentByteCost(
                    source, target
                );
            auto plan =
                StackPermutationPlanner::planCopyAssignment(source, target);
            require(plan.has_value(), "copy assignment was not planned");
            validatePlanMetrics(*plan);
            require(
                plan->serializedBytes <= legacy,
                "copy planner exceeded the known legacy sequence"
            );

            const size_t windowSize = std::max(source, target) + 1;
            std::vector<size_t> actual(windowSize);
            std::iota(actual.begin(), actual.end(), size_t{0});
            actual.push_back(1000);
            actual.push_back(1001);
            std::vector<size_t> alt{2000, 2001};
            applyPlan(actual, alt, *plan);

            std::vector<size_t> expected(windowSize);
            std::iota(expected.begin(), expected.end(), size_t{0});
            expected[target] = source;
            expected.push_back(1000);
            expected.push_back(1001);
            require(actual == expected, "copy plan reached wrong main layout");
            require(
                alt == std::vector<size_t>({2000, 2001}),
                "copy plan did not restore the pre-existing altstack"
            );

            auto strict = StackPermutationPlanner::planCopyAssignment(
                source, target, legacy
            );
            require(
                strict.has_value() == (plan->serializedBytes < legacy),
                "strict legacy comparison returned the wrong result"
            );
            if (strict.has_value()) {
                require(
                    strict->encodedInstructions() ==
                        plan->encodedInstructions(),
                    "strict lookup changed the optimal plan"
                );
            }
        }
    }

    require(
        !StackPermutationPlanner::planCopyAssignment(0, 1, 2).has_value(),
        "the two-byte adjacent copy has no strict improvement"
    );
    require(
        !StackPermutationPlanner::planCopyAssignment(7, 0).has_value(),
        "copy depth beyond the cap must fall back"
    );
    require(
        !StackPermutationPlanner::planCopyAssignment(0, 7).has_value(),
        "target depth beyond the cap must fall back"
    );
}

void testInt64MinimumScriptNumber()
{
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    const std::string encoded = tbc::numberToScriptHex(minimum);
    require(
        encoded == "0x09000000000000008080",
        "INT64_MIN ScriptNum encoding changed"
    );
    require(
        tbc::isStrictScriptNumberHex(encoded),
        "INT64_MIN should be accepted as the unique nine-byte int64"
    );
    require(
        tbc::scriptHexToNumber(encoded) == minimum,
        "INT64_MIN ScriptNum round-trip failed"
    );
    require(
        !tbc::isStrictScriptNumberHex("0x09010000000000008080"),
        "other nine-byte ScriptNums must not be accepted as int64"
    );
}

} // namespace

int main()
{
    try {
        testEncodingCosts();
        testKnownMovePlans();
        testAllMovePermutations();
        testCopyAssignments();
        testInt64MinimumScriptNumber();
        std::cout << "stack_permutation_planner_test: all checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "stack_permutation_planner_test: " << error.what()
                  << '\n';
        return 1;
    }
}
