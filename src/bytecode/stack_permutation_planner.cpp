#include "stack_permutation_planner.h"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "bytecode_helper_fun.h"

namespace tbc
{
namespace
{

struct PlanCost
{
    size_t bytes{std::numeric_limits<size_t>::max()};
    size_t instructions{std::numeric_limits<size_t>::max()};
    size_t maximumDepth{std::numeric_limits<size_t>::max()};

    static PlanCost zero()
    {
        return {0, 0, 0};
    }

    bool finite() const
    {
        return bytes != std::numeric_limits<size_t>::max();
    }
};

bool operator<(const PlanCost& lhs, const PlanCost& rhs)
{
    return std::tie(lhs.bytes, lhs.instructions, lhs.maximumDepth) <
           std::tie(rhs.bytes, rhs.instructions, rhs.maximumDepth);
}

bool operator==(const PlanCost& lhs, const PlanCost& rhs)
{
    return lhs.bytes == rhs.bytes && lhs.instructions == rhs.instructions &&
           lhs.maximumDepth == rhs.maximumDepth;
}

PlanCost extendCost(const PlanCost& cost, const StackPlanStep& step)
{
    return {
        cost.bytes + step.serializedByteSize(),
        cost.instructions + step.emittedInstructionCount(),
        std::max(cost.maximumDepth, step.touchedDepth())
    };
}

StackPermutationPlan makePlan(std::vector<StackPlanStep> steps)
{
    StackPermutationPlan plan;
    plan.steps = std::move(steps);
    for (const auto& step : plan.steps) {
        plan.serializedBytes += step.serializedByteSize();
        plan.emittedInstructions += step.emittedInstructionCount();
        plan.maximumTouchedDepth =
            std::max(plan.maximumTouchedDepth, step.touchedDepth());
    }
    return plan;
}

size_t encodedByteSize(const std::string& encoded)
{
    size_t offset = 0;
    if (encoded.size() >= 2 && encoded[0] == '0' &&
        (encoded[1] == 'x' || encoded[1] == 'X')) {
        offset = 2;
    }
    const size_t hexDigits = encoded.size() - offset;
    if (hexDigits % 2 != 0) {
        throw std::invalid_argument("script hex must contain complete bytes");
    }
    return hexDigits / 2;
}

size_t factorial(size_t n)
{
    static constexpr std::array<size_t, 9> values{
        1, 1, 2, 6, 24, 120, 720, 5040, 40320
    };
    if (n >= values.size()) {
        throw std::invalid_argument("permutation is too large to rank");
    }
    return values[n];
}

size_t rankPermutation(const std::vector<uint8_t>& permutation)
{
    size_t rank = 0;
    const size_t n = permutation.size();
    for (size_t i = 0; i < n; ++i) {
        size_t smaller = 0;
        for (size_t j = i + 1; j < n; ++j) {
            if (permutation[j] < permutation[i]) {
                ++smaller;
            }
        }
        rank += smaller * factorial(n - i - 1);
    }
    return rank;
}

std::vector<uint8_t> unrankPermutation(size_t n, size_t rank)
{
    std::vector<uint8_t> available(n);
    std::iota(available.begin(), available.end(), uint8_t{0});

    std::vector<uint8_t> result;
    result.reserve(n);
    for (size_t remaining = n; remaining > 0; --remaining) {
        const size_t blockSize = factorial(remaining - 1);
        const size_t selected = rank / blockSize;
        rank %= blockSize;
        result.push_back(available.at(selected));
        available.erase(available.begin() + static_cast<ptrdiff_t>(selected));
    }
    return result;
}

bool applyMoveOnly(
    std::vector<uint8_t>& stack,
    const StackPlanStep& step
)
{
    auto roll = [&stack](size_t depth) {
        if (depth >= stack.size()) {
            return false;
        }
        const uint8_t element = stack[depth];
        stack.erase(stack.begin() + static_cast<ptrdiff_t>(depth));
        stack.insert(stack.begin(), element);
        return true;
    };

    switch (step.op) {
        case StackPlanOp::Swap:
            if (stack.size() < 2)
                return false;
            std::swap(stack[0], stack[1]);
            return true;
        case StackPlanOp::Rot:
            return roll(2);
        case StackPlanOp::TwoSwap:
            if (stack.size() < 4)
                return false;
            std::rotate(stack.begin(), stack.begin() + 2, stack.begin() + 4);
            return true;
        case StackPlanOp::TwoRot:
            if (stack.size() < 6)
                return false;
            std::rotate(stack.begin(), stack.begin() + 4, stack.begin() + 6);
            return true;
        case StackPlanOp::Roll:
            return roll(step.depth);
        default:
            return false;
    }
}

std::vector<StackPlanStep> moveOnlyOperations(size_t stackSize)
{
    std::vector<StackPlanStep> result;
    result.reserve(stackSize + 1);
    if (stackSize >= 2) {
        result.push_back({StackPlanOp::Swap, 1});
    }
    if (stackSize >= 3) {
        result.push_back({StackPlanOp::Rot, 2});
    }
    if (stackSize >= 4) {
        result.push_back({StackPlanOp::TwoSwap, 3});
    }
    if (stackSize >= 6) {
        result.push_back({StackPlanOp::TwoRot, 5});
    }
    for (size_t depth = 3; depth < stackSize; ++depth) {
        result.push_back({StackPlanOp::Roll, depth});
    }
    return result;
}

struct MoveTableEntry
{
    PlanCost cost;
    size_t predecessor{std::numeric_limits<size_t>::max()};
    StackPlanStep incoming;
};

struct MoveTable
{
    size_t windowSize{0};
    std::vector<MoveTableEntry> entries;
};

struct MoveQueueItem
{
    PlanCost cost;
    size_t rank{0};
};

struct MoveQueueGreater
{
    bool operator()(const MoveQueueItem& lhs, const MoveQueueItem& rhs) const
    {
        if (lhs.cost == rhs.cost) {
            return lhs.rank > rhs.rank;
        }
        return rhs.cost < lhs.cost;
    }
};

MoveTable buildMoveTable(size_t windowSize)
{
    MoveTable table;
    table.windowSize = windowSize;
    table.entries.resize(factorial(windowSize));
    table.entries[0].cost = PlanCost::zero();

    std::priority_queue<
        MoveQueueItem,
        std::vector<MoveQueueItem>,
        MoveQueueGreater>
        queue;
    queue.push({PlanCost::zero(), 0});

    const auto operations = moveOnlyOperations(windowSize);
    while (!queue.empty()) {
        const MoveQueueItem item = queue.top();
        queue.pop();
        if (!(item.cost == table.entries[item.rank].cost)) {
            continue;
        }

        const auto state = unrankPermutation(windowSize, item.rank);
        for (const auto& operation : operations) {
            auto next = state;
            if (!applyMoveOnly(next, operation)) {
                continue;
            }
            const size_t nextRank = rankPermutation(next);
            const PlanCost nextCost = extendCost(item.cost, operation);
            if (!nextCost.finite() ||
                !(nextCost < table.entries[nextRank].cost)) {
                continue;
            }
            table.entries[nextRank].cost = nextCost;
            table.entries[nextRank].predecessor = item.rank;
            table.entries[nextRank].incoming = operation;
            queue.push({nextCost, nextRank});
        }
    }

    return table;
}

const MoveTable& getMoveTable(size_t windowSize)
{
    struct CacheEntry
    {
        std::once_flag initialized;
        std::unique_ptr<MoveTable> table;
    };
    static std::array<CacheEntry, 9> tables;

    CacheEntry& entry = tables[windowSize];
    std::call_once(entry.initialized, [&entry, windowSize]() {
        entry.table =
            std::make_unique<MoveTable>(buildMoveTable(windowSize));
    });
    return *entry.table;
}

struct CopyState
{
    std::vector<uint8_t> main;
    // Both stacks are top-first.  alt contains only entries temporarily
    // pushed by the plan; any pre-existing shared-altstack suffix is frozen.
    std::vector<uint8_t> alt;
    bool copied{false};
    bool deleted{false};
};

std::string copyStateKey(const CopyState& state)
{
    std::string key;
    key.reserve(state.main.size() + state.alt.size() + 4);
    key.push_back(static_cast<char>(state.copied));
    key.push_back(static_cast<char>(state.deleted));
    key.push_back(static_cast<char>(state.main.size()));
    for (uint8_t item : state.main) {
        key.push_back(static_cast<char>(item + 1));
    }
    key.push_back(static_cast<char>(state.alt.size()));
    for (uint8_t item : state.alt) {
        key.push_back(static_cast<char>(item + 1));
    }
    return key;
}

bool isCopyGoal(
    const CopyState& state,
    const std::vector<uint8_t>& goal
)
{
    return state.copied && state.deleted && state.alt.empty() &&
           state.main == goal;
}

struct CopyRecord
{
    PlanCost cost;
    std::string predecessor;
    StackPlanStep incoming;
};

struct CopyQueueItem
{
    PlanCost cost;
    std::string key;
    CopyState state;
};

struct CopyQueueGreater
{
    bool operator()(const CopyQueueItem& lhs, const CopyQueueItem& rhs) const
    {
        if (lhs.cost == rhs.cost) {
            return lhs.key > rhs.key;
        }
        return rhs.cost < lhs.cost;
    }
};

void addCopyNeighbour(
    const CopyState& state,
    const StackPlanStep& operation,
    std::vector<std::pair<CopyState, StackPlanStep>>& neighbours
)
{
    CopyState next = state;
    auto roll = [&next](size_t depth) {
        if (depth >= next.main.size()) {
            return false;
        }
        const uint8_t element = next.main[depth];
        next.main.erase(
            next.main.begin() + static_cast<ptrdiff_t>(depth)
        );
        next.main.insert(next.main.begin(), element);
        return true;
    };

    bool applied = true;
    switch (operation.op) {
        case StackPlanOp::Swap:
            if (next.main.size() < 2) {
                applied = false;
            } else {
                std::swap(next.main[0], next.main[1]);
            }
            break;
        case StackPlanOp::Rot:
            applied = roll(2);
            break;
        case StackPlanOp::TwoSwap:
            if (next.main.size() < 4) {
                applied = false;
            } else {
                std::rotate(
                    next.main.begin(),
                    next.main.begin() + 2,
                    next.main.begin() + 4
                );
            }
            break;
        case StackPlanOp::TwoRot:
            if (next.main.size() < 6) {
                applied = false;
            } else {
                std::rotate(
                    next.main.begin(),
                    next.main.begin() + 4,
                    next.main.begin() + 6
                );
            }
            break;
        case StackPlanOp::Roll:
            applied = roll(operation.depth);
            break;
        case StackPlanOp::Dup:
            if (next.main.empty() || next.copied) {
                applied = false;
            } else {
                next.main.insert(next.main.begin(), next.main[0]);
                next.copied = true;
            }
            break;
        case StackPlanOp::Over:
            if (next.main.size() < 2 || next.copied) {
                applied = false;
            } else {
                next.main.insert(next.main.begin(), next.main[1]);
                next.copied = true;
            }
            break;
        case StackPlanOp::Pick:
            if (operation.depth >= next.main.size() || next.copied) {
                applied = false;
            } else {
                next.main.insert(
                    next.main.begin(), next.main[operation.depth]
                );
                next.copied = true;
            }
            break;
        case StackPlanOp::Drop:
            if (next.main.empty() || next.deleted) {
                applied = false;
            } else {
                next.main.erase(next.main.begin());
                next.deleted = true;
            }
            break;
        case StackPlanOp::Nip:
            if (next.main.size() < 2 || next.deleted) {
                applied = false;
            } else {
                next.main.erase(next.main.begin() + 1);
                next.deleted = true;
            }
            break;
        case StackPlanOp::Tuck:
            if (next.main.size() < 2 || next.copied) {
                applied = false;
            } else {
                next.main.insert(next.main.begin() + 2, next.main[0]);
                next.copied = true;
            }
            break;
        case StackPlanOp::ToAltStack:
            if (next.main.empty()) {
                applied = false;
            } else {
                const uint8_t top = next.main.front();
                next.main.erase(next.main.begin());
                next.alt.insert(next.alt.begin(), top);
            }
            break;
        case StackPlanOp::FromAltStack:
            if (next.alt.empty()) {
                applied = false;
            } else {
                const uint8_t top = next.alt.front();
                next.alt.erase(next.alt.begin());
                next.main.insert(next.main.begin(), top);
            }
            break;
    }

    if (applied) {
        neighbours.emplace_back(std::move(next), operation);
    }
}

std::vector<std::pair<CopyState, StackPlanStep>> copyNeighbours(
    const CopyState& state,
    uint8_t sourceToken,
    uint8_t targetToken
)
{
    std::vector<std::pair<CopyState, StackPlanStep>> result;

    // A valid one-copy solution must duplicate the source token, and its one
    // deletion must remove the old target token.  Filtering those transitions
    // keeps the exact search small without excluding a supported solution.
    if (!state.copied) {
        if (!state.main.empty() && state.main[0] == sourceToken) {
            addCopyNeighbour(state, {StackPlanOp::Dup, 0}, result);
            if (state.main.size() >= 2) {
                addCopyNeighbour(state, {StackPlanOp::Tuck, 1}, result);
            }
        }
        if (state.main.size() >= 2 && state.main[1] == sourceToken) {
            addCopyNeighbour(state, {StackPlanOp::Over, 1}, result);
        }
        for (size_t depth = 2; depth < state.main.size(); ++depth) {
            if (state.main[depth] == sourceToken) {
                addCopyNeighbour(
                    state, {StackPlanOp::Pick, depth}, result
                );
            }
        }
    }

    if (!state.deleted) {
        if (!state.main.empty() && state.main[0] == targetToken) {
            addCopyNeighbour(state, {StackPlanOp::Drop, 0}, result);
        }
        if (state.main.size() >= 2 && state.main[1] == targetToken) {
            addCopyNeighbour(state, {StackPlanOp::Nip, 1}, result);
        }
    }

    const auto moves = moveOnlyOperations(state.main.size());
    for (const auto& move : moves) {
        addCopyNeighbour(state, move, result);
    }
    if (!state.main.empty()) {
        addCopyNeighbour(state, {StackPlanOp::ToAltStack, 0}, result);
    }
    if (!state.alt.empty()) {
        addCopyNeighbour(state, {StackPlanOp::FromAltStack, 0}, result);
    }
    return result;
}

std::optional<StackPermutationPlan> searchCopyAssignment(
    size_t sourceDepth,
    size_t targetDepth,
    size_t exclusiveByteLimit
)
{
    const size_t windowSize = std::max(sourceDepth, targetDepth) + 1;
    CopyState initial;
    initial.main.resize(windowSize);
    std::iota(initial.main.begin(), initial.main.end(), uint8_t{0});

    std::vector<uint8_t> goal = initial.main;
    goal[targetDepth] = static_cast<uint8_t>(sourceDepth);

    const std::string initialKey = copyStateKey(initial);
    std::unordered_map<std::string, CopyRecord> records;
    records.emplace(initialKey, CopyRecord{PlanCost::zero(), {}, {}});

    std::priority_queue<
        CopyQueueItem,
        std::vector<CopyQueueItem>,
        CopyQueueGreater>
        queue;
    queue.push({PlanCost::zero(), initialKey, initial});

    std::string goalKey;
    while (!queue.empty()) {
        CopyQueueItem item = queue.top();
        queue.pop();

        const auto currentRecord = records.find(item.key);
        if (currentRecord == records.end() ||
            !(currentRecord->second.cost == item.cost)) {
            continue;
        }
        if (isCopyGoal(item.state, goal)) {
            goalKey = item.key;
            break;
        }

        for (auto& [nextState, operation] :
             copyNeighbours(
                 item.state,
                 static_cast<uint8_t>(sourceDepth),
                 static_cast<uint8_t>(targetDepth)
             )) {
            const PlanCost nextCost = extendCost(item.cost, operation);
            if (nextCost.bytes >= exclusiveByteLimit) {
                continue;
            }
            std::string nextKey = copyStateKey(nextState);
            auto nextRecord = records.find(nextKey);
            if (nextRecord != records.end() &&
                !(nextCost < nextRecord->second.cost)) {
                continue;
            }

            records[nextKey] = CopyRecord{nextCost, item.key, operation};
            queue.push(
                {nextCost, std::move(nextKey), std::move(nextState)}
            );
        }
    }

    if (goalKey.empty()) {
        return std::nullopt;
    }

    std::vector<StackPlanStep> reversed;
    std::string cursor = goalKey;
    while (cursor != initialKey) {
        const auto record = records.find(cursor);
        if (record == records.end() || record->second.predecessor.empty()) {
            throw std::logic_error("broken copy-assignment predecessor chain");
        }
        reversed.push_back(record->second.incoming);
        cursor = record->second.predecessor;
    }
    std::reverse(reversed.begin(), reversed.end());
    return makePlan(std::move(reversed));
}

const std::optional<StackPermutationPlan>& getCachedCopyAssignmentPlan(
    size_t sourceDepth,
    size_t targetDepth,
    size_t legacyCost
)
{
    struct CacheEntry
    {
        std::once_flag initialized;
        std::optional<StackPermutationPlan> plan;
    };
    using CacheRow = std::array<
        CacheEntry,
        StackPermutationPlanner::MAX_COPY_DEPTH + 1>;
    static std::array<
        CacheRow,
        StackPermutationPlanner::MAX_COPY_DEPTH + 1>
        cache;

    CacheEntry& entry = cache[sourceDepth][targetDepth];
    std::call_once(entry.initialized, [&entry, sourceDepth, targetDepth,
                                       legacyCost]() {
        entry.plan = searchCopyAssignment(
            sourceDepth, targetDepth, legacyCost + 1
        );
    });
    return entry.plan;
}

size_t pickByteCost(size_t depth)
{
    if (depth <= 1) {
        return 1;
    }
    return encodedByteSize(numberToScriptHex(static_cast<int64_t>(depth))) + 1;
}

} // namespace

size_t StackPlanStep::serializedByteSize() const
{
    size_t result = 0;
    for (const auto& instruction : encodedInstructions()) {
        result += encodedByteSize(instruction);
    }
    return result;
}

size_t StackPlanStep::emittedInstructionCount() const
{
    if (op == StackPlanOp::Roll) {
        return depth == 0 ? 0 : (depth <= 2 ? 1 : 2);
    }
    if (op == StackPlanOp::Pick) {
        return depth <= 1 ? 1 : 2;
    }
    return 1;
}

size_t StackPlanStep::touchedDepth() const
{
    switch (op) {
        case StackPlanOp::Swap:
        case StackPlanOp::Over:
        case StackPlanOp::Nip:
        case StackPlanOp::Tuck:
            return 1;
        case StackPlanOp::Rot:
            return 2;
        case StackPlanOp::TwoSwap:
            return 3;
        case StackPlanOp::TwoRot:
            return 5;
        case StackPlanOp::Roll:
        case StackPlanOp::Pick:
            return depth;
        default:
            return 0;
    }
}

std::vector<std::string> StackPlanStep::encodedInstructions() const
{
    switch (op) {
        case StackPlanOp::Swap:
            return {opcodeToHex(BytOpcode::OP_SWAP)};
        case StackPlanOp::Rot:
            return {opcodeToHex(BytOpcode::OP_ROT)};
        case StackPlanOp::TwoSwap:
            return {opcodeToHex(BytOpcode::OP_2SWAP)};
        case StackPlanOp::TwoRot:
            return {opcodeToHex(BytOpcode::OP_2ROT)};
        case StackPlanOp::Roll:
            if (depth == 0) {
                return {};
            }
            if (depth == 1) {
                return {opcodeToHex(BytOpcode::OP_SWAP)};
            }
            if (depth == 2) {
                return {opcodeToHex(BytOpcode::OP_ROT)};
            }
            return {
                numberToScriptHex(static_cast<int64_t>(depth)),
                opcodeToHex(BytOpcode::OP_ROLL)
            };
        case StackPlanOp::Dup:
            return {opcodeToHex(BytOpcode::OP_DUP)};
        case StackPlanOp::Over:
            return {opcodeToHex(BytOpcode::OP_OVER)};
        case StackPlanOp::Pick:
            if (depth == 0) {
                return {opcodeToHex(BytOpcode::OP_DUP)};
            }
            if (depth == 1) {
                return {opcodeToHex(BytOpcode::OP_OVER)};
            }
            return {
                numberToScriptHex(static_cast<int64_t>(depth)),
                opcodeToHex(BytOpcode::OP_PICK)
            };
        case StackPlanOp::Drop:
            return {opcodeToHex(BytOpcode::OP_DROP)};
        case StackPlanOp::Nip:
            return {opcodeToHex(BytOpcode::OP_NIP)};
        case StackPlanOp::Tuck:
            return {opcodeToHex(BytOpcode::OP_TUCK)};
        case StackPlanOp::ToAltStack:
            return {opcodeToHex(BytOpcode::OP_TOALTSTACK)};
        case StackPlanOp::FromAltStack:
            return {opcodeToHex(BytOpcode::OP_FROMALTSTACK)};
    }
    throw std::logic_error("unknown stack-plan operation");
}

std::vector<std::string> StackPermutationPlan::encodedInstructions() const
{
    std::vector<std::string> result;
    result.reserve(emittedInstructions);
    for (const auto& step : steps) {
        auto encoded = step.encodedInstructions();
        result.insert(
            result.end(),
            std::make_move_iterator(encoded.begin()),
            std::make_move_iterator(encoded.end())
        );
    }
    return result;
}

std::optional<StackPermutationPlan>
StackPermutationPlanner::planMoveOnly(
    const std::vector<size_t>& targetTopFirst
)
{
    const size_t windowSize = targetTopFirst.size();
    if (windowSize > MAX_MOVE_WINDOW) {
        return std::nullopt;
    }
    if (windowSize == 0) {
        return StackPermutationPlan{};
    }

    std::vector<bool> seen(windowSize, false);
    std::vector<uint8_t> target;
    target.reserve(windowSize);
    for (size_t slot : targetTopFirst) {
        if (slot >= windowSize || seen[slot]) {
            return std::nullopt;
        }
        seen[slot] = true;
        target.push_back(static_cast<uint8_t>(slot));
    }

    const MoveTable& table = getMoveTable(windowSize);
    const size_t targetRank = rankPermutation(target);
    if (!table.entries[targetRank].cost.finite()) {
        return std::nullopt;
    }

    std::vector<StackPlanStep> reversed;
    size_t cursor = targetRank;
    while (cursor != 0) {
        const MoveTableEntry& entry = table.entries[cursor];
        if (entry.predecessor == std::numeric_limits<size_t>::max()) {
            throw std::logic_error("broken move-only predecessor chain");
        }
        reversed.push_back(entry.incoming);
        cursor = entry.predecessor;
    }
    std::reverse(reversed.begin(), reversed.end());
    return makePlan(std::move(reversed));
}

std::optional<StackPermutationPlan>
StackPermutationPlanner::planMoveOnly(
    const std::vector<uint64_t>& currentTopFirst,
    const std::vector<uint64_t>& targetTopFirst
)
{
    if (currentTopFirst.size() != targetTopFirst.size() ||
        currentTopFirst.size() > MAX_MOVE_WINDOW) {
        return std::nullopt;
    }

    std::unordered_map<uint64_t, size_t> initialPositions;
    for (size_t i = 0; i < currentTopFirst.size(); ++i) {
        if (!initialPositions.emplace(currentTopFirst[i], i).second) {
            return std::nullopt;
        }
    }

    std::vector<size_t> targetPermutation;
    targetPermutation.reserve(targetTopFirst.size());
    std::unordered_set<uint64_t> targetSeen;
    for (uint64_t slot : targetTopFirst) {
        const auto position = initialPositions.find(slot);
        if (position == initialPositions.end() ||
            !targetSeen.insert(slot).second) {
            return std::nullopt;
        }
        targetPermutation.push_back(position->second);
    }
    return planMoveOnly(targetPermutation);
}

std::optional<StackPermutationPlan>
StackPermutationPlanner::planCopyAssignment(
    size_t sourceDepth,
    size_t targetDepth,
    size_t strictByteLimit
)
{
    if (sourceDepth == targetDepth || sourceDepth > MAX_COPY_DEPTH ||
        targetDepth > MAX_COPY_DEPTH || strictByteLimit == 0) {
        return std::nullopt;
    }

    const size_t legacyCost =
        legacyCopyAssignmentByteCost(sourceDepth, targetDepth);
    const auto& plan =
        getCachedCopyAssignmentPlan(sourceDepth, targetDepth, legacyCost);
    if (!plan.has_value() || plan->serializedBytes >= strictByteLimit) {
        return std::nullopt;
    }
    return plan;
}

size_t StackPermutationPlanner::legacyCopyAssignmentByteCost(
    size_t sourceDepth,
    size_t targetDepth
)
{
    if (sourceDepth == targetDepth) {
        return std::numeric_limits<size_t>::max();
    }
    if (targetDepth < sourceDepth) {
        return 2 * targetDepth + 1 +
               pickByteCost(sourceDepth - targetDepth - 1);
    }
    if (sourceDepth == 0 && targetDepth == 1) {
        return 2;
    }
    return pickByteCost(sourceDepth) + 3 * targetDepth + 3;
}

} // namespace tbc
