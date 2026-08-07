#include "compiler/value_lifetime_analysis.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace
{

using namespace apc::compiler;

static_assert(!std::is_convertible_v<BindingId, ValueId>);
static_assert(!std::is_convertible_v<ValueId, AtomId>);
static_assert(!std::is_constructible_v<CallSiteId, InlineFrameId>);

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "value_lifetime_analysis_test: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

void registerValue(
    StableValueTable& table,
    BindingId binding,
    ValueId value,
    AtomId atom,
    ValueContext context = {}
)
{
    const auto error = table.registerValue(
        ValueIdentity{binding, value, atom, context}
    );
    require(!error.has_value(), "value registration failed");
}

const KillSuggestion* findKill(
    const LifetimeAnalysisResult& result,
    BasicBlockId block,
    size_t afterStatement,
    AtomId atom
)
{
    for (const auto& kill : result.kills) {
        if (kill.boundary.block == block &&
            kill.boundary.afterStatement == afterStatement &&
            kill.atom == atom) {
            return &kill;
        }
    }
    return nullptr;
}

const EdgeKillSuggestion* findEdgeKill(
    const LifetimeAnalysisResult& result,
    BasicBlockId from,
    BasicBlockId to,
    AtomId atom
)
{
    for (const auto& kill : result.edgeKills) {
        if (kill.boundary == EdgeBoundary{from, to} && kill.atom == atom) {
            return &kill;
        }
    }
    return nullptr;
}

bool hasIssue(
    const LifetimeAnalysisResult& result,
    LifetimeIssueCode expected
)
{
    for (const auto& issue : result.issues) {
        if (issue.code == expected) {
            return true;
        }
    }
    return false;
}

void testStrongIdentityAndAliasTable()
{
    StableValueIdFactory ids;
    const BindingId sourceBinding = ids.nextBinding();
    const BindingId aliasBinding = ids.nextBinding();
    const ValueId sourceValue = ids.nextValue();
    const ValueId aliasValue = ids.nextValue();
    const AtomId atom = ids.nextAtom();
    const ValueContext context{
        ids.nextInlineFrame(), ids.nextCallSite(), ids.nextLoopIteration()
    };

    require(sourceBinding.valid() && sourceValue.valid() && atom.valid(),
        "factory returned invalid identifiers");
    require(sourceBinding.value() == 1 && sourceValue.value() == 1,
        "independent identifier domains should start deterministically");

    std::unordered_set<ValueId, StableIdHash<ValueId>> hashedValues;
    hashedValues.insert(sourceValue);
    require(hashedValues.contains(sourceValue), "stable id hash failed");

    StableValueTable table;
    registerValue(table, sourceBinding, sourceValue, atom, context);
    const auto aliasError = table.registerAlias(
        aliasBinding, aliasValue, sourceValue, context
    );
    require(!aliasError.has_value(), "alias registration failed");
    require(table.sharesAtom(sourceValue, aliasValue),
        "alias did not retain the source atom");
    require(table.aliasSource(aliasValue) == sourceValue,
        "alias source relationship was not retained");
    require(table.valuesForAtom(atom) ==
                std::set<ValueId>({sourceValue, aliasValue}),
        "atom alias set mismatch");
    require(table.find(aliasValue)->context == context,
        "alias lost inline/call/iteration context");

    const auto duplicate = table.registerValue(
        ValueIdentity{sourceBinding, sourceValue, ids.nextAtom(), {}}
    );
    require(duplicate.has_value() &&
                duplicate->code == ValueRegistryErrorCode::DuplicateValue,
        "duplicate value registration was accepted");
    const auto unknownAlias = table.registerAlias(
        aliasBinding, ids.nextValue(), ValueId{999}
    );
    require(unknownAlias.has_value() &&
                unknownAlias->code ==
                    ValueRegistryErrorCode::UnknownAliasSource,
        "unknown alias source was accepted");
}

void testFreshAtomsAggregatesAndExplicitRelations()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId scalar = ids.nextValue();
    const AtomId scalarAtom = ids.nextAtom();
    registerValue(table, ids.nextBinding(), scalar, scalarAtom);

    const ValueId accidentalAlias = ids.nextValue();
    const auto reusedAtom = table.registerValue(
        ValueIdentity{
            ids.nextBinding(), accidentalAlias, scalarAtom, {}
        }
    );
    require(reusedAtom.has_value() &&
                reusedAtom->code ==
                    ValueRegistryErrorCode::AtomAlreadyRegistered,
        "registerValue accepted accidental physical atom reuse");

    const ValueId explicitAlias = ids.nextValue();
    require(!table.registerAlias(
                       ids.nextBinding(), explicitAlias, scalar
                   )
                 .has_value(),
        "explicit alias did not permit atom sharing");

    const AtomId leftAtom = ids.nextAtom();
    const AtomId rightAtom = ids.nextAtom();
    const ValueId aggregate = ids.nextValue();
    const std::vector<AtomLeaf> leaves{
        {{0}, leftAtom}, {{1, 0}, rightAtom}
    };
    require(!table.registerAggregate(
                       ids.nextBinding(), aggregate, leaves
                   )
                 .has_value(),
        "fresh aggregate registration failed");
    require(table.atomsOf(aggregate) == AtomSet({leftAtom, rightAtom}),
        "aggregate atom set was not preserved");
    require(table.find(aggregate)->leaves == leaves,
        "aggregate leaf paths were not preserved");
    require(!table.atomOf(aggregate).has_value(),
        "multi-atom aggregate was exposed as a scalar atom");

    const ValueId merged = ids.nextValue();
    require(!table.registerMerge(
                       ids.nextBinding(), merged, {scalar, aggregate}
                   )
                 .has_value(),
        "explicit multi-source atom relation failed");
    require(table.atomsOf(merged) ==
                AtomSet({scalarAtom, leftAtom, rightAtom}),
        "merge did not conservatively union source atom sets");
    require(table.relationSources(merged) ==
                std::set<ValueId>({scalar, aggregate}),
        "merge source relationship was not preserved");
}

