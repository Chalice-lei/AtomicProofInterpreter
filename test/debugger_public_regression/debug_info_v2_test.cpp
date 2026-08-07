#include "debugger/info/debug_info.h"
#include "debugger/info/debug_info_generator.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool hasVariable(
    const std::vector<apc_debug::VariableDebugInfo>& variables,
    const std::string& name
)
{
    return std::any_of(
        variables.begin(),
        variables.end(),
        [&](const auto& variable) { return variable.name == name; }
    );
}

const apc_debug::VariableDebugInfo* findVariable(
    const std::vector<apc_debug::VariableDebugInfo>& variables,
    const std::string& name
)
{
    const auto found = std::find_if(
        variables.begin(),
        variables.end(),
        [&](const auto& variable) { return variable.name == name; }
    );
    return found == variables.end() ? nullptr : &*found;
}
} // namespace

int main()
{
    using namespace apc_debug;

    bool ok = true;

    // Functions containing only compile-time declarations legitimately have
    // an empty half-open runtime range. V2 validation and JSON upgrade must
    // preserve that range instead of treating it as reversed.
    {
        DebugInfoGenerator emptyGenerator("empty-runtime-function.ct");
        emptyGenerator.onEnterFunction(
            "setup",
            true,
            SourceLocation("empty-runtime-function.ct", 1, 1),
            0
        );
        emptyGenerator.onExitFunction(0);
        emptyGenerator.finalizeScopes();
        const auto emptyInfo = emptyGenerator.getDebugInfo();
        const auto emptyRoundTrip =
            DebugInfo::fromJson(emptyInfo->toJson());
        ok &= expect(
            emptyInfo->validate() && emptyRoundTrip &&
                emptyRoundTrip->validate() &&
                emptyRoundTrip->functions.at("setup").startPC == 0 &&
                emptyRoundTrip->functions.at("setup").endPC == 0,
            "empty runtime function range was rejected or changed"
        );
    }

    const std::string source = "debug-v2-fixture/main.ct";
    DebugInfoGenerator generator(source);
    generator.setContractName("DebugV2");
    generator.onEnterFunction(
        "unlock",
        true,
        SourceLocation(source, 1, 1),
        0
    );
    generator.onVariableDecl(
        "parentVisible",
        "number",
        SourceLocation(source, 2, 5),
        true,
        0
    );
    generator.onVariableDecl(
        "functionAutoClosed",
        "number",
        SourceLocation(source, 3, 5),
        true,
        1
    );
    generator.onEmitInstruction(
        0,
        "63",
        "",
        SourceLocation(source, 5, 1)
    );

    const ScopeId thenScope = generator.onEnterScope(
        "branch",
        ScopeType::CONDITIONAL,
        SourceLocation(source, 10, 1),
        1
    );
    generator.onVariableDecl(
        "thenOnly",
        "number",
        SourceLocation(source, 10, 5),
        true,
        0
    );
    generator.onEmitInstruction(
        1,
        "87",
        "",
        SourceLocation(source, 10, 9),
        {{0, BranchArm::Then}}
    );
    generator.onExitScope(2);

    const ScopeId elseScope = generator.onEnterScope(
        "branch",
        ScopeType::CONDITIONAL,
        SourceLocation(source, 20, 1),
        2
    );
    generator.onVariableDecl(
        "elseOnly",
        "number",
        SourceLocation(source, 20, 5),
        true,
        0
    );
    generator.onEmitInstruction(
        2,
        "87",
        "",
        SourceLocation(source, 20, 9),
        {{0, BranchArm::Else}}
    );
    generator.onExitScope(3);
    generator.onVariableEnd("parentVisible", 3);
    generator.onEmitInstruction(
        3,
        "68",
        "",
        SourceLocation(source, 21, 1)
    );
    generator.onExitFunction(4);

    auto original = generator.getDebugInfo();
    auto disjoint = std::make_shared<ScopeDebugInfo>(
        "disjoint",
        ScopeType::BLOCK
    );
    disjoint->parent = original->globalScope;
    disjoint->addRange(0, 1);
    disjoint->addRange(2, 3);
    original->addScope(disjoint);
    original->globalScope->children.push_back(disjoint);

    ok &= expect(thenScope != INVALID_SCOPE_ID, "then scope has no stable ID");
    ok &= expect(
        elseScope != INVALID_SCOPE_ID && elseScope != thenScope,
        "branch scopes must have distinct stable IDs"
    );
    ok &= expect(original->validate(), "generated V2 debug info is invalid");
    const auto originalFunction = original->functions.find("unlock");
    const auto originalParentScope = original->getScopeById(
        originalFunction->second.scopeId
    );
    const auto* scopeParent = findVariable(
        originalParentScope->variables,
        "parentVisible"
    );
    const auto* functionParent = findVariable(
        originalFunction->second.localVars,
        "parentVisible"
    );
    const auto topParent = original->variables.find("parentVisible");
    const auto* automaticallyClosed = findVariable(
        originalFunction->second.localVars,
        "functionAutoClosed"
    );
    const auto originalThenScope = original->getScopeById(thenScope);
    const auto* automaticallyScopeClosed = findVariable(
        originalThenScope->variables,
        "thenOnly"
    );
    ok &= expect(
        scopeParent && functionParent && topParent != original->variables.end() &&
            scopeParent->hasExplicitAvailability &&
            scopeParent->availabilityRanges ==
                std::vector<PCRange>{PCRange(0, 3)} &&
            functionParent->availabilityRanges ==
                scopeParent->availabilityRanges &&
            topParent->second.availabilityRanges ==
                scopeParent->availabilityRanges,
        "variable lifetime was not synchronized across debug-info copies"
    );
    ok &= expect(
        automaticallyClosed && automaticallyScopeClosed &&
            automaticallyClosed->availabilityRanges ==
                std::vector<PCRange>{PCRange(0, 4)} &&
            automaticallyScopeClosed->availabilityRanges ==
                std::vector<PCRange>{PCRange(1, 2)},
        "scope/function exit did not close active variable ranges"
    );

    const std::vector<size_t> oldToNew{0, 2, 2, 1};
    std::vector<std::vector<OriginRewriteRef>> origins(3);
    origins[0] = {{0, {}}};
    origins[1] = {{3, {}}};
    origins[2] = {
        {1, {{0, BranchArm::Then}}},
        {2, {{0, BranchArm::Else}}},
    };
    auto rewritten = original->remapped(oldToNew, 3, origins);
    ok &= expect(rewritten != nullptr, "transactional provenance remap failed");
    if (!rewritten) {
        return 1;
    }

    const BranchTrace thenTrace{{0, BranchArm::Then}};
    const BranchTrace elseTrace{{0, BranchArm::Else}};
    ok &= expect(
        rewritten->getOriginsForPC(2).size() == 2,
        "many-to-one rewrite did not retain both origins"
    );
    ok &= expect(
        rewritten->getSourceLocation(2, thenTrace).line == 10,
        "then trace resolved to the wrong source origin"
    );
    ok &= expect(
        rewritten->getSourceLocation(2, elseTrace).line == 20,
        "else trace resolved to the wrong source origin"
    );
    ok &= expect(
        rewritten->getPCsForLine(10) == std::vector<size_t>{2} &&
            rewritten->getPCsForLine(20) == std::vector<size_t>{2},
        "line index was not built from every source origin"
    );
    ok &= expect(
        rewritten->hasActiveSourceOrigin(2, source, 10, thenTrace) &&
            !rewritten->hasActiveSourceOrigin(2, source, 20, thenTrace),
        "active-origin breakpoint filtering ignored the branch trace"
    );
    auto multiOrigin = DebugInfo::fromJson(rewritten->toJson());
    SourceOrigin secondThenOrigin(SourceLocation(source, 11, 1));
    secondThenOrigin.scopeId = thenScope;
    secondThenOrigin.originalPC = 1;
    secondThenOrigin.path = {{0, BranchArm::Then}};
    if (multiOrigin) {
        multiOrigin->addInstructionOrigin(2, secondThenOrigin);
    }
    ok &= expect(
        multiOrigin && multiOrigin->validate() &&
            multiOrigin->hasActiveSourceOrigin(
                2, source, 10, thenTrace
            ) &&
            multiOrigin->hasActiveSourceOrigin(
                2, source, 11, thenTrace
            ) &&
            !multiOrigin->hasActiveSourceOrigin(
                2, source, 11, elseTrace
            ),
        "active-origin lookup only inspected the primary compatible origin"
    );
    ok &= expect(
        rewritten->getScopeAtPC(2, thenTrace)->scopeId == thenScope &&
            rewritten->getScopeAtPC(2, elseTrace)->scopeId == elseScope,
        "path-qualified scope lookup selected the wrong branch scope"
    );

    // A later cleanup pass moves the IF opener. Existing provenance paths must
    // follow that remap instead of retaining the stale region PC.
    const std::vector<size_t> cleanupMap{1, 2, 3};
    std::vector<std::vector<OriginRewriteRef>> cleanupOrigins(4);
    cleanupOrigins[1] = {{0, {}}};
    cleanupOrigins[2] = {{1, {}}};
    cleanupOrigins[3] = {{2, {}}};
    auto cleaned = rewritten->remapped(cleanupMap, 4, cleanupOrigins);
    ok &= expect(
        cleaned &&
            cleaned->getSourceLocation(
                3,
                BranchTrace{{1, BranchArm::Then}}
            ).line == 10 &&
            !cleaned->getSourceLocation(
                 3,
                 BranchTrace{{0, BranchArm::Then}}
             ).isValid(),
        "structured-to-cleanup remap left a stale branch region PC"
    );

    const auto thenVariables =
        rewritten->getVariablesInScope(2, thenTrace);
    const auto elseVariables =
        rewritten->getVariablesInScope(2, elseTrace);
    const auto ambiguousVariables = rewritten->getVariablesInScope(2);
    ok &= expect(
        hasVariable(thenVariables, "thenOnly") &&
            !hasVariable(thenVariables, "elseOnly"),
        "then branch leaked an else-only variable"
    );
    ok &= expect(
        hasVariable(elseVariables, "elseOnly") &&
            !hasVariable(elseVariables, "thenOnly"),
        "else branch leaked a then-only variable"
    );
    ok &= expect(
        hasVariable(ambiguousVariables, "parentVisible") &&
            !hasVariable(ambiguousVariables, "thenOnly") &&
            !hasVariable(ambiguousVariables, "elseOnly"),
        "empty branch trace guessed a branch scope instead of using the LCA"
    );
    ok &= expect(
        !hasVariable(
            rewritten->getVariablesInScope(1),
            "parentVisible"
        ) &&
            rewritten->getVariableInfo(
                "parentVisible", 1, BranchTrace{}
            ) == nullptr &&
            hasVariable(thenVariables, "parentVisible"),
        "ended availability range leaked through the legacy variable map"
    );
    ok &= expect(
        rewritten->getVariableInfo("thenOnly", 2, thenTrace) != nullptr &&
            rewritten->getVariableInfo("elseOnly", 2, thenTrace) == nullptr,
        "path-qualified variable lookup ignored branch visibility"
    );

    auto remappedDisjoint = rewritten->getScopeById(disjoint->scopeId);
    ok &= expect(
        remappedDisjoint && remappedDisjoint->ranges.size() == 2 &&
            remappedDisjoint->ranges[0] == PCRange(0, 1) &&
            remappedDisjoint->ranges[1] == PCRange(2, 3),
        "scope remap did not retain disjoint half-open ranges"
    );

    const std::string serialized = rewritten->toJson();
    const auto json = nlohmann::json::parse(serialized);
    ok &= expect(
        json.value("version", "") == "2.0",
        "V2 serializer emitted the wrong version"
    );
    auto roundTrip = DebugInfo::fromJson(serialized);
    ok &= expect(
        roundTrip && roundTrip->validate() &&
            roundTrip->getOriginsForPC(2).size() == 2 &&
            roundTrip->getSourceLocation(2, elseTrace).line == 20,
        "V2 JSON round trip lost provenance"
    );
    ok &= expect(
        roundTrip &&
            !hasVariable(roundTrip->getVariablesInScope(1), "parentVisible") &&
            hasVariable(
                roundTrip->getVariablesInScope(2, thenTrace),
                "parentVisible"
            ),
        "V2 JSON round trip lost half-open variable availability"
    );

    auto invalidArmJson = json;
    for (auto& instruction : invalidArmJson["instructions"]) {
        if (instruction.value("pc", 0U) == 2U) {
            instruction["origins"][0]["path"][0]["arm"] = "sideways";
            break;
        }
    }
    ok &= expect(
        DebugInfo::fromJson(invalidArmJson.dump()) == nullptr,
        "V2 loader accepted an unknown branch arm"
    );

    auto cyclicScopesJson = json;
    ScopeId globalScopeId = INVALID_SCOPE_ID;
    ScopeId functionScopeId = INVALID_SCOPE_ID;
    for (const auto& scopeJson : cyclicScopesJson["scopes"]) {
        if (scopeJson.value("name", "") == "global") {
            globalScopeId = scopeJson.value(
                "scopeId", INVALID_SCOPE_ID
            );
        } else if (scopeJson.value("name", "") == "unlock") {
            functionScopeId = scopeJson.value(
                "scopeId", INVALID_SCOPE_ID
            );
        }
    }
    for (auto& scopeJson : cyclicScopesJson["scopes"]) {
        if (scopeJson.value("scopeId", INVALID_SCOPE_ID) == globalScopeId) {
            scopeJson["parentScopeId"] = functionScopeId;
        } else if (scopeJson.value("scopeId", INVALID_SCOPE_ID) ==
                   functionScopeId) {
            scopeJson["parentScopeId"] = globalScopeId;
        }
    }
    ok &= expect(
        globalScopeId != INVALID_SCOPE_ID &&
            functionScopeId != INVALID_SCOPE_ID &&
            DebugInfo::fromJson(cyclicScopesJson.dump()) == nullptr,
        "V2 loader accepted a cyclic scope-parent graph"
    );

    auto invalidPredicateOrder = DebugInfo::fromJson(serialized);
    if (invalidPredicateOrder) {
        invalidPredicateOrder->instructions[2].opcode = "OP_IF";
        invalidPredicateOrder->instructions[2].origins[0].originalPC = 99;
        invalidPredicateOrder->instructions[2].origins[0].path = {
            {2, BranchArm::Then}
        };
    }
    ok &= expect(
        invalidPredicateOrder && !invalidPredicateOrder->validate(),
        "predicate ordering used originalPC instead of the final instruction PC"
    );

    const std::string v1 = R"JSON({
      "version":"1.0",
      "format":"apc-debug",
      "sourceFile":"legacy.ct",
      "contractName":"Legacy",
      "pcToSource":{"0":{"file":"legacy.ct","line":7,"column":1,"endLine":7,"endColumn":1}},
      "instructions":[{"pc":0,"opcode":"51","operand":"","affectedVars":[],"location":{"file":"legacy.ct","line":7,"column":1,"endLine":7,"endColumn":1}}],
      "functions":[{"name":"unlock","startPC":0,"endPC":1,"isPublic":true,"location":{"file":"legacy.ct","line":1,"column":1,"endLine":1,"endColumn":1},"parameters":[],"localVars":[]}],
      "variables":[],
      "scopes":[
        {"index":0,"name":"global","type":"global","startPC":0,"endPC":1,"parentIndex":-1,"variables":[]},
        {"index":1,"name":"unlock","type":"function","startPC":0,"endPC":1,"parentIndex":0,"variables":[]}
      ]
    })JSON";
    auto upgraded = DebugInfo::fromJson(v1);
    ok &= expect(
        upgraded && upgraded->version == "1.0" &&
            upgraded->getOriginsForPC(0).size() == 1 &&
            upgraded->getOriginsForPC(0)[0].scopeId != INVALID_SCOPE_ID,
        "V1 loader did not synthesize V2 scope/origin data"
    );
    if (upgraded) {
        const auto upgradedJson = nlohmann::json::parse(upgraded->toJson());
        ok &= expect(
            upgradedJson.value("version", "") == "2.0",
            "saving loaded V1 data did not upgrade it to V2"
        );
    }

    auto legacyWithoutScopes = std::make_shared<DebugInfo>();
    legacyWithoutScopes->sourceFilename = "legacy-no-scopes.ct";
    legacyWithoutScopes->addInstruction(
        0,
        "51",
        "",
        SourceLocation("legacy-no-scopes.ct", 7, 1)
    );
    FunctionDebugInfo legacyFunction("unlock");
    legacyFunction.startPC = 0;
    legacyFunction.endPC = 1;
    VariableDebugInfo legacyVariable;
    legacyVariable.name = "legacyValue";
    legacyVariable.type = "number";
    legacyVariable.declLine = 6;
    legacyVariable.isStackVar = true;
    legacyVariable.stackOffset = 0;
    legacyFunction.localVars.push_back(legacyVariable);
    legacyWithoutScopes->addFunction(legacyFunction);
    legacyWithoutScopes->addVariable(legacyVariable);
    const auto* legacyValue = legacyWithoutScopes->getVariableInfo(
        "legacyValue", 0, BranchTrace{}
    );
    ok &= expect(
        legacyValue && legacyValue->name == "legacyValue",
        "scope-aware lookup lost its V1 no-scope fallback"
    );

    auto invalidOrigins = origins;
    invalidOrigins[2].push_back({99, {}});
    const auto beforeFailure = original->toJson();
    ok &= expect(
        !original->applyRemapTransactional(oldToNew, 3, invalidOrigins) &&
            original->toJson() == beforeFailure,
        "failed transactional remap modified the original DebugInfo"
    );

    std::weak_ptr<ScopeDebugInfo> releasedParent;
    std::weak_ptr<ScopeDebugInfo> releasedChild;
    {
        auto ownedInfo = std::make_shared<DebugInfo>();
        auto parent = std::make_shared<ScopeDebugInfo>(
            "owned-parent", ScopeType::GLOBAL
        );
        auto child = std::make_shared<ScopeDebugInfo>(
            "owned-child", ScopeType::BLOCK
        );
        child->parent = parent;
        parent->children.push_back(child);
        ownedInfo->addScope(parent);
        ownedInfo->addScope(child);
        releasedParent = parent;
        releasedChild = child;
    }
    ok &= expect(
        releasedParent.expired() && releasedChild.expired(),
        "scope parent/child ownership formed a reference cycle"
    );

    if (!ok) {
        return 1;
    }
    std::cout << "DebugInfo V2 checks passed.\n";
    return 0;
}
