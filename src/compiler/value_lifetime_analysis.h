#ifndef VALUE_LIFETIME_ANALYSIS_H
#define VALUE_LIFETIME_ANALYSIS_H

#include "stable_value_identity.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace apc::compiler
{

// Access and ownership are deliberately orthogonal.  In particular, Delete
// needs an lvalue access and a consuming ownership effect at the same time.
enum class AccessKind {
    Borrow,
    LValue
};

enum class OwnershipEffect {
    None,
    Consume,
    Relocate,
    Escape
};

// Source compatibility for callers that still describe a use with one enum.
// New code should use AccessKind + OwnershipEffect.
enum class UseKind {
    Consume,
    Borrow,
    Relocate,
    Escape,
    LValue
};

enum class EscapeKind {
    None,
    Explicit,
    DynamicIndex,
    AltStack
};

struct ValueUse
{
    ValueId value;
    AccessKind access{AccessKind::Borrow};
    OwnershipEffect ownership{OwnershipEffect::None};
    EscapeKind escapeKind{EscapeKind::None};

    constexpr ValueUse() noexcept = default;

    constexpr ValueUse(
        ValueId usedValue,
        AccessKind accessKind,
        OwnershipEffect ownershipEffect = OwnershipEffect::None,
        EscapeKind escape = EscapeKind::None
    ) noexcept
        : value(usedValue),
          access(accessKind),
          ownership(ownershipEffect),
          escapeKind(escape)
    {}

    constexpr ValueUse(
        ValueId usedValue,
        UseKind legacyKind,
        EscapeKind escape = EscapeKind::None
    ) noexcept
        : value(usedValue), escapeKind(escape)
    {
        switch (legacyKind) {
        case UseKind::Consume:
            ownership = OwnershipEffect::Consume;
            break;
        case UseKind::Borrow:
            break;
        case UseKind::Relocate:
            ownership = OwnershipEffect::Relocate;
            break;
        case UseKind::Escape:
            ownership = OwnershipEffect::Escape;
            break;
        case UseKind::LValue:
            access = AccessKind::LValue;
            break;
        }
    }

    constexpr UseKind legacyKind() const noexcept
    {
        if (ownership == OwnershipEffect::Consume) {
            return UseKind::Consume;
        }
        if (ownership == OwnershipEffect::Relocate) {
            return UseKind::Relocate;
        }
        if (ownership == OwnershipEffect::Escape) {
            return UseKind::Escape;
        }
        return access == AccessKind::LValue ? UseKind::LValue
                                            : UseKind::Borrow;
    }

    constexpr bool causesEscape() const noexcept
    {
        return ownership == OwnershipEffect::Escape ||
            escapeKind != EscapeKind::None;
    }

    constexpr bool consumes() const noexcept
    {
        return ownership == OwnershipEffect::Consume;
    }
};

struct ValueDefinition
{
    ValueId value;
};

struct LifetimeStatement
{
    std::vector<ValueDefinition> definitions;
    std::vector<ValueUse> uses;
};

struct LifetimeBasicBlock
{
    BasicBlockId id;
    std::vector<LifetimeStatement> statements;
    std::vector<BasicBlockId> successors;
};

struct LifetimeControlFlowGraph
{
    BasicBlockId entry;
    std::vector<LifetimeBasicBlock> blocks;
    // Values available on entry (function parameters, captures, or explicitly
    // modelled incoming stack values).  Every other used value needs one unique
    // reachable definition.
    std::vector<ValueId> parameters;

    LifetimeControlFlowGraph() = default;

    LifetimeControlFlowGraph(
        BasicBlockId entryBlock,
        std::vector<LifetimeBasicBlock> basicBlocks,
        std::vector<ValueId> incomingValues = {}
    )
        : entry(entryBlock),
          blocks(std::move(basicBlocks)),
          parameters(std::move(incomingValues))
    {}
};

enum class ValueRegistryErrorCode {
    InvalidBinding,
    InvalidValue,
    InvalidAtom,
    InvalidLeaf,
    EmptyAtomSet,
    DuplicateValue,
    AtomAlreadyRegistered,
    UnknownAliasSource,
    EmptyAtomRelation
};

struct ValueRegistryError
{
    ValueRegistryErrorCode code{ValueRegistryErrorCode::InvalidValue};
    std::string message;
};

// Values are logical definitions while atoms are physical storage identities.
// registerValue/registerAggregate create fresh storage: accidental Atom reuse
// is rejected.  Sharing is only possible through an explicit relation API.
class StableValueTable
{
public:
    std::optional<ValueRegistryError>
    registerValue(const ValueIdentity& identity);

    std::optional<ValueRegistryError> registerAggregate(
        BindingId binding,
        ValueId value,
        std::vector<AtomLeaf> leaves,
        ValueContext context = {}
    );

    std::optional<ValueRegistryError> registerAlias(
        BindingId binding,
        ValueId aliasValue,
        ValueId sourceValue,
        ValueContext context = {}
    );

    std::optional<ValueRegistryError> registerAtomRelation(
        BindingId binding,
        ValueId relatedValue,
        const std::vector<ValueId>& sourceValues,
        ValueContext context = {}
    );

    std::optional<ValueRegistryError> registerMerge(
        BindingId binding,
        ValueId mergedValue,
        const std::vector<ValueId>& sourceValues,
        ValueContext context = {}
    )
    {
        return registerAtomRelation(
            binding, mergedValue, sourceValues, context
        );
    }

    const ValueIdentity* find(ValueId value) const;
    std::optional<AtomId> atomOf(ValueId value) const;
    const AtomSet& atomsOf(ValueId value) const;
    std::optional<ValueId> aliasSource(ValueId value) const;
    const std::set<ValueId>& relationSources(ValueId value) const;
    const std::set<ValueId>& valuesForAtom(AtomId atom) const;
    bool sharesAtom(ValueId lhs, ValueId rhs) const;
    size_t size() const noexcept
    {
        return m_values.size();
    }

private:
    std::optional<ValueRegistryError>
    registerIdentity(ValueIdentity identity, bool allowSharedAtoms);

    std::map<ValueId, ValueIdentity> m_values;
    std::map<ValueId, AtomSet> m_valueAtoms;
    std::map<AtomId, std::set<ValueId>> m_atomValues;
    std::map<ValueId, std::set<ValueId>> m_relationSources;
};

using LiveValueSet = std::set<ValueId>;
using LiveAtomSet = AtomSet;

struct StatementLiveness
{
    LiveValueSet liveBefore;
    LiveValueSet liveAfter;
    LiveAtomSet liveAtomsBefore;
    LiveAtomSet liveAtomsAfter;
};

struct BlockLiveness
{
    BasicBlockId block;
    LiveValueSet liveIn;
    LiveValueSet liveOut;
    LiveAtomSet liveAtomsIn;
    LiveAtomSet liveAtomsOut;
    std::vector<StatementLiveness> statements;
};

struct EdgeBoundary
{
    BasicBlockId from;
    BasicBlockId to;

    friend auto operator<=>(const EdgeBoundary&, const EdgeBoundary&) = default;
};

struct EdgeLiveness
{
    EdgeBoundary boundary;
    LiveValueSet liveValues;
    LiveAtomSet liveAtoms;
};

struct StatementBoundary
{
    BasicBlockId block;
    // Number of statements completed in the block.  A value of 1 denotes the
    // boundary immediately after statements[0].
    size_t afterStatement{0};

    friend bool operator==(const StatementBoundary&, const StatementBoundary&) =
        default;
};

enum class KillReason {
    LastUse,
    DeadDefinition,
    Consumed,
    EdgeExit
};

struct EdgeKillSuggestion
{
    EdgeBoundary boundary;
    AtomId atom;
    // Only values definitely reaching this edge are reported.  Values merely
    // registered as aliases elsewhere are never globally retired here.
    std::vector<ValueId> retiredValues;
    KillReason reason{KillReason::EdgeExit};
    bool requiresCleanup{true};
};

struct KillSuggestion
{
    StatementBoundary boundary;
    AtomId atom;
    std::vector<ValueId> retiredValues;
    KillReason reason{KillReason::LastUse};

    // Consume already removes the physical atom.  Other last uses leave a
    // value behind and need an explicit cleanup/move decision downstream.
    bool requiresCleanup{true};
};

enum class LifetimeIssueCode {
    MissingEntry,
    DuplicateBlock,
    UnknownSuccessor,
    UnknownValue,
    DuplicateParameter,
    ParameterDefined,
    MissingDefinition,
    DuplicateDefinition,
    UseBeforeDefinition,
    AliasSourceUnavailable,
    MultipleConsume,
    ConsumeWithDefinition,
    ConsumedAtomStillLive,
    ConsumedEscapedAtom
};

struct LifetimeIssue
{
    LifetimeIssueCode code{LifetimeIssueCode::UnknownValue};
    BasicBlockId block;
    size_t statementIndex{0};
    std::optional<ValueId> value;
    std::string message;
};

struct LifetimeAnalysisResult
{
    std::vector<BlockLiveness> blocks;
    std::vector<EdgeLiveness> edges;
    std::vector<KillSuggestion> kills;
    std::vector<EdgeKillSuggestion> edgeKills;
    std::set<AtomId> escapedAtoms;
    std::vector<LifetimeIssue> issues;

    bool valid() const noexcept
    {
        return issues.empty();
    }

    const BlockLiveness* findBlock(BasicBlockId block) const;
    const EdgeLiveness*
    findEdge(BasicBlockId from, BasicBlockId to) const;
};

class ValueLifetimeAnalyzer final
{
public:
    static LifetimeAnalysisResult analyze(
        const LifetimeControlFlowGraph& cfg,
        const StableValueTable& values
    );
};

struct StrictBenefitDecision
{
    size_t baselineBytes{0};
    size_t candidateBytes{0};

    constexpr bool accepted() const noexcept
    {
        return candidateBytes < baselineBytes;
    }

    constexpr size_t savings() const noexcept
    {
        return accepted() ? baselineBytes - candidateBytes : 0;
    }
};

class StrictBenefitPolicy final
{
public:
    static constexpr StrictBenefitDecision
    evaluate(size_t baselineBytes, size_t candidateBytes) noexcept
    {
        return {baselineBytes, candidateBytes};
    }

    static constexpr bool
    accepts(size_t baselineBytes, size_t candidateBytes) noexcept
    {
        return candidateBytes < baselineBytes;
    }
};

} // namespace apc::compiler

#endif // VALUE_LIFETIME_ANALYSIS_H