void testStraightLineKindsAndKillBoundaries()
{
    StableValueIdFactory ids;
    const BasicBlockId block = ids.nextBasicBlock();

    auto run = [&](UseKind kind) {
        StableValueTable table;
        const ValueId value = ids.nextValue();
        const AtomId atom = ids.nextAtom();
        registerValue(table, ids.nextBinding(), value, atom);

        LifetimeControlFlowGraph cfg{
            block,
            {{block,
              {{{ValueDefinition{value}}, {}},
               {{}, {ValueUse{value, kind, EscapeKind::None}}}},
              {}}}
        };
        const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
        require(result.valid(), "straight-line analysis failed");
        const auto* liveness = result.findBlock(block);
        require(liveness != nullptr, "missing straight-line block result");
        require(liveness->statements[0].liveAfter.contains(value),
            "definition was not live until its use");
        require(liveness->statements[1].liveBefore.contains(value) &&
                    liveness->statements[1].liveAfter.empty(),
            "last-use liveness mismatch");

        const auto* kill = findKill(result, block, 2, atom);
        require(kill != nullptr, "last use did not produce a kill boundary");
        if (kind == UseKind::Consume) {
            require(kill->reason == KillReason::Consumed &&
                        !kill->requiresCleanup,
                "consume kill should already be physically satisfied");
        } else {
            require(kill->reason == KillReason::LastUse &&
                        kill->requiresCleanup,
                "non-consuming last use should request cleanup");
        }
    };

    run(UseKind::Borrow);
    run(UseKind::Consume);
    run(UseKind::Relocate);
    run(UseKind::LValue);

    StableValueTable deadTable;
    const ValueId deadValue = ids.nextValue();
    const AtomId deadAtom = ids.nextAtom();
    registerValue(deadTable, ids.nextBinding(), deadValue, deadAtom);
    const BasicBlockId deadBlock = ids.nextBasicBlock();
    const LifetimeControlFlowGraph deadCfg{
        deadBlock,
        {{deadBlock, {{{ValueDefinition{deadValue}}, {}}}, {}}}
    };
    const auto deadResult = ValueLifetimeAnalyzer::analyze(deadCfg, deadTable);
    const auto* deadKill = findKill(deadResult, deadBlock, 1, deadAtom);
    require(deadResult.valid() && deadKill != nullptr &&
                deadKill->reason == KillReason::DeadDefinition &&
                deadKill->requiresCleanup,
        "unused definition did not produce a cleanup suggestion");
}

