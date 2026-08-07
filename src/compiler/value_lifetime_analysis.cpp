#include "value_lifetime_analysis.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <tuple>
#include <utility>

namespace apc::compiler
{
namespace
{

const std::set<ValueId> kEmptyValueSet;
const AtomSet kEmptyAtomSet;

std::string idText(uint64_t value)
{
    return std::to_string(value);
}

bool intersects(const AtomSet& lhs, const AtomSet& rhs)
{
    auto left = lhs.begin();
    auto right = rhs.begin();
    while (left != lhs.end() && right != rhs.end()) {
        if (*left == *right) {
            return true;
        }
        if (*left < *right) {
            ++left;
        } else {
            ++right;
        }
    }
    return false;
}

AtomSet atomsForValues(
    const LiveValueSet& liveValues,
    const StableValueTable& values
)
{
    AtomSet atoms;
    for (ValueId value : liveValues) {
        const auto& valueAtoms = values.atomsOf(value);
        atoms.insert(valueAtoms.begin(), valueAtoms.end());
    }
    return atoms;
}

std::vector<ValueId> valuesForReachingAtom(
    const LiveValueSet& reachingValues,
    AtomId atom,
    const StableValueTable& values
)
{
    std::vector<ValueId> result;
    for (ValueId value : reachingValues) {
        if (values.atomsOf(value).contains(atom)) {
            result.push_back(value);
        }
    }
    return result;
}

LiveValueSet transferLivenessStatement(
    const LifetimeStatement& statement,
    LiveValueSet liveAfter,
    const StableValueTable& values
)
{
    std::set<ValueId> definitions;
    for (const auto& definition : statement.definitions) {
        definitions.insert(definition.value);
        liveAfter.erase(definition.value);
    }

    // An alias/merge definition reads its relation sources.  Co-definitions in
    // one statement are treated as an atomic producer and need no live-in.
    for (const auto& definition : statement.definitions) {
        for (ValueId source : values.relationSources(definition.value)) {
            if (!definitions.contains(source)) {
                liveAfter.insert(source);
            }
        }
    }
    for (const auto& use : statement.uses) {
        liveAfter.insert(use.value);
    }
    return liveAfter;
}

LiveValueSet transferLivenessBlock(
    const LifetimeBasicBlock& block,
    LiveValueSet liveOut,
    const StableValueTable& values
)
{
    for (auto it = block.statements.rbegin(); it != block.statements.rend();
         ++it) {
        liveOut =
            transferLivenessStatement(*it, std::move(liveOut), values);
    }
    return liveOut;
}

LiveValueSet transferReachingStatement(
    const LifetimeStatement& statement,
    LiveValueSet reachingBefore,
    const StableValueTable& values
)
{
    AtomSet consumedAtoms;
    for (const auto& use : statement.uses) {
        if (use.consumes()) {
            const auto& useAtoms = values.atomsOf(use.value);
            consumedAtoms.insert(useAtoms.begin(), useAtoms.end());
        }
    }

    if (!consumedAtoms.empty()) {
        std::erase_if(reachingBefore, [&](ValueId value) {
            return intersects(values.atomsOf(value), consumedAtoms);
        });
    }
    for (const auto& definition : statement.definitions) {
        reachingBefore.insert(definition.value);
    }
    return reachingBefore;
}

LiveValueSet transferReachingBlock(
    const LifetimeBasicBlock& block,
    LiveValueSet reachingIn,
    const StableValueTable& values
)
{
    for (const auto& statement : block.statements) {
        reachingIn =
            transferReachingStatement(statement, std::move(reachingIn), values);
    }
    return reachingIn;
}

LiveValueSet intersectPredecessors(
    const std::vector<size_t>& predecessors,
    const std::vector<LiveValueSet>& out,
    const LiveValueSet& universe
)
{
    if (predecessors.empty()) {
        return {};
    }
    LiveValueSet result = universe;
    for (size_t predecessor : predecessors) {
        LiveValueSet intersection;
        std::set_intersection(
            result.begin(),
            result.end(),
            out[predecessor].begin(),
            out[predecessor].end(),
            std::inserter(intersection, intersection.end())
        );
        result = std::move(intersection);
    }
    return result;
}

struct DefinitionLocation
{
    BasicBlockId block;
    size_t statement{0};
};

struct StatementAtomFacts
{
    bool hasDefinition{false};
    bool hasUse{false};
    size_t consumeCount{0};
    std::optional<ValueId> representative;
};

using ConsumeSite = std::tuple<BasicBlockId, size_t, AtomId>;

} // namespace

std::optional<ValueRegistryError>
StableValueTable::registerIdentity(
    ValueIdentity identity,
    bool allowSharedAtoms
)
{
    if (!identity.binding.valid()) {
        return ValueRegistryError{
            ValueRegistryErrorCode::InvalidBinding,
            "value identity has an invalid binding id"
        };
    }
    if (!identity.value.valid()) {
        return ValueRegistryError{
            ValueRegistryErrorCode::InvalidValue,
            "value identity has an invalid value id"
        };
    }
    if (m_values.contains(identity.value)) {
        return ValueRegistryError{
            ValueRegistryErrorCode::DuplicateValue,
            "value id " + idText(identity.value.value()) +
                " is already registered"
        };
    }
    for (const auto& leaf : identity.leaves) {
        if (!leaf.atom.valid()) {
            return ValueRegistryError{
                ValueRegistryErrorCode::InvalidLeaf,
                "value identity contains an invalid leaf atom"
            };
        }
    }

    const AtomSet atoms = identity.atoms();
    if (atoms.empty()) {
        return ValueRegistryError{
            identity.leaves.empty() ? ValueRegistryErrorCode::InvalidAtom
                                    : ValueRegistryErrorCode::EmptyAtomSet,
            "value identity has no valid storage atom"
        };
    }
    if (!allowSharedAtoms) {
        for (AtomId atom : atoms) {
            const auto existing = m_atomValues.find(atom);
            if (existing != m_atomValues.end() && !existing->second.empty()) {
                return ValueRegistryError{
                    ValueRegistryErrorCode::AtomAlreadyRegistered,
                    "atom " + idText(atom.value()) +
                        " is already owned by another value; use an explicit "
                        "alias or atom relation"
                };
            }
        }
    }

    const ValueId value = identity.value;
    m_values.emplace(value, std::move(identity));
    m_valueAtoms.emplace(value, atoms);
    for (AtomId atom : atoms) {
        m_atomValues[atom].insert(value);
    }
    return std::nullopt;
}

std::optional<ValueRegistryError>
StableValueTable::registerValue(const ValueIdentity& identity)
{
    return registerIdentity(identity, false);
}

std::optional<ValueRegistryError> StableValueTable::registerAggregate(
    BindingId binding,
    ValueId value,
    std::vector<AtomLeaf> leaves,
    ValueContext context
)
{
    return registerIdentity(
        ValueIdentity{
            binding, value, AtomId{}, context, std::move(leaves)
        },
        false
    );
}

std::optional<ValueRegistryError> StableValueTable::registerAlias(
    BindingId binding,
    ValueId aliasValue,
    ValueId sourceValue,
    ValueContext context
)
{
    const ValueIdentity* source = find(sourceValue);
    if (!source) {
        return ValueRegistryError{
            ValueRegistryErrorCode::UnknownAliasSource,
            "alias source value " + idText(sourceValue.value()) +
                " is not registered"
        };
    }

    auto error = registerIdentity(
        ValueIdentity{
            binding, aliasValue, source->atom, context, source->leaves
        },
        true
    );
    if (!error.has_value()) {
        m_relationSources.emplace(
            aliasValue, std::set<ValueId>{sourceValue}
        );
    }
    return error;
}

std::optional<ValueRegistryError> StableValueTable::registerAtomRelation(
    BindingId binding,
    ValueId relatedValue,
    const std::vector<ValueId>& sourceValues,
    ValueContext context
)
{
    if (sourceValues.empty()) {
        return ValueRegistryError{
            ValueRegistryErrorCode::EmptyAtomRelation,
            "an atom relation needs at least one source value"
        };
    }

    std::set<ValueId> sources;
    AtomSet atoms;
    std::vector<AtomLeaf> leaves;
    for (ValueId sourceValue : sourceValues) {
        const ValueIdentity* source = find(sourceValue);
        if (!source) {
            return ValueRegistryError{
                ValueRegistryErrorCode::UnknownAliasSource,
                "atom relation source value " + idText(sourceValue.value()) +
                    " is not registered"
            };
        }
        sources.insert(sourceValue);
        const auto& sourceAtoms = atomsOf(sourceValue);
        atoms.insert(sourceAtoms.begin(), sourceAtoms.end());
        if (source->leaves.empty()) {
            for (AtomId atom : sourceAtoms) {
                leaves.push_back(AtomLeaf{{}, atom});
            }
        } else {
            leaves.insert(
                leaves.end(), source->leaves.begin(), source->leaves.end()
            );
        }
    }
    std::sort(leaves.begin(), leaves.end());
    leaves.erase(std::unique(leaves.begin(), leaves.end()), leaves.end());

    const AtomId scalarAtom =
        atoms.size() == 1 ? *atoms.begin() : AtomId{};
    if (atoms.size() == 1) {
        // Retain the source leaf paths, but avoid manufacturing a redundant
        // empty-path leaf for the common scalar case.
        const bool onlySyntheticScalar = leaves.size() == 1 &&
            leaves.front().path.empty() && leaves.front().atom == scalarAtom;
        if (onlySyntheticScalar) {
            leaves.clear();
        }
    }

    auto error = registerIdentity(
        ValueIdentity{
            binding, relatedValue, scalarAtom, context, std::move(leaves)
        },
        true
    );
    if (!error.has_value()) {
        m_relationSources.emplace(relatedValue, std::move(sources));
    }
    return error;
}

const ValueIdentity* StableValueTable::find(ValueId value) const
{
    const auto it = m_values.find(value);
    return it == m_values.end() ? nullptr : &it->second;
}

std::optional<AtomId> StableValueTable::atomOf(ValueId value) const
{
    const auto& atoms = atomsOf(value);
    if (atoms.size() != 1) {
        return std::nullopt;
    }
    return *atoms.begin();
}

const AtomSet& StableValueTable::atomsOf(ValueId value) const
{
    const auto it = m_valueAtoms.find(value);
    return it == m_valueAtoms.end() ? kEmptyAtomSet : it->second;
}

std::optional<ValueId> StableValueTable::aliasSource(ValueId value) const
{
    const auto& sources = relationSources(value);
    if (sources.size() != 1) {
        return std::nullopt;
    }
    return *sources.begin();
}

const std::set<ValueId>&
StableValueTable::relationSources(ValueId value) const
{
    const auto it = m_relationSources.find(value);
    return it == m_relationSources.end() ? kEmptyValueSet : it->second;
}

const std::set<ValueId>& StableValueTable::valuesForAtom(AtomId atom) const
{
    const auto it = m_atomValues.find(atom);
    return it == m_atomValues.end() ? kEmptyValueSet : it->second;
}

bool StableValueTable::sharesAtom(ValueId lhs, ValueId rhs) const
{
    return intersects(atomsOf(lhs), atomsOf(rhs));
}

const BlockLiveness*
LifetimeAnalysisResult::findBlock(BasicBlockId block) const
{
    const auto it = std::find_if(
        blocks.begin(), blocks.end(), [&](const BlockLiveness& candidate) {
            return candidate.block == block;
        }
    );
    return it == blocks.end() ? nullptr : &*it;
}

const EdgeLiveness* LifetimeAnalysisResult::findEdge(
    BasicBlockId from,
    BasicBlockId to
) const
{
    const auto it = std::find_if(
        edges.begin(), edges.end(), [&](const EdgeLiveness& candidate) {
            return candidate.boundary == EdgeBoundary{from, to};
        }
    );
    return it == edges.end() ? nullptr : &*it;
}

LifetimeAnalysisResult ValueLifetimeAnalyzer::analyze(
    const LifetimeControlFlowGraph& cfg,
    const StableValueTable& values
)
{
    LifetimeAnalysisResult result;

    std::map<BasicBlockId, size_t> blockIndices;
    for (size_t index = 0; index < cfg.blocks.size(); ++index) {
        const BasicBlockId block = cfg.blocks[index].id;
        if (!block.valid() || blockIndices.contains(block)) {
            result.issues.push_back(
                {LifetimeIssueCode::DuplicateBlock,
                 block,
                 0,
                 std::nullopt,
                 "CFG contains an invalid or duplicate block id"}
            );
            continue;
        }
        blockIndices.emplace(block, index);
    }

    if (!cfg.entry.valid() || !blockIndices.contains(cfg.entry)) {
        result.issues.push_back(
            {LifetimeIssueCode::MissingEntry,
             cfg.entry,
             0,
             std::nullopt,
             "CFG entry block is missing"}
        );
    }

    for (const auto& block : cfg.blocks) {
        for (BasicBlockId successor : block.successors) {
            if (!blockIndices.contains(successor)) {
                result.issues.push_back(
                    {LifetimeIssueCode::UnknownSuccessor,
                     block.id,
                     block.statements.size(),
                     std::nullopt,
                     "block references unknown successor " +
                         idText(successor.value())}
                );
            }
        }
        for (size_t statementIndex = 0;
             statementIndex < block.statements.size(); ++statementIndex) {
            const auto& statement = block.statements[statementIndex];
            for (const auto& definition : statement.definitions) {
                if (!values.find(definition.value)) {
                    result.issues.push_back(
                        {LifetimeIssueCode::UnknownValue,
                         block.id,
                         statementIndex,
                         definition.value,
                         "definition references unknown value " +
                             idText(definition.value.value())}
                    );
                }
            }
            for (const auto& use : statement.uses) {
                if (!values.find(use.value)) {
                    result.issues.push_back(
                        {LifetimeIssueCode::UnknownValue,
                         block.id,
                         statementIndex,
                         use.value,
                         "use references unknown value " +
                             idText(use.value.value())}
                    );
                }
            }
        }
    }

    std::set<ValueId> parameters;
    for (ValueId parameter : cfg.parameters) {
        if (!values.find(parameter)) {
            result.issues.push_back(
                {LifetimeIssueCode::UnknownValue,
                 cfg.entry,
                 0,
                 parameter,
                 "parameter references unknown value " +
                     idText(parameter.value())}
            );
        } else if (!parameters.insert(parameter).second) {
            result.issues.push_back(
                {LifetimeIssueCode::DuplicateParameter,
                 cfg.entry,
                 0,
                 parameter,
                 "parameter value is listed more than once"}
            );
        }
    }

    if (!result.issues.empty()) {
        return result;
    }

    std::vector<bool> reachable(cfg.blocks.size(), false);
    std::deque<size_t> pending;
    pending.push_back(blockIndices.at(cfg.entry));
    while (!pending.empty()) {
        const size_t index = pending.front();
        pending.pop_front();
        if (reachable[index]) {
            continue;
        }
        reachable[index] = true;
        for (BasicBlockId successor : cfg.blocks[index].successors) {
            pending.push_back(blockIndices.at(successor));
        }
    }

    std::map<ValueId, std::vector<DefinitionLocation>> definitions;
    std::set<ValueId> referencedValues;
    for (size_t blockIndex = 0; blockIndex < cfg.blocks.size(); ++blockIndex) {
        const auto& block = cfg.blocks[blockIndex];
        for (size_t statementIndex = 0;
             statementIndex < block.statements.size(); ++statementIndex) {
            const auto& statement = block.statements[statementIndex];
            for (const auto& definition : statement.definitions) {
                definitions[definition.value].push_back(
                    {block.id, statementIndex}
                );
                for (ValueId source :
                     values.relationSources(definition.value)) {
                    referencedValues.insert(source);
                }
            }
            for (const auto& use : statement.uses) {
                referencedValues.insert(use.value);
            }
        }
    }

    for (const auto& [value, locations] : definitions) {
        if (parameters.contains(value)) {
            result.issues.push_back(
                {LifetimeIssueCode::ParameterDefined,
                 locations.front().block,
                 locations.front().statement,
                 value,
                 "a parameter/live-in value cannot also be defined in the CFG"}
            );
        }
        if (locations.size() > 1) {
            result.issues.push_back(
                {LifetimeIssueCode::DuplicateDefinition,
                 locations[1].block,
                 locations[1].statement,
                 value,
                 "a stable value must have exactly one definition"}
            );
        }
    }
    for (ValueId referenced : referencedValues) {
        if (!parameters.contains(referenced) &&
            !definitions.contains(referenced)) {
            result.issues.push_back(
                {LifetimeIssueCode::MissingDefinition,
                 cfg.entry,
                 0,
                 referenced,
                 "used value is neither a parameter nor defined in the CFG"}
            );
        }
    }
    if (!result.issues.empty()) {
        return result;
    }

    std::vector<std::vector<size_t>> predecessors(cfg.blocks.size());
    for (size_t blockIndex = 0; blockIndex < cfg.blocks.size(); ++blockIndex) {
        if (!reachable[blockIndex]) {
            continue;
        }
        for (BasicBlockId successor : cfg.blocks[blockIndex].successors) {
            const size_t successorIndex = blockIndices.at(successor);
            if (reachable[successorIndex]) {
                predecessors[successorIndex].push_back(blockIndex);
            }
        }
    }

    // Dominance provides an exact unique-definition/use-before-definition
    // check, including loops and joins, without relying on block vector order.
    std::set<BasicBlockId> allReachableBlocks;
    for (size_t index = 0; index < cfg.blocks.size(); ++index) {
        if (reachable[index]) {
            allReachableBlocks.insert(cfg.blocks[index].id);
        }
    }
    std::vector<std::set<BasicBlockId>> dominators(cfg.blocks.size());
    const size_t entryIndex = blockIndices.at(cfg.entry);
    for (size_t index = 0; index < cfg.blocks.size(); ++index) {
        if (!reachable[index]) {
            continue;
        }
        dominators[index] = index == entryIndex
            ? std::set<BasicBlockId>{cfg.entry}
            : allReachableBlocks;
    }
    bool dominatorsChanged = true;
    while (dominatorsChanged) {
        dominatorsChanged = false;
        for (size_t index = 0; index < cfg.blocks.size(); ++index) {
            if (!reachable[index] || index == entryIndex) {
                continue;
            }
            std::set<BasicBlockId> next = allReachableBlocks;
            for (size_t predecessor : predecessors[index]) {
                std::set<BasicBlockId> intersection;
                std::set_intersection(
                    next.begin(),
                    next.end(),
                    dominators[predecessor].begin(),
                    dominators[predecessor].end(),
                    std::inserter(intersection, intersection.end())
                );
                next = std::move(intersection);
            }
            next.insert(cfg.blocks[index].id);
            if (next != dominators[index]) {
                dominators[index] = std::move(next);
                dominatorsChanged = true;
            }
        }
    }

    auto isAvailableBefore = [&](ValueId value,
                                 size_t useBlockIndex,
                                 size_t useStatement) {
        if (parameters.contains(value)) {
            return true;
        }
        const auto locationIt = definitions.find(value);
        if (locationIt == definitions.end() ||
            locationIt->second.size() != 1) {
            return false;
        }
        const auto& definition = locationIt->second.front();
        if (definition.block == cfg.blocks[useBlockIndex].id) {
            return definition.statement < useStatement;
        }
        return dominators[useBlockIndex].contains(definition.block);
    };

    for (size_t blockIndex = 0; blockIndex < cfg.blocks.size(); ++blockIndex) {
        if (!reachable[blockIndex]) {
            continue;
        }
        const auto& block = cfg.blocks[blockIndex];
        for (size_t statementIndex = 0;
             statementIndex < block.statements.size(); ++statementIndex) {
            const auto& statement = block.statements[statementIndex];
            std::set<ValueId> statementDefinitions;
            for (const auto& definition : statement.definitions) {
                statementDefinitions.insert(definition.value);
            }
            for (const auto& use : statement.uses) {
                if (!isAvailableBefore(
                        use.value, blockIndex, statementIndex
                    )) {
                    result.issues.push_back(
                        {LifetimeIssueCode::UseBeforeDefinition,
                         block.id,
                         statementIndex,
                         use.value,
                         "value use is not dominated by its definition"}
                    );
                }
            }
            for (const auto& definition : statement.definitions) {
                for (ValueId source :
                     values.relationSources(definition.value)) {
                    if (!statementDefinitions.contains(source) &&
                        !isAvailableBefore(
                            source, blockIndex, statementIndex
                        )) {
                        result.issues.push_back(
                            {LifetimeIssueCode::AliasSourceUnavailable,
                             block.id,
                             statementIndex,
                             source,
                             "atom relation source is not available at its "
                             "definition"}
                        );
                    }
                }
            }
        }
    }
    if (!result.issues.empty()) {
        return result;
    }

    std::set<ConsumeSite> unsafeConsumes;
    for (size_t blockIndex = 0; blockIndex < cfg.blocks.size(); ++blockIndex) {
        if (!reachable[blockIndex]) {
            continue;
        }
        const auto& block = cfg.blocks[blockIndex];
        for (size_t statementIndex = 0;
             statementIndex < block.statements.size(); ++statementIndex) {
            const auto& statement = block.statements[statementIndex];
            std::map<AtomId, size_t> consumeCounts;
            AtomSet definitionAtoms;
            for (const auto& definition : statement.definitions) {
                const auto& atoms = values.atomsOf(definition.value);
                definitionAtoms.insert(atoms.begin(), atoms.end());
            }
            for (const auto& use : statement.uses) {
                if (!use.consumes()) {
                    continue;
                }
                for (AtomId atom : values.atomsOf(use.value)) {
                    ++consumeCounts[atom];
                }
            }
            for (const auto& [atom, count] : consumeCounts) {
                const ConsumeSite site{block.id, statementIndex, atom};
                if (count > 1) {
                    unsafeConsumes.insert(site);
                    result.issues.push_back(
                        {LifetimeIssueCode::MultipleConsume,
                         block.id,
                         statementIndex,
                         std::nullopt,
                         "one physical atom is consumed multiple times in a "
                         "statement"}
                    );
                }
                if (definitionAtoms.contains(atom)) {
                    unsafeConsumes.insert(site);
                    result.issues.push_back(
                        {LifetimeIssueCode::ConsumeWithDefinition,
                         block.id,
                         statementIndex,
                         std::nullopt,
                         "a statement cannot consume and define the same atom; "
                         "model a transfer with Relocate"}
                    );
                }
            }
        }
    }

    // Escapes are intentionally function-wide and flow-insensitive.  Dynamic
    // indexing escapes every leaf/may-alias atom of the value.
    for (size_t index = 0; index < cfg.blocks.size(); ++index) {
        if (!reachable[index]) {
            continue;
        }
        for (const auto& statement : cfg.blocks[index].statements) {
            for (const auto& use : statement.uses) {
                if (!use.causesEscape()) {
                    continue;
                }
                const auto& useAtoms = values.atomsOf(use.value);
                result.escapedAtoms.insert(useAtoms.begin(), useAtoms.end());
            }
        }
    }

    std::vector<LiveValueSet> liveIn(cfg.blocks.size());
    std::vector<LiveValueSet> liveOut(cfg.blocks.size());
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t reverse = cfg.blocks.size(); reverse > 0; --reverse) {
            const size_t index = reverse - 1;
            if (!reachable[index]) {
                continue;
            }

            LiveValueSet nextOut;
            for (BasicBlockId successor : cfg.blocks[index].successors) {
                const auto& successorIn = liveIn[blockIndices.at(successor)];
                nextOut.insert(successorIn.begin(), successorIn.end());
            }
            LiveValueSet nextIn = transferLivenessBlock(
                cfg.blocks[index], nextOut, values
            );
            if (nextOut != liveOut[index] || nextIn != liveIn[index]) {
                liveOut[index] = std::move(nextOut);
                liveIn[index] = std::move(nextIn);
                changed = true;
            }
        }
    }

    result.blocks.reserve(cfg.blocks.size());
    for (size_t index = 0; index < cfg.blocks.size(); ++index) {
        BlockLiveness blockResult;
        blockResult.block = cfg.blocks[index].id;
        blockResult.liveIn = liveIn[index];
        blockResult.liveOut = liveOut[index];
        blockResult.liveAtomsIn = atomsForValues(liveIn[index], values);
        blockResult.liveAtomsOut = atomsForValues(liveOut[index], values);
        blockResult.statements.resize(cfg.blocks[index].statements.size());

        if (reachable[index]) {
            LiveValueSet live = liveOut[index];
            for (size_t reverse = cfg.blocks[index].statements.size();
                 reverse > 0; --reverse) {
                const size_t statementIndex = reverse - 1;
                auto& statementResult =
                    blockResult.statements[statementIndex];
                statementResult.liveAfter = live;
                statementResult.liveAtomsAfter =
                    atomsForValues(live, values);
                live = transferLivenessStatement(
                    cfg.blocks[index].statements[statementIndex],
                    std::move(live),
                    values
                );
                statementResult.liveBefore = live;
                statementResult.liveAtomsBefore =
                    atomsForValues(live, values);
            }
        }
        result.blocks.push_back(std::move(blockResult));
    }

    // Definite reaching values are separate from liveness: they prevent a kill
    // from retiring aliases that are registered globally but have not reached
    // the current program point.
    LiveValueSet reachingUniverse = parameters;
    for (const auto& [value, locations] : definitions) {
        if (!locations.empty()) {
            reachingUniverse.insert(value);
        }
    }
    std::vector<LiveValueSet> reachingIn(cfg.blocks.size());
    std::vector<LiveValueSet> reachingOut(cfg.blocks.size());
    for (size_t index = 0; index < cfg.blocks.size(); ++index) {
        if (!reachable[index]) {
            continue;
        }
        reachingIn[index] = index == entryIndex ? parameters
                                                 : reachingUniverse;
        reachingOut[index] = transferReachingBlock(
            cfg.blocks[index], reachingIn[index], values
        );
    }
    bool reachingChanged = true;
    while (reachingChanged) {
        reachingChanged = false;
        for (size_t index = 0; index < cfg.blocks.size(); ++index) {
            if (!reachable[index]) {
                continue;
            }
            LiveValueSet nextIn = index == entryIndex
                ? parameters
                : intersectPredecessors(
                      predecessors[index], reachingOut, reachingUniverse
                  );
            LiveValueSet nextOut = transferReachingBlock(
                cfg.blocks[index], nextIn, values
            );
            if (nextIn != reachingIn[index] ||
                nextOut != reachingOut[index]) {
                reachingIn[index] = std::move(nextIn);
                reachingOut[index] = std::move(nextOut);
                reachingChanged = true;
            }
        }
    }

    std::vector<std::vector<LiveValueSet>> reachingBefore(cfg.blocks.size());
    std::vector<std::vector<LiveValueSet>> reachingAfter(cfg.blocks.size());
    for (size_t blockIndex = 0; blockIndex < cfg.blocks.size(); ++blockIndex) {
        if (!reachable[blockIndex]) {
            continue;
        }
        reachingBefore[blockIndex].resize(
            cfg.blocks[blockIndex].statements.size()
        );
        reachingAfter[blockIndex].resize(
            cfg.blocks[blockIndex].statements.size()
        );
        LiveValueSet reaching = reachingIn[blockIndex];
        for (size_t statementIndex = 0;
             statementIndex < cfg.blocks[blockIndex].statements.size();
             ++statementIndex) {
            reachingBefore[blockIndex][statementIndex] = reaching;
            reaching = transferReachingStatement(
                cfg.blocks[blockIndex].statements[statementIndex],
                std::move(reaching),
                values
            );
            reachingAfter[blockIndex][statementIndex] = reaching;
        }
    }

    for (size_t blockIndex = 0; blockIndex < cfg.blocks.size(); ++blockIndex) {
        if (!reachable[blockIndex]) {
            continue;
        }
        const auto& block = cfg.blocks[blockIndex];
        const auto& blockResult = result.blocks[blockIndex];
        for (size_t statementIndex = 0;
             statementIndex < block.statements.size(); ++statementIndex) {
            const auto& statement = block.statements[statementIndex];
            const auto& statementLiveness =
                blockResult.statements[statementIndex];
            std::map<AtomId, StatementAtomFacts> facts;

            for (const auto& definition : statement.definitions) {
                for (AtomId atom : values.atomsOf(definition.value)) {
                    auto& atomFacts = facts[atom];
                    atomFacts.hasDefinition = true;
                    atomFacts.representative = definition.value;
                }
            }
            for (const auto& use : statement.uses) {
                for (AtomId atom : values.atomsOf(use.value)) {
                    auto& atomFacts = facts[atom];
                    atomFacts.hasUse = true;
                    if (use.consumes()) {
                        ++atomFacts.consumeCount;
                    }
                    atomFacts.representative = use.value;
                }
            }

            for (const auto& [atom, atomFacts] : facts) {
                const bool hasConsume = atomFacts.consumeCount != 0;
                const ConsumeSite site{block.id, statementIndex, atom};
                bool unsafeConsume = unsafeConsumes.contains(site);
                const bool remainsLive =
                    statementLiveness.liveAtomsAfter.contains(atom);

                if (hasConsume && result.escapedAtoms.contains(atom)) {
                    unsafeConsume = true;
                    result.issues.push_back(
                        {LifetimeIssueCode::ConsumedEscapedAtom,
                         block.id,
                         statementIndex,
                         atomFacts.representative,
                         "an escaped atom cannot be proven safe to consume"}
                    );
                }
                if (hasConsume && remainsLive) {
                    unsafeConsume = true;
                    result.issues.push_back(
                        {LifetimeIssueCode::ConsumedAtomStillLive,
                         block.id,
                         statementIndex,
                         atomFacts.representative,
                         "a consumed atom still has a may-live alias"}
                    );
                }
                if (unsafeConsume || remainsLive ||
                    result.escapedAtoms.contains(atom)) {
                    continue;
                }

                const auto& reaching = hasConsume
                    ? reachingBefore[blockIndex][statementIndex]
                    : reachingAfter[blockIndex][statementIndex];
                std::vector<ValueId> retired =
                    valuesForReachingAtom(reaching, atom, values);
                if (retired.empty()) {
                    continue;
                }

                KillSuggestion suggestion;
                suggestion.boundary = {block.id, statementIndex + 1};
                suggestion.atom = atom;
                suggestion.retiredValues = std::move(retired);
                if (hasConsume) {
                    suggestion.reason = KillReason::Consumed;
                    suggestion.requiresCleanup = false;
                } else if (atomFacts.hasUse) {
                    suggestion.reason = KillReason::LastUse;
                } else if (atomFacts.hasDefinition) {
                    suggestion.reason = KillReason::DeadDefinition;
                }
                result.kills.push_back(std::move(suggestion));
            }
        }
    }

    // The block live-out is a union.  Materialize each successor's live-in so
    // a planner may clean storage only on the edge where it is dead.
    for (size_t blockIndex = 0; blockIndex < cfg.blocks.size(); ++blockIndex) {
        if (!reachable[blockIndex]) {
            continue;
        }
        const auto& block = cfg.blocks[blockIndex];
        const auto& blockLive = result.blocks[blockIndex];
        const AtomSet reachingAtoms =
            atomsForValues(reachingOut[blockIndex], values);
        for (BasicBlockId successor : block.successors) {
            const size_t successorIndex = blockIndices.at(successor);
            EdgeLiveness edge;
            edge.boundary = {block.id, successor};
            edge.liveValues = liveIn[successorIndex];
            edge.liveAtoms = atomsForValues(edge.liveValues, values);
            result.edges.push_back(edge);

            for (AtomId atom : blockLive.liveAtomsOut) {
                if (edge.liveAtoms.contains(atom) ||
                    !reachingAtoms.contains(atom) ||
                    result.escapedAtoms.contains(atom)) {
                    continue;
                }
                std::vector<ValueId> retired = valuesForReachingAtom(
                    reachingOut[blockIndex], atom, values
                );
                if (retired.empty()) {
                    continue;
                }
                result.edgeKills.push_back(
                    {{block.id, successor},
                     atom,
                     std::move(retired),
                     KillReason::EdgeExit,
                     true}
                );
            }
        }
    }

    return result;
}

} // namespace apc::compiler
