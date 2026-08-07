#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "../../src/bytecode/structured_if_tail_optimizer.h"

namespace
{

using tbc::StructuredIfTailOptimizer;
using tbc::StructuredIfTailOptions;
using tbc::StructuredIfTailRewriteResult;
using tbc::BranchArm;
using tbc::BranchPredicate;
using tbc::InstructionOriginRef;

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "structured_if_tail_optimizer_test: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

void expectInstructions(
    const StructuredIfTailRewriteResult& result,
    const std::vector<std::string>& expected,
    const std::string& testName
)
{
    if (result.instructions != expected) {
        std::cerr << testName << " expected:";
        for (const auto& instruction : expected) {
            std::cerr << ' ' << instruction;
        }
        std::cerr << "\n" << testName << " actual:  ";
        for (const auto& instruction : result.instructions) {
            std::cerr << ' ' << instruction;
        }
        std::cerr << '\n';
        fail(testName + " instruction mismatch");
    }
}

void expectCompleteMapping(
    const StructuredIfTailRewriteResult& result,
    size_t oldSize,
    const std::string& testName
)
{
    expect(result.oldToNew.size() == oldSize, testName + " oldToNew size");
    for (size_t oldPC = 0; oldPC < oldSize; ++oldPC) {
        expect(
            result.oldToNew[oldPC] < result.instructions.size(),
            testName + " unmapped old PC " + std::to_string(oldPC)
        );
    }
    expect(
        result.newToOldOrigins.size() == result.instructions.size(),
        testName + " newToOldOrigins size"
    );
    for (const auto& origins : result.newToOldOrigins) {
        expect(!origins.empty(), testName + " empty origin list");
    }
    expect(result.rewritePlanValid, testName + " rewrite plan valid flag");
    expect(
        result.rewritePlan.oldToNew == result.oldToNew,
        testName + " compatibility oldToNew"
    );
    expect(
        result.rewritePlan.newToOld.size() ==
            result.newToOldOrigins.size(),
        testName + " compatibility reverse mapping size"
    );
    for (size_t newPC = 0;
         newPC < result.rewritePlan.newToOld.size(); ++newPC) {
        std::vector<size_t> oldPCs;
        for (const InstructionOriginRef& origin :
             result.rewritePlan.newToOld[newPC]) {
            oldPCs.push_back(origin.oldPC);
        }
        expect(
            oldPCs == result.newToOldOrigins[newPC],
            testName + " compatibility reverse mapping values"
        );
    }
}

void expectOrigins(
    const StructuredIfTailRewriteResult& result,
    size_t newPC,
    const std::vector<InstructionOriginRef>& expected,
    const std::string& testName
)
{
    expect(
        newPC < result.rewritePlan.newToOld.size(),
        testName + " new PC in range"
    );
    expect(
        result.rewritePlan.newToOld[newPC] == expected,
        testName + " path-aware origins"
    );
}

void testSimpleTailAndMapping()
{
    const std::vector<std::string> input = {
        "63", "51", "76", "67", "52", "76", "68"
    };
    const auto result = StructuredIfTailOptimizer::optimize(input);

    expect(result.changed, "simple tail should change");
    expect(result.structurallyValid, "simple tail should be structurally valid");
    expect(result.mergedIfCount == 1, "simple tail merge count");
    expect(result.removedInstructionCount == 1, "simple tail removed count");
    expectInstructions(
        result,
        {"63", "51", "67", "52", "68", "76"},
        "simple tail"
    );
    expectCompleteMapping(result, input.size(), "simple tail");
    expect(result.oldToNew == std::vector<size_t>({0, 1, 5, 2, 3, 5, 4}),
           "simple tail oldToNew values");
    expect(result.newToOldOrigins[5] == std::vector<size_t>({2, 5}),
           "simple tail dual origin");
    expectOrigins(
        result,
        5,
        {
            {2, {{0, BranchArm::THEN}}},
            {5, {{0, BranchArm::ELSE}}}
        },
        "simple tail dual path"
    );
    expectOrigins(
        result,
        1,
        {{1, {{0, BranchArm::THEN}}}},
        "simple then path"
    );
    expectOrigins(
        result,
        3,
        {{4, {{0, BranchArm::ELSE}}}},
        "simple else path"
    );
    expectOrigins(result, 0, {{0, {}}}, "IF marker has outer path only");
    expectOrigins(result, 2, {{3, {}}}, "ELSE marker has outer path only");
    expectOrigins(result, 4, {{6, {}}}, "ENDIF marker has outer path only");
    expect(
        tbc::validateInstructionRewritePlan(
            result.rewritePlan, input, result.instructions
        ),
        "simple rewrite plan validation"
    );

    const auto secondPass =
        StructuredIfTailOptimizer::optimize(result.instructions);
    expect(!secondPass.changed, "optimizer must be idempotent");
    expectInstructions(secondPass, result.instructions, "idempotent pass");
}

void testLongestMultiInstructionTail()
{
    const std::vector<std::string> input = {
        "64", "51", "76", "93", "67", "52", "76", "93", "68"
    };
    const auto result = StructuredIfTailOptimizer::optimize(input);
    expectInstructions(
        result,
        {"64", "51", "67", "52", "68", "76", "93"},
        "multi instruction tail"
    );
    expect(result.removedInstructionCount == 2, "multi tail removed count");
}

void testNoElseAndDifferentTailRemainUnchanged()
{
    const std::vector<std::string> noElse = {"63", "51", "76", "68"};
    auto result = StructuredIfTailOptimizer::optimize(noElse);
    expect(!result.changed, "implicit else must not change");
    expectInstructions(result, noElse, "implicit else");

    const std::vector<std::string> different = {
        "63", "51", "76", "67", "52", "75", "68"
    };
    result = StructuredIfTailOptimizer::optimize(different);
    expect(!result.changed, "different tails must not change");
    expectInstructions(result, different, "different tails");
}

void testCompleteNestedNodeCanMove()
{
    const std::vector<std::string> input = {
        "63", "51", "63", "53", "67", "54", "68",
        "67", "52", "63", "53", "67", "54", "68", "68"
    };
    const auto result = StructuredIfTailOptimizer::optimize(input);
    expectInstructions(
        result,
        {"63", "51", "67", "52", "68", "63", "53", "67", "54", "68"},
        "complete nested node"
    );
    expect(result.mergedIfCount == 1, "complete nested merge count");
    expectCompleteMapping(result, input.size(), "complete nested node");
    expectOrigins(
        result,
        5,
        {
            {2, {{0, BranchArm::THEN}}},
            {9, {{0, BranchArm::ELSE}}}
        },
        "merged nested opener paths"
    );
    expectOrigins(
        result,
        6,
        {
            {3,
             {{0, BranchArm::THEN}, {5, BranchArm::THEN}}},
            {10,
             {{0, BranchArm::ELSE}, {5, BranchArm::THEN}}}
        },
        "merged nested then paths"
    );
    expectOrigins(
        result,
        8,
        {
            {5,
             {{0, BranchArm::THEN}, {5, BranchArm::ELSE}}},
            {12,
             {{0, BranchArm::ELSE}, {5, BranchArm::ELSE}}}
        },
        "merged nested else paths"
    );
    expect(
        tbc::validateInstructionRewritePlan(
            result.rewritePlan, input, result.instructions
        ),
        "nested rewrite plan validation"
    );
}

void testNestedNodeIsNeverPartiallyCut()
{
    const std::vector<std::string> input = {
        "63", "51", "63", "53", "67", "54", "68",
        "67", "52", "63", "55", "67", "54", "68", "68"
    };
    const auto result = StructuredIfTailOptimizer::optimize(input);
    expect(!result.changed, "different nested nodes must remain whole");
    expectInstructions(result, input, "no partial nested cut");
}

void testInnerMergesExposeOuterTail()
{
    const std::vector<std::string> input = {
        "63",
          "63", "51", "76", "67", "52", "76", "68",
        "67",
          "63", "53", "76", "67", "54", "76", "68",
        "68"
    };
    const auto result = StructuredIfTailOptimizer::optimize(input);
    expectInstructions(
        result,
        {
            "63",
              "63", "51", "67", "52", "68",
            "67",
              "63", "53", "67", "54", "68",
            "68", "76"
        },
        "inner exposes outer"
    );
    expect(result.mergedIfCount == 3, "inner and outer merge count");
    expectCompleteMapping(result, input.size(), "inner exposes outer");
    expectOrigins(
        result,
        13,
        {
            {3,
             {{0, BranchArm::THEN}, {1, BranchArm::THEN}}},
            {6,
             {{0, BranchArm::THEN}, {1, BranchArm::ELSE}}},
            {11,
             {{0, BranchArm::ELSE}, {7, BranchArm::THEN}}},
            {14,
             {{0, BranchArm::ELSE}, {7, BranchArm::ELSE}}}
        },
        "nested guards on transitive hoist"
    );
    expect(
        tbc::validateInstructionRewritePlan(
            result.rewritePlan, input, result.instructions
        ),
        "transitive nested rewrite plan validation"
    );
}

void testSafetyBarriers()
{
    const std::vector<std::vector<std::string>> inputs = {
        {"63", "51", "6a", "67", "52", "6a", "68"},
        {"63", "51", "ab", "67", "52", "ab", "68"},
        {"63", "51", "#comment", "67", "52", "#comment", "68"},
        {"63", "51", "<self.x>", "67", "52", "<self.x>", "68"},
        // A barrier anywhere in either branch prevents moving a later suffix.
        {"63", "6a", "76", "67", "6a", "76", "68"},
        {"63", "ab", "76", "67", "ab", "76", "68"},
        {"63", "<unknown>", "76", "67", "<unknown>", "76", "68"}
    };

    for (size_t index = 0; index < inputs.size(); ++index) {
        const auto result = StructuredIfTailOptimizer::optimize(inputs[index]);
        expect(!result.changed, "unsafe barrier must not move");
        expect(result.skippedUnsafeTailCount == 1,
               "unsafe barrier should be reported");
        expectInstructions(
            result, inputs[index], "unsafe barrier " + std::to_string(index)
        );
    }
}

void testKnownPlaceholderEncodingMustBeWellFormed()
{
    StructuredIfTailOptions options;
    options.knownDataPlaceholderLabels.insert("self.x");

    const std::vector<std::vector<std::string>> inputs = {
        {"63", "51", "<self.y>", "67", "52", "<self.y>", "68"},
        {"63", "51", "a<self.x>b", "67", "52", "a<self.x>b", "68"},
        {"63", "51", "6a<self.x>", "67", "52", "6a<self.x>", "68"},
        {"63", "51", "ab<self.x>", "67", "52", "ab<self.x>", "68"}
    };
    for (const auto& input : inputs) {
        const auto result = StructuredIfTailOptimizer::optimize(input, options);
        expect(!result.changed, "unknown or malformed placeholder is a barrier");
        expectInstructions(result, input, "placeholder barrier");
    }
}

void testKnownDataPlaceholderCanMove()
{
    const std::vector<std::string> input = {
        "63", "51", "<self.x>", "67", "52", "<self.x>", "68"
    };
    StructuredIfTailOptions options;
    options.knownDataPlaceholderLabels.insert("self.x");
    const auto result = StructuredIfTailOptimizer::optimize(input, options);
    expectInstructions(
        result,
        {"63", "51", "67", "52", "68", "<self.x>"},
        "known data placeholder"
    );
    expect(result.newToOldOrigins.back() == std::vector<size_t>({2, 5}),
           "known placeholder origins");
}

void testPushedControlByteIsNotParsedAsControlFlow()
{
    const std::vector<std::string> input = {
        "63", "51", "0163", "67", "52", "0163", "68"
    };
    const auto result = StructuredIfTailOptimizer::optimize(input);
    expectInstructions(
        result,
        {"63", "51", "67", "52", "68", "0163"},
        "pushed control byte"
    );
}

void testRewritePlanValidatorRejectsCorruption()
{
    const std::vector<std::string> input = {
        "63", "51", "76", "67", "52", "76", "68"
    };
    const auto result = StructuredIfTailOptimizer::optimize(input);
    expect(
        tbc::validateInstructionRewritePlan(
            result.rewritePlan, input, result.instructions
        ),
        "baseline plan must validate"
    );

    auto wrongForward = result.rewritePlan;
    wrongForward.oldToNew[2] = 1;
    expect(
        !tbc::validateInstructionRewritePlan(
            wrongForward, input, result.instructions
        ),
        "validator rejects inconsistent forward mapping"
    );

    auto missingReverse = result.rewritePlan;
    missingReverse.newToOld[5].clear();
    expect(
        !tbc::validateInstructionRewritePlan(
            missingReverse, input, result.instructions
        ),
        "validator rejects missing reverse origins"
    );

    auto nonIfPredicate = result.rewritePlan;
    nonIfPredicate.newToOld[5][0].path[0].ifPC = 1;
    expect(
        !tbc::validateInstructionRewritePlan(
            nonIfPredicate, input, result.instructions
        ),
        "validator rejects a path predicate targeting non-IF"
    );

    auto conflictingPath = result.rewritePlan;
    conflictingPath.newToOld[5][0].path.push_back(
        BranchPredicate{0, BranchArm::ELSE}
    );
    expect(
        !tbc::validateInstructionRewritePlan(
            conflictingPath, input, result.instructions
        ),
        "validator rejects duplicate control region predicates"
    );

    auto duplicateOrigin = result.rewritePlan;
    duplicateOrigin.newToOld[5].push_back(
        duplicateOrigin.newToOld[5].front()
    );
    expect(
        !tbc::validateInstructionRewritePlan(
            duplicateOrigin, input, result.instructions
        ),
        "validator rejects a duplicate reverse origin"
    );

    tbc::InstructionRewritePlan deletionPlan;
    deletionPlan.oldToNew = {
        0, std::numeric_limits<size_t>::max()
    };
    deletionPlan.newToOld = {{{0, {}}}};
    expect(
        tbc::validateInstructionRewritePlan(
            deletionPlan, {"51", "75"}, {"51"}
        ),
        "validator accepts an explicitly deleted old instruction"
    );
}

void testMalformedControlFlowIsIdentity()
{
    const std::vector<std::vector<std::string>> malformed = {
        {"63", "51"},
        {"67", "51", "68"},
        {"63", "51", "67", "52"},
        {"63", "51", "68", "68"}
    };
    for (size_t index = 0; index < malformed.size(); ++index) {
        const auto result =
            StructuredIfTailOptimizer::optimize(malformed[index]);
        expect(!result.structurallyValid, "malformed flow must be reported");
        expect(!result.changed, "malformed flow must not change");
        expectInstructions(
            result, malformed[index], "malformed " + std::to_string(index)
        );
        expectCompleteMapping(
            result, malformed[index].size(), "malformed mapping"
        );
        expect(
            result.rewritePlanValid,
            "malformed flow returns a validated identity plan"
        );
        expect(
            tbc::validateInstructionRewritePlan(
                result.rewritePlan,
                malformed[index],
                result.instructions
            ),
            "malformed identity rewrite plan validates"
        );
        for (size_t pc = 0; pc < malformed[index].size(); ++pc) {
            expectOrigins(
                result,
                pc,
                {{pc, {}}},
                "malformed identity origin"
            );
        }
    }
}

} // namespace

int main()
{
    testSimpleTailAndMapping();
    testLongestMultiInstructionTail();
    testNoElseAndDifferentTailRemainUnchanged();
    testCompleteNestedNodeCanMove();
    testNestedNodeIsNeverPartiallyCut();
    testInnerMergesExposeOuterTail();
    testSafetyBarriers();
    testKnownDataPlaceholderCanMove();
    testKnownPlaceholderEncodingMustBeWellFormed();
    testPushedControlByteIsNotParsedAsControlFlow();
    testRewritePlanValidatorRejectsCorruption();
    testMalformedControlFlowIsIdentity();
    std::cout << "structured_if_tail_optimizer_test: PASS\n";
    return 0;
}