void testOrthogonalDeleteUse()
{
    constexpr ValueUse deleteUse{
        ValueId{1}, AccessKind::LValue, OwnershipEffect::Consume
    };
    static_assert(deleteUse.access == AccessKind::LValue);
    static_assert(deleteUse.ownership == OwnershipEffect::Consume);
    static_assert(deleteUse.consumes());
    static_assert(deleteUse.legacyKind() == UseKind::Consume);

    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId value = ids.nextValue();
    const AtomId atom = ids.nextAtom();
    registerValue(table, ids.nextBinding(), value, atom);
    const BasicBlockId block = ids.nextBasicBlock();
    const LifetimeControlFlowGraph cfg{
        block,
        {{block,
          {{{ValueDefinition{value}}, {}},
           {{},
            {ValueUse{
                value, AccessKind::LValue, OwnershipEffect::Consume
            }}}},
          {}}}
    };
    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    const auto* kill = findKill(result, block, 2, atom);
    require(result.valid() && kill &&
                kill->reason == KillReason::Consumed &&
                !kill->requiresCleanup,
        "lvalue+consume Delete use was not modelled as a safe consume");
}

void testBranchMayLiveness()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId value = ids.nextValue();
    const AtomId atom = ids.nextAtom();
    registerValue(table, ids.nextBinding(), value, atom);

    const BasicBlockId entry = ids.nextBasicBlock();
    const BasicBlockId thenBlock = ids.nextBasicBlock();
    const BasicBlockId elseBlock = ids.nextBasicBlock();
    LifetimeControlFlowGraph cfg{
        entry,
        {
            {entry, {{{ValueDefinition{value}}, {}}}, {thenBlock, elseBlock}},
            {elseBlock,
             {{{}, {ValueUse{value, UseKind::Borrow, EscapeKind::None}}}},
             {}},
            {thenBlock,
             {{{}, {ValueUse{value, UseKind::Borrow, EscapeKind::None}}}},
             {}}
        }
    };

    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    require(result.valid(), "branch analysis failed");
    const auto* entryState = result.findBlock(entry);
    require(entryState && entryState->liveOut.contains(value),
        "may-liveness did not union branch successors");
    require(findKill(result, entry, 1, atom) == nullptr,
        "entry killed a value needed by a branch");
    require(findKill(result, thenBlock, 1, atom) != nullptr,
        "then last use lacked a kill");
    require(findKill(result, elseBlock, 1, atom) != nullptr,
        "else last use lacked a kill");
}

void testEdgeSpecificLivenessAndKill()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId value = ids.nextValue();
    const AtomId atom = ids.nextAtom();
    registerValue(table, ids.nextBinding(), value, atom);

    const BasicBlockId entry = ids.nextBasicBlock();
    const BasicBlockId usesValue = ids.nextBasicBlock();
    const BasicBlockId skipsValue = ids.nextBasicBlock();
    const LifetimeControlFlowGraph cfg{
        entry,
        {
            {entry,
             {{{ValueDefinition{value}}, {}}},
             {usesValue, skipsValue}},
            {usesValue,
             {{{}, {ValueUse{value, UseKind::Borrow}}}},
             {}},
            {skipsValue, {}, {}}
        }
    };

    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    require(result.valid(), "edge-specific branch analysis failed");
    const auto* liveEdge = result.findEdge(entry, usesValue);
    const auto* deadEdge = result.findEdge(entry, skipsValue);
    require(liveEdge && liveEdge->liveValues.contains(value) &&
                liveEdge->liveAtoms.contains(atom),
        "use edge lost value/atom liveness");
    require(deadEdge && deadEdge->liveValues.empty() &&
                deadEdge->liveAtoms.empty(),
        "dead edge inherited block-union liveness");
    require(findEdgeKill(result, entry, usesValue, atom) == nullptr,
        "live edge received a kill");
    const auto* edgeKill =
        findEdgeKill(result, entry, skipsValue, atom);
    require(edgeKill && edgeKill->reason == KillReason::EdgeExit &&
                edgeKill->requiresCleanup &&
                edgeKill->retiredValues == std::vector<ValueId>({value}),
        "dead branch edge did not receive its isolated cleanup");
    require(findKill(result, entry, 1, atom) == nullptr,
        "edge cleanup was incorrectly mixed into statement cleanup");
}

void testLoopFixedPoint()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId value = ids.nextValue();
    const AtomId atom = ids.nextAtom();
    registerValue(table, ids.nextBinding(), value, atom);

    const BasicBlockId entry = ids.nextBasicBlock();
    const BasicBlockId header = ids.nextBasicBlock();
    const BasicBlockId body = ids.nextBasicBlock();
    const BasicBlockId exit = ids.nextBasicBlock();

    // Deliberately store blocks out of control-flow order.  The fixed point
    // must depend on IDs and edges, not vector layout.
    LifetimeControlFlowGraph cfg{
        entry,
        {
            {exit,
             {{{}, {ValueUse{value, UseKind::Borrow, EscapeKind::None}}}},
             {}},
            {body,
             {{{}, {ValueUse{value, UseKind::Borrow, EscapeKind::None}}}},
             {header}},
            {entry, {{{ValueDefinition{value}}, {}}}, {header}},
            {header, {}, {body, exit}}
        }
    };

    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    require(result.valid(), "loop analysis failed");
    const auto* bodyState = result.findBlock(body);
    require(bodyState && bodyState->liveOut.contains(value),
        "loop back edge did not keep value may-live");
    require(findKill(result, body, 1, atom) == nullptr,
        "loop body killed a value needed by the next iteration");
    require(findKill(result, exit, 1, atom) != nullptr,
        "loop exit last use lacked a kill");
}

void testAliasAtomDiesOnlyAfterLastAlias()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const BindingId sourceBinding = ids.nextBinding();
    const BindingId aliasBinding = ids.nextBinding();
    const ValueId source = ids.nextValue();
    const ValueId alias = ids.nextValue();
    const AtomId atom = ids.nextAtom();
    registerValue(table, sourceBinding, source, atom);
    require(!table.registerAlias(aliasBinding, alias, source).has_value(),
        "failed to register analysis alias");

    const BasicBlockId block = ids.nextBasicBlock();
    LifetimeControlFlowGraph cfg{
        block,
        {{block,
          {
              {{ValueDefinition{source}}, {}},
              {{ValueDefinition{alias}}, {}},
              {{}, {ValueUse{source, UseKind::Borrow, EscapeKind::None}}},
              {{}, {ValueUse{alias, UseKind::Borrow, EscapeKind::None}}}
          },
          {}}}
    };

    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    require(result.valid(), "alias analysis failed");
    require(result.findBlock(block)->statements[0].liveAfter.contains(source),
        "future alias creation did not keep its source live");
    require(findKill(result, block, 3, atom) == nullptr,
        "source use killed an atom with a live alias");
    const auto* kill = findKill(result, block, 4, atom);
    require(kill != nullptr, "last alias did not kill shared atom");
    require(kill->retiredValues == std::vector<ValueId>({source, alias}),
        "shared-atom kill did not retire all aliases");
}

void testKillOnlyRetiresReachingAliases()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId source = ids.nextValue();
    const ValueId registeredButUnreachedAlias = ids.nextValue();
    const AtomId atom = ids.nextAtom();
    registerValue(table, ids.nextBinding(), source, atom);
    require(!table.registerAlias(
                       ids.nextBinding(),
                       registeredButUnreachedAlias,
                       source
                   )
                 .has_value(),
        "failed to register unreached alias");

    const BasicBlockId block = ids.nextBasicBlock();
    const LifetimeControlFlowGraph cfg{
        block,
        {{block,
          {{{ValueDefinition{source}}, {}},
           {{}, {ValueUse{source, UseKind::Borrow}}}},
          {}}}
    };
    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    const auto* kill = findKill(result, block, 2, atom);
    require(result.valid() && kill,
        "source last-use kill was not produced");
    require(kill->retiredValues == std::vector<ValueId>({source}),
        "kill globally retired an alias whose definition never reached");
}

void testConsumeEscapedAtomIsRejected()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId value = ids.nextValue();
    const AtomId atom = ids.nextAtom();
    registerValue(table, ids.nextBinding(), value, atom);

    const BasicBlockId block = ids.nextBasicBlock();
    LifetimeControlFlowGraph cfg{
        block,
        {{block,
          {
              {{ValueDefinition{value}}, {}},
              {{}, {ValueUse{value, UseKind::Escape, EscapeKind::Explicit}}},
              {{}, {ValueUse{value, UseKind::Consume, EscapeKind::None}}}
          },
          {}}}
    };
    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    require(!result.valid() &&
                hasIssue(result, LifetimeIssueCode::ConsumedEscapedAtom),
        "escaped atom consumption was not conservatively rejected");
    require(findKill(result, block, 3, atom) == nullptr,
        "escaped consume produced a kill suggestion");
}

void testEscapeKindsAreConservative()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId dynamicValue = ids.nextValue();
    const ValueId explicitValue = ids.nextValue();
    const ValueId altValue = ids.nextValue();
    const AtomId dynamicAtom = ids.nextAtom();
    const AtomId explicitAtom = ids.nextAtom();
    const AtomId altAtom = ids.nextAtom();
    registerValue(table, ids.nextBinding(), dynamicValue, dynamicAtom);
    registerValue(table, ids.nextBinding(), explicitValue, explicitAtom);
    registerValue(table, ids.nextBinding(), altValue, altAtom);

    const BasicBlockId block = ids.nextBasicBlock();
    LifetimeControlFlowGraph cfg{
        block,
        {{block,
          {
              {{ValueDefinition{dynamicValue},
                ValueDefinition{explicitValue},
                ValueDefinition{altValue}},
               {}},
              {{},
               {ValueUse{
                    dynamicValue, UseKind::Borrow, EscapeKind::DynamicIndex},
                ValueUse{
                    explicitValue, UseKind::Escape, EscapeKind::Explicit},
                ValueUse{
                    altValue, UseKind::Relocate, EscapeKind::AltStack}}}
          },
          {}}}
    };

    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    require(result.valid(), "escape analysis failed");
    require(result.escapedAtoms ==
                std::set<AtomId>({dynamicAtom, explicitAtom, altAtom}),
        "escape atom set mismatch");
    require(result.kills.empty(),
        "escaped storage received an early kill suggestion");
}

void testDynamicIndexEscapesWholeAggregateAndPrecomputesAtoms()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId aggregate = ids.nextValue();
    const AtomId first = ids.nextAtom();
    const AtomId second = ids.nextAtom();
    require(!table.registerAggregate(
                       ids.nextBinding(),
                       aggregate,
                       {{{0}, first}, {{1}, second}}
                   )
                 .has_value(),
        "aggregate registration failed");

    const BasicBlockId block = ids.nextBasicBlock();
    const LifetimeControlFlowGraph cfg{
        block,
        {{block,
          {{{ValueDefinition{aggregate}}, {}},
           {{},
            {ValueUse{
                aggregate,
                AccessKind::Borrow,
                OwnershipEffect::None,
                EscapeKind::DynamicIndex
            }}}},
          {}}}
    };
    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    const auto expectedAtoms = AtomSet({first, second});
    const auto* live = result.findBlock(block);
    require(result.valid() && live,
        "aggregate dynamic-index analysis failed");
    require(result.escapedAtoms == expectedAtoms,
        "dynamic index did not escape the complete may-alias set");
    require(live->statements[0].liveAtomsAfter == expectedAtoms &&
                live->statements[1].liveAtomsBefore == expectedAtoms,
        "statement live atom sets were not precomputed for every leaf");
    require(live->liveAtomsIn.empty() && live->liveAtomsOut.empty(),
        "block live atom summaries do not match value liveness");
    require(result.kills.empty() && result.edgeKills.empty(),
        "escaped aggregate received a cleanup suggestion");
}

void testConsumeWithLiveAliasIsRejected()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId source = ids.nextValue();
    const ValueId alias = ids.nextValue();
    const AtomId atom = ids.nextAtom();
    registerValue(table, ids.nextBinding(), source, atom);
    require(!table.registerAlias(ids.nextBinding(), alias, source).has_value(),
        "failed to register consume alias");

    const BasicBlockId block = ids.nextBasicBlock();
    LifetimeControlFlowGraph cfg{
        block,
        {{block,
          {
              {{ValueDefinition{source}, ValueDefinition{alias}}, {}},
              {{}, {ValueUse{source, UseKind::Consume, EscapeKind::None}}},
              {{}, {ValueUse{alias, UseKind::Borrow, EscapeKind::None}}}
          },
          {}}}
    };
    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    require(!result.valid() &&
                hasIssue(result, LifetimeIssueCode::ConsumedAtomStillLive),
        "consume with a may-live alias was not diagnosed");
    require(findKill(result, block, 2, atom) == nullptr,
        "unsafe consume produced a kill suggestion");
}

void testParametersDefinitionsAndDominanceValidation()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId value = ids.nextValue();
    const AtomId atom = ids.nextAtom();
    registerValue(table, ids.nextBinding(), value, atom);
    const BasicBlockId block = ids.nextBasicBlock();

    const LifetimeControlFlowGraph parameterCfg{
        block,
        {{block, {{{}, {ValueUse{value, UseKind::Borrow}}}}, {}}},
        {value}
    };
    const auto parameterResult =
        ValueLifetimeAnalyzer::analyze(parameterCfg, table);
    require(parameterResult.valid() &&
                parameterResult.findBlock(block)->liveIn.contains(value),
        "declared parameter/live-in was not available on entry");

    const LifetimeControlFlowGraph missingDefinitionCfg{
        block, {{block, {{{}, {ValueUse{value, UseKind::Borrow}}}}, {}}}
    };
    require(hasIssue(
                ValueLifetimeAnalyzer::analyze(missingDefinitionCfg, table),
                LifetimeIssueCode::MissingDefinition
            ),
        "use without parameter or definition was accepted");

    const LifetimeControlFlowGraph parameterDefinedCfg{
        block,
        {{block, {{{ValueDefinition{value}}, {}}}, {}}},
        {value}
    };
    require(hasIssue(
                ValueLifetimeAnalyzer::analyze(parameterDefinedCfg, table),
                LifetimeIssueCode::ParameterDefined
            ),
        "parameter was allowed to have a CFG definition");

    const LifetimeControlFlowGraph duplicateDefinitionCfg{
        block,
        {{block,
          {{{ValueDefinition{value}}, {}},
           {{ValueDefinition{value}}, {}}},
          {}}}
    };
    require(hasIssue(
                ValueLifetimeAnalyzer::analyze(
                    duplicateDefinitionCfg, table
                ),
                LifetimeIssueCode::DuplicateDefinition
            ),
        "duplicate stable-value definition was accepted");

    const LifetimeControlFlowGraph localUseBeforeDefCfg{
        block,
        {{block,
          {{{}, {ValueUse{value, UseKind::Borrow}}},
           {{ValueDefinition{value}}, {}}},
          {}}}
    };
    require(hasIssue(
                ValueLifetimeAnalyzer::analyze(
                    localUseBeforeDefCfg, table
                ),
                LifetimeIssueCode::UseBeforeDefinition
            ),
        "same-block use-before-definition was accepted");

    const BasicBlockId entry = ids.nextBasicBlock();
    const BasicBlockId defines = ids.nextBasicBlock();
    const BasicBlockId skips = ids.nextBasicBlock();
    const BasicBlockId join = ids.nextBasicBlock();
    const LifetimeControlFlowGraph nonDominatingDefinitionCfg{
        entry,
        {
            {entry, {}, {defines, skips}},
            {defines, {{{ValueDefinition{value}}, {}}}, {join}},
            {skips, {}, {join}},
            {join, {{{}, {ValueUse{value, UseKind::Borrow}}}}, {}}
        }
    };
    require(hasIssue(
                ValueLifetimeAnalyzer::analyze(
                    nonDominatingDefinitionCfg, table
                ),
                LifetimeIssueCode::UseBeforeDefinition
            ),
        "join use accepted a definition from only one predecessor");
}

void testAliasSourceMustReachDefinition()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const ValueId source = ids.nextValue();
    const ValueId alias = ids.nextValue();
    registerValue(table, ids.nextBinding(), source, ids.nextAtom());
    require(!table.registerAlias(ids.nextBinding(), alias, source).has_value(),
        "alias registration failed");

    const BasicBlockId entry = ids.nextBasicBlock();
    const BasicBlockId definesSource = ids.nextBasicBlock();
    const BasicBlockId skipsSource = ids.nextBasicBlock();
    const BasicBlockId join = ids.nextBasicBlock();
    const LifetimeControlFlowGraph cfg{
        entry,
        {
            {entry, {}, {definesSource, skipsSource}},
            {definesSource, {{{ValueDefinition{source}}, {}}}, {join}},
            {skipsSource, {}, {join}},
            {join, {{{ValueDefinition{alias}}, {}}}, {}}
        }
    };
    const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
    require(!result.valid() &&
                hasIssue(result, LifetimeIssueCode::AliasSourceUnavailable),
        "alias definition accepted a non-dominating relation source");
}

void testAmbiguousConsumesAreRejected()
{
    StableValueIdFactory ids;

    {
        StableValueTable table;
        const ValueId source = ids.nextValue();
        const ValueId alias = ids.nextValue();
        const AtomId atom = ids.nextAtom();
        registerValue(table, ids.nextBinding(), source, atom);
        require(!table.registerAlias(ids.nextBinding(), alias, source)
                     .has_value(),
            "same-statement alias registration failed");
        const BasicBlockId block = ids.nextBasicBlock();
        const LifetimeControlFlowGraph cfg{
            block,
            {{block,
              {{{ValueDefinition{source}}, {}},
               {{ValueDefinition{alias}},
                {ValueUse{
                    source,
                    AccessKind::LValue,
                    OwnershipEffect::Consume
                }}}},
              {}}}
        };
        const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
        require(hasIssue(result, LifetimeIssueCode::ConsumeWithDefinition),
            "consume+definition of one atom was not rejected");
        require(findKill(result, block, 2, atom) == nullptr,
            "ambiguous consume+definition produced a kill");
    }

    {
        StableValueTable table;
        const ValueId source = ids.nextValue();
        const ValueId alias = ids.nextValue();
        const AtomId atom = ids.nextAtom();
        registerValue(table, ids.nextBinding(), source, atom);
        require(!table.registerAlias(ids.nextBinding(), alias, source)
                     .has_value(),
            "multiple-consume alias registration failed");
        const BasicBlockId block = ids.nextBasicBlock();
        const LifetimeControlFlowGraph cfg{
            block,
            {{block,
              {{{ValueDefinition{source}}, {}},
               {{ValueDefinition{alias}}, {}},
               {{},
                {ValueUse{source, UseKind::Consume},
                 ValueUse{alias, UseKind::Consume}}}},
              {}}}
        };
        const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
        require(hasIssue(result, LifetimeIssueCode::MultipleConsume),
            "multiple consumes of one atom were not rejected");
        require(findKill(result, block, 3, atom) == nullptr,
            "multiple-consume statement produced a kill");
    }

    {
        StableValueTable table;
        const ValueId consumed = ids.nextValue();
        const ValueId fresh = ids.nextValue();
        const AtomId consumedAtom = ids.nextAtom();
        const AtomId freshAtom = ids.nextAtom();
        registerValue(
            table, ids.nextBinding(), consumed, consumedAtom
        );
        registerValue(table, ids.nextBinding(), fresh, freshAtom);
        const BasicBlockId block = ids.nextBasicBlock();
        const LifetimeControlFlowGraph cfg{
            block,
            {{block,
              {{{ValueDefinition{consumed}}, {}},
               {{ValueDefinition{fresh}},
                {ValueUse{
                    consumed,
                    AccessKind::LValue,
                    OwnershipEffect::Consume
                }}}},
              {}}}
        };
        const auto result = ValueLifetimeAnalyzer::analyze(cfg, table);
        require(result.valid() &&
                    findKill(result, block, 2, consumedAtom) != nullptr &&
                    findKill(result, block, 2, freshAtom) != nullptr,
            "consume plus independent fresh definition was over-rejected");
    }
}

void testInvalidCfgAndStrictBenefitPolicy()
{
    StableValueIdFactory ids;
    StableValueTable table;
    const BasicBlockId block = ids.nextBasicBlock();
    LifetimeControlFlowGraph invalid{
        block,
        {{block,
          {{{}, {ValueUse{ValueId{999}, UseKind::Borrow, EscapeKind::None}}}},
          {}}}
    };
    const auto invalidResult = ValueLifetimeAnalyzer::analyze(invalid, table);
    require(!invalidResult.valid() &&
                hasIssue(invalidResult, LifetimeIssueCode::UnknownValue),
        "unknown CFG value was accepted");

    constexpr auto win = StrictBenefitPolicy::evaluate(10, 9);
    constexpr auto tie = StrictBenefitPolicy::evaluate(10, 10);
    constexpr auto loss = StrictBenefitPolicy::evaluate(10, 11);
    static_assert(win.accepted() && win.savings() == 1);
    static_assert(!tie.accepted() && tie.savings() == 0);
    static_assert(!loss.accepted() && loss.savings() == 0);
    require(StrictBenefitPolicy::accepts(2, 1),
        "strictly smaller candidate was rejected");
    require(!StrictBenefitPolicy::accepts(2, 2) &&
                !StrictBenefitPolicy::accepts(2, 3),
        "non-smaller candidate was accepted");
}

} // namespace

int main()
{
    testStrongIdentityAndAliasTable();
    testFreshAtomsAggregatesAndExplicitRelations();
    testStraightLineKindsAndKillBoundaries();
    testOrthogonalDeleteUse();
    testBranchMayLiveness();
    testEdgeSpecificLivenessAndKill();
    testLoopFixedPoint();
    testAliasAtomDiesOnlyAfterLastAlias();
    testKillOnlyRetiresReachingAliases();
    testEscapeKindsAreConservative();
    testDynamicIndexEscapesWholeAggregateAndPrecomputesAtoms();
    testConsumeWithLiveAliasIsRejected();
    testConsumeEscapedAtomIsRejected();
    testParametersDefinitionsAndDominanceValidation();
    testAliasSourceMustReachDefinition();
    testAmbiguousConsumesAreRejected();
    testInvalidCfgAndStrictBenefitPolicy();
    std::cout << "value_lifetime_analysis_test: PASS\n";
    return 0;
}
