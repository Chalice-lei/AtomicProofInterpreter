#include "src/bytecode_peephole_pass.h"

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/log/logger.h"
#include "src/pass/pass_context.h"

#ifdef ENABLE_DEBUGGER
#include "src/debugger/info/debug_info.h"
#endif

namespace
{
using BytData = std::pair<
    std::vector<std::string>,
    std::unordered_map<std::string, std::string>>;

std::string op(tbc::BytOpcode opcode)
{
    return tbc::opcodeToHex(opcode);
}

bool expectInstructions(
    const std::string& name,
    const std::vector<std::string>& actual,
    const std::vector<std::string>& expected
)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << name << ": unexpected instructions\n  expected:";
    for (const auto& instruction : expected) {
        std::cerr << ' ' << instruction;
    }
    std::cerr << "\n  actual:";
    for (const auto& instruction : actual) {
        std::cerr << ' ' << instruction;
    }
    std::cerr << '\n';
    return false;
}

std::vector<std::string> optimize(
    std::vector<std::string> instructions,
    std::unordered_map<std::string, size_t> selfPlaceholderLengths = {}
)
{
    PassContext context;
    auto bytecode = std::make_shared<BytData>(
        std::move(instructions),
        std::unordered_map<std::string, std::string>{}
    );
    context.set<BytData>("bytcode", bytecode);
    if (!selfPlaceholderLengths.empty()) {
        context.set<std::unordered_map<std::string, size_t>>(
            "self_placeholder_lengths",
            std::make_shared<std::unordered_map<std::string, size_t>>(
                std::move(selfPlaceholderLengths)
            )
        );
    }

    BytecodePeepholePass pass;
    pass.execute(context);
    return bytecode->first;
}

struct TypedOptimizationResult
{
    std::vector<std::string> instructions;
    tbc::BytecodeArtifact artifact;
};

TypedOptimizationResult optimizeCanonical(
    std::vector<std::string> instructions,
    std::unordered_map<std::string, size_t> selfPlaceholderLengths = {}
)
{
    PassContext context;
    auto bytecode = std::make_shared<BytData>(
        std::move(instructions),
        std::unordered_map<std::string, std::string>{}
    );
    context.set<BytData>("bytcode", bytecode);

    auto artifact = std::make_shared<tbc::BytecodeArtifact>(
        tbc::LegacyBytecodeAdapter::import(*bytecode)
    );
    artifact->format = tbc::ArtifactFormat::CanonicalV2;
    for (auto& instruction : artifact->lockingScript) {
        auto* placeholder =
            std::get_if<tbc::PlaceholderPushInstruction>(
                &instruction.body
            );
        if (!placeholder) {
            continue;
        }
        for (const auto& [base, length] : selfPlaceholderLengths) {
            if (placeholder->label == base ||
                placeholder->label == base + std::to_string(length)) {
                placeholder->expectedPayloadSize = length;
                break;
            }
        }
    }
    context.set<tbc::BytecodeArtifact>("bytecode_artifact", artifact);
    if (!selfPlaceholderLengths.empty()) {
        context.set<std::unordered_map<std::string, size_t>>(
            "self_placeholder_lengths",
            std::make_shared<std::unordered_map<std::string, size_t>>(
                std::move(selfPlaceholderLengths)
            )
        );
    }

    BytecodePeepholePass pass;
    pass.execute(context);
    return {bytecode->first, *artifact};
}

tbc::BytecodeInstruction typedInstruction(
    tbc::InstructionId id,
    tbc::InstructionBody body,
    tbc::ScriptRegion region,
    std::string encoding
)
{
    tbc::BytecodeInstruction instruction;
    instruction.id = id;
    instruction.body = std::move(body);
    instruction.region = region;
    instruction.origins = {id};
    instruction.legacyEncoding = std::move(encoding);
    return instruction;
}

std::vector<std::string> optimizeTyped(
    std::vector<tbc::BytecodeInstruction> instructions
)
{
    PassContext context;
    auto artifact = std::make_shared<tbc::BytecodeArtifact>();
    artifact->format = tbc::ArtifactFormat::LegacyV1;
    artifact->lockingScript = std::move(instructions);
    const auto legacy = tbc::LegacyBytecodeAdapter::exportPreserving(
        *artifact
    );
    auto bytecode = std::make_shared<BytData>(legacy);
    context.set<BytData>("bytcode", bytecode);
    context.set<tbc::BytecodeArtifact>("bytecode_artifact", artifact);

    BytecodePeepholePass pass;
    pass.execute(context);
    return bytecode->first;
}

bool runRewriteTests()
{
    bool ok = true;

    // Verify fusion uses the four consensus-defined VERIFY variants.
    const std::pair<tbc::BytOpcode, tbc::BytOpcode> verifyCases[] = {
        {tbc::BytOpcode::OP_EQUAL, tbc::BytOpcode::OP_EQUALVERIFY},
        {tbc::BytOpcode::OP_NUMEQUAL,
         tbc::BytOpcode::OP_NUMEQUALVERIFY},
        {tbc::BytOpcode::OP_CHECKSIG,
         tbc::BytOpcode::OP_CHECKSIGVERIFY},
        {tbc::BytOpcode::OP_CHECKMULTISIG,
         tbc::BytOpcode::OP_CHECKMULTISIGVERIFY},
    };
    for (const auto& [producer, fused] : verifyCases) {
        ok &= expectInstructions(
            "verify fusion",
            optimize({op(producer), op(tbc::BytOpcode::OP_VERIFY)}),
            {op(fused)}
        );
    }
    ok &= expectInstructions(
        "constant true VERIFY keeps nonempty script",
        optimize({op(tbc::BytOpcode::OP_1), op(tbc::BytOpcode::OP_VERIFY)}),
        {op(tbc::BytOpcode::OP_1), op(tbc::BytOpcode::OP_VERIFY)}
    );
    ok &= expectInstructions(
        "constant true VERIFY",
        optimize({op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_VERIFY),
                  op(tbc::BytOpcode::OP_1)}),
        {op(tbc::BytOpcode::OP_1)}
    );
    ok &= expectInstructions(
        "constant true VERIFY observes final nonempty result",
        optimize({op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_VERIFY),
                  op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_DROP)}),
        {op(tbc::BytOpcode::OP_1),
         op(tbc::BytOpcode::OP_VERIFY),
         op(tbc::BytOpcode::OP_1),
         op(tbc::BytOpcode::OP_DROP)}
    );

    // NOT fusion is intentionally guarded by an adjacent canonical-bool
    // producer. A naked NOT may consume arbitrary numeric/byte input.
    ok &= expectInstructions(
        "canonical bool NOT IF",
        optimize({op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_NOT),
                  op(tbc::BytOpcode::OP_IF),
                  op(tbc::BytOpcode::OP_ENDIF)}),
        {op(tbc::BytOpcode::OP_EQUAL),
         op(tbc::BytOpcode::OP_NOTIF),
         op(tbc::BytOpcode::OP_ENDIF)}
    );
    ok &= expectInstructions(
        "canonical bool NOT NOTIF",
        optimize({op(tbc::BytOpcode::OP_NUMEQUAL),
                  op(tbc::BytOpcode::OP_NOT),
                  op(tbc::BytOpcode::OP_NOTIF),
                  op(tbc::BytOpcode::OP_ENDIF)}),
        {op(tbc::BytOpcode::OP_NUMEQUAL),
         op(tbc::BytOpcode::OP_IF),
         op(tbc::BytOpcode::OP_ENDIF)}
    );
    ok &= expectInstructions(
        "unguarded NOT IF",
        optimize({op(tbc::BytOpcode::OP_NOT),
                  op(tbc::BytOpcode::OP_IF),
                  op(tbc::BytOpcode::OP_ENDIF)}),
        {op(tbc::BytOpcode::OP_NOT),
         op(tbc::BytOpcode::OP_IF),
         op(tbc::BytOpcode::OP_ENDIF)}
    );

    // Safe stack canonicalizations preserve both successful behavior and
    // stack-underflow behavior.
    const struct
    {
        const char* name;
        std::vector<std::string> input;
        std::vector<std::string> expected;
    } stackCases[] = {
        {"SWAP DROP",
         {op(tbc::BytOpcode::OP_SWAP), op(tbc::BytOpcode::OP_DROP)},
         {op(tbc::BytOpcode::OP_NIP)}},
        {"OVER OVER",
         {op(tbc::BytOpcode::OP_OVER), op(tbc::BytOpcode::OP_OVER)},
         {op(tbc::BytOpcode::OP_2DUP)}},
        {"0 PICK",
         {op(tbc::BytOpcode::OP_0), op(tbc::BytOpcode::OP_PICK)},
         {op(tbc::BytOpcode::OP_DUP)}},
        {"1 PICK",
         {op(tbc::BytOpcode::OP_1), op(tbc::BytOpcode::OP_PICK)},
         {op(tbc::BytOpcode::OP_OVER)}},
        {"1 ROLL",
         {op(tbc::BytOpcode::OP_1), op(tbc::BytOpcode::OP_ROLL)},
         {op(tbc::BytOpcode::OP_SWAP)}},
        {"2 ROLL",
         {op(tbc::BytOpcode::OP_2), op(tbc::BytOpcode::OP_ROLL)},
         {op(tbc::BytOpcode::OP_ROT)}},
    };
    for (const auto& test : stackCases) {
        ok &= expectInstructions(
            test.name, optimize(test.input), test.expected
        );
    }

    const struct
    {
        const char* name;
        tbc::BytOpcode constant;
        tbc::BytOpcode arithmetic;
        tbc::BytOpcode expected;
    } numericCases[] = {
        {"1 ADD",
         tbc::BytOpcode::OP_1,
         tbc::BytOpcode::OP_ADD,
         tbc::BytOpcode::OP_1ADD},
        {"1 SUB",
         tbc::BytOpcode::OP_1,
         tbc::BytOpcode::OP_SUB,
         tbc::BytOpcode::OP_1SUB},
        {"-1 ADD",
         tbc::BytOpcode::OP_1NEGATE,
         tbc::BytOpcode::OP_ADD,
         tbc::BytOpcode::OP_1SUB},
        {"-1 SUB",
         tbc::BytOpcode::OP_1NEGATE,
         tbc::BytOpcode::OP_SUB,
         tbc::BytOpcode::OP_1ADD},
    };
    for (const auto& test : numericCases) {
        ok &= expectInstructions(
            test.name,
            optimize({op(test.constant), op(test.arithmetic)}),
            {op(test.expected)}
        );
    }

    ok &= expectInstructions(
        "adjacent data push reuse",
        optimize({"02aabb", "02aabb"}),
        {"02aabb", op(tbc::BytOpcode::OP_DUP)}
    );
    const auto canonicalSmallInteger =
        optimizeCanonical({"0101", "0101"});
    ok &= expectInstructions(
        "canonical small-integer duplicate is not strictly smaller",
        canonicalSmallInteger.instructions,
        {"0101", "0101"}
    );
    if (canonicalSmallInteger.artifact.format !=
            tbc::ArtifactFormat::CanonicalV2 ||
        !std::holds_alternative<tbc::PushDataInstruction>(
            canonicalSmallInteger.artifact.lockingScript[0].body
        )) {
        std::cerr << "canonical no-op optimization corrupted typed pushes\n";
        ok = false;
    }
    ok &= expectInstructions(
        "canonical concrete one-byte non-small push reuse",
        optimizeCanonical({"0180", "0180"}).instructions,
        {"0180", op(tbc::BytOpcode::OP_DUP)}
    );
    ok &= expectInstructions(
        "canonical multi-byte push reuse",
        optimizeCanonical({"02aabb", "02aabb"}).instructions,
        {"02aabb", op(tbc::BytOpcode::OP_DUP)}
    );
    ok &= expectInstructions(
        "canonical unknown one-byte placeholder is not provably smaller",
        optimizeCanonical(
            {"<self.value1>", "<self.value1>"},
            {{"self.value", 1}}
        ).instructions,
        {"<self.value1>", "<self.value1>"}
    );
    ok &= expectInstructions(
        "canonical multi-byte placeholder reuse",
        optimizeCanonical(
            {"<self.value20>", "<self.value20>"},
            {{"self.value", 20}}
        ).instructions,
        {"<self.value20>", op(tbc::BytOpcode::OP_DUP)}
    );
    ok &= expectInstructions(
        "malformed data push is not reused",
        optimize({"02aa", "02aa"}),
        {"02aa", "02aa"}
    );
    ok &= expectInstructions(
        "malformed data push is not constant-folded",
        optimize({"02aa", "02aa", op(tbc::BytOpcode::OP_EQUAL)}),
        {"02aa", "02aa", op(tbc::BytOpcode::OP_EQUAL)}
    );
    ok &= expectInstructions(
        "identical pushed constants fold before duplicate reuse",
        optimize({"02aabb", "02aabb", op(tbc::BytOpcode::OP_EQUAL)}),
        {op(tbc::BytOpcode::OP_1)}
    );
    ok &= expectInstructions(
        "different pushed constants fold",
        optimize({"02aabb", "02aabc", op(tbc::BytOpcode::OP_EQUAL)}),
        {op(tbc::BytOpcode::OP_0)}
    );
    ok &= expectInstructions(
        "equivalent push encodings compare payloads",
        optimize({"01aa", "4c01aa", op(tbc::BytOpcode::OP_EQUAL)}),
        {op(tbc::BytOpcode::OP_1)}
    );
    ok &= expectInstructions(
        "push DUP EQUAL is constant true",
        optimize({"02aabb",
                  op(tbc::BytOpcode::OP_DUP),
                  op(tbc::BytOpcode::OP_EQUAL)}),
        {op(tbc::BytOpcode::OP_1)}
    );
    ok &= expectInstructions(
        "push DUP DROP retains the proven value",
        optimize({"02aabb",
                  op(tbc::BytOpcode::OP_DUP),
                  op(tbc::BytOpcode::OP_DROP)}),
        {"02aabb"}
    );
    ok &= expectInstructions(
        "malformed push does not prove DUP DROP depth",
        optimize({"02aa",
                  op(tbc::BytOpcode::OP_DUP),
                  op(tbc::BytOpcode::OP_DROP)}),
        {"02aa",
         op(tbc::BytOpcode::OP_DUP),
         op(tbc::BytOpcode::OP_DROP)}
    );
    ok &= expectInstructions(
        "push DUP 2DROP removes a balanced temporary",
        optimize({"02aabb",
                  op(tbc::BytOpcode::OP_DUP),
                  op(tbc::BytOpcode::OP_2DROP)}),
        {}
    );

    const std::string pushData1 = "4c4c" + std::string(76 * 2, 'a');
    const std::string pushData2 = "4d0001" + std::string(256 * 2, 'b');
    const std::string pushData4 =
        "4e00000100" + std::string(65536 * 2, 'c');
    for (const auto& [name, encoding] :
         std::vector<std::pair<std::string, std::string>>{
             {"PUSHDATA1 boundary reuse", pushData1},
             {"PUSHDATA2 boundary reuse", pushData2},
             {"PUSHDATA4 boundary reuse", pushData4}}) {
        ok &= expectInstructions(
            name,
            optimize({encoding, encoding}),
            {encoding, op(tbc::BytOpcode::OP_DUP)}
        );
    }
    for (const auto& [name, malformed] :
         std::vector<std::pair<std::string, std::string>>{
             {"short PUSHDATA1", "4c02aa"},
             {"trailing PUSHDATA1", "4c01aabb"},
             {"short PUSHDATA2", "4d0200aa"},
             {"trailing PUSHDATA2", "4d0100aabb"},
             {"short PUSHDATA4", "4e02000000aa"},
             {"trailing PUSHDATA4", "4e01000000aabb"}}) {
        ok &= expectInstructions(
            name + " is not reused",
            optimize({malformed, malformed}),
            {malformed, malformed}
        );
    }
    for (const auto& [name, malformed] :
         std::vector<std::pair<std::string, std::string>>{
             {"truncated direct push", "01"},
             {"truncated direct data", "02aa"},
             {"short PUSHDATA1 before DROP", "4c02aa"},
             {"small opcode with trailing byte", "5175"}}) {
        ok &= expectInstructions(
            name + " is not deleted before DROP",
            optimize({malformed, op(tbc::BytOpcode::OP_DROP)}),
            {malformed, op(tbc::BytOpcode::OP_DROP)}
        );
    }
    ok &= expectInstructions(
        "complete push is deleted before DROP",
        optimize({"02aabb", op(tbc::BytOpcode::OP_DROP)}),
        {}
    );
    ok &= expectInstructions(
        "known placeholder reuse",
        optimize(
            {"<self.value20>", "<self.value20>"},
            {{"self.value", 20}}
        ),
        {"<self.value20>", op(tbc::BytOpcode::OP_DUP)}
    );
    ok &= expectInstructions(
        "unknown placeholder is not reused",
        optimize({"<self.value20>", "<self.value20>"}),
        {"<self.value20>", "<self.value20>"}
    );
    ok &= expectInstructions(
        "comment blocks three-instruction constant folding",
        optimize({"01aa",
                  "#barrier",
                  "01aa",
                  op(tbc::BytOpcode::OP_EQUAL)}),
        {"01aa", "#barrier", "01aa", op(tbc::BytOpcode::OP_EQUAL)}
    );

    // Pair rules without a proven entry depth must preserve underflow.
    const struct
    {
        const char* name;
        std::vector<std::string> input;
        std::vector<std::string> expected;
    } existingCases[] = {
        {"push DROP",
         {op(tbc::BytOpcode::OP_1), op(tbc::BytOpcode::OP_DROP)},
         {}},
        {"DUP DROP",
         {op(tbc::BytOpcode::OP_DUP), op(tbc::BytOpcode::OP_DROP)},
         {op(tbc::BytOpcode::OP_DUP), op(tbc::BytOpcode::OP_DROP)}},
        {"SWAP SWAP",
         {op(tbc::BytOpcode::OP_SWAP), op(tbc::BytOpcode::OP_SWAP)},
         {op(tbc::BytOpcode::OP_SWAP), op(tbc::BytOpcode::OP_SWAP)}},
        {"NOT NOT",
         {op(tbc::BytOpcode::OP_NOT), op(tbc::BytOpcode::OP_NOT)},
         {op(tbc::BytOpcode::OP_NOT), op(tbc::BytOpcode::OP_NOT)}},
        {"DROP DROP",
         {op(tbc::BytOpcode::OP_DROP), op(tbc::BytOpcode::OP_DROP)},
         {op(tbc::BytOpcode::OP_2DROP)}},
        {"0 EQUAL",
         {op(tbc::BytOpcode::OP_0), op(tbc::BytOpcode::OP_EQUAL)},
         {op(tbc::BytOpcode::OP_0), op(tbc::BytOpcode::OP_EQUAL)}},
        {"0 NUMEQUAL",
         {op(tbc::BytOpcode::OP_0), op(tbc::BytOpcode::OP_NUMEQUAL)},
         {op(tbc::BytOpcode::OP_0), op(tbc::BytOpcode::OP_NUMEQUAL)}},
    };
    for (const auto& test : existingCases) {
        ok &= expectInstructions(
            test.name, optimize(test.input), test.expected
        );
    }
    ok &= expectInstructions(
        "canonical bool triple NOT NOT",
        optimize({op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_NOT),
                  op(tbc::BytOpcode::OP_NOT)}),
        {op(tbc::BytOpcode::OP_EQUAL)}
    );
    ok &= expectInstructions(
        "canonical bool equals true",
        optimize({op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_EQUAL)}),
        {op(tbc::BytOpcode::OP_EQUAL)}
    );
    ok &= expectInstructions(
        "canonical bool equals false",
        optimize({op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_0),
                  op(tbc::BytOpcode::OP_EQUAL)}),
        {op(tbc::BytOpcode::OP_EQUAL), op(tbc::BytOpcode::OP_NOT)}
    );
    ok &= expectInstructions(
        "canonical bool numeric equals true",
        optimize({op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_NUMEQUAL)}),
        {op(tbc::BytOpcode::OP_EQUAL)}
    );
    ok &= expectInstructions(
        "canonical bool numeric equals false",
        optimize({op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_0),
                  op(tbc::BytOpcode::OP_NUMEQUAL)}),
        {op(tbc::BytOpcode::OP_EQUAL), op(tbc::BytOpcode::OP_NOT)}
    );
    ok &= expectInstructions(
        "canonical bool numeric not-equals false",
        optimize({op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_0),
                  op(tbc::BytOpcode::OP_NUMNOTEQUAL)}),
        {op(tbc::BytOpcode::OP_EQUAL)}
    );
    ok &= expectInstructions(
        "canonical bool numeric not-equals true",
        optimize({op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_NUMNOTEQUAL)}),
        {op(tbc::BytOpcode::OP_EQUAL), op(tbc::BytOpcode::OP_NOT)}
    );
    ok &= expectInstructions(
        "naked DUP DROP retains underflow behavior",
        optimize({op(tbc::BytOpcode::OP_DUP),
                  op(tbc::BytOpcode::OP_DROP),
                  op(tbc::BytOpcode::OP_1)}),
        {op(tbc::BytOpcode::OP_DUP),
         op(tbc::BytOpcode::OP_DROP),
         op(tbc::BytOpcode::OP_1)}
    );
    ok &= expectInstructions(
        "one pushed item cannot prove SWAP SWAP",
        optimize({op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_SWAP),
                  op(tbc::BytOpcode::OP_SWAP)}),
        {op(tbc::BytOpcode::OP_1),
         op(tbc::BytOpcode::OP_SWAP),
         op(tbc::BytOpcode::OP_SWAP)}
    );
    ok &= expectInstructions(
        "two pushed items prove SWAP SWAP",
        optimize({op(tbc::BytOpcode::OP_7),
                  op(tbc::BytOpcode::OP_3),
                  op(tbc::BytOpcode::OP_SWAP),
                  op(tbc::BytOpcode::OP_SWAP)}),
        {op(tbc::BytOpcode::OP_7), op(tbc::BytOpcode::OP_3)}
    );
    ok &= expectInstructions(
        "canonical bool 0 EQUAL",
        optimize({op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_0),
                  op(tbc::BytOpcode::OP_EQUAL)}),
        {op(tbc::BytOpcode::OP_0)}
    );
    ok &= expectInstructions(
        "canonical bool 0 NUMEQUAL",
        optimize({op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_0),
                  op(tbc::BytOpcode::OP_NUMEQUAL)}),
        {op(tbc::BytOpcode::OP_1), op(tbc::BytOpcode::OP_NOT)}
    );

    // OP_0 OP_ROLL is an identity only when another main-stack item exists;
    // without stack-depth proof it must remain unchanged.
    ok &= expectInstructions(
        "0 ROLL remains guarded",
        optimize({op(tbc::BytOpcode::OP_0), op(tbc::BytOpcode::OP_ROLL)}),
        {op(tbc::BytOpcode::OP_0), op(tbc::BytOpcode::OP_ROLL)}
    );
    ok &= expectInstructions(
        "non-minimal 1 PICK remains unchanged",
        optimize({"0101", op(tbc::BytOpcode::OP_PICK)}),
        {"0101", op(tbc::BytOpcode::OP_PICK)}
    );

    // Placeholders remain hard barriers and cannot be inspected as opcodes.
    ok &= expectInstructions(
        "placeholder barrier",
        optimize({op(tbc::BytOpcode::OP_EQUAL),
                  "<self.value20>",
                  op(tbc::BytOpcode::OP_VERIFY)}),
        {op(tbc::BytOpcode::OP_EQUAL),
         "<self.value20>",
         op(tbc::BytOpcode::OP_VERIFY)}
    );

    // The pass invokes the structured optimizer after local normalization.
    ok &= expectInstructions(
        "structured common tail",
        optimize({op(tbc::BytOpcode::OP_IF),
                  op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_ELSE),
                  op(tbc::BytOpcode::OP_2),
                  op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_ENDIF)}),
        {op(tbc::BytOpcode::OP_IF),
         op(tbc::BytOpcode::OP_1),
         op(tbc::BytOpcode::OP_ELSE),
         op(tbc::BytOpcode::OP_2),
         op(tbc::BytOpcode::OP_ENDIF),
         op(tbc::BytOpcode::OP_EQUAL)}
    );
    ok &= expectInstructions(
        "structured known fixed placeholder tail",
        optimize({op(tbc::BytOpcode::OP_IF),
                  op(tbc::BytOpcode::OP_1),
                  "<self.value20>",
                  op(tbc::BytOpcode::OP_ELSE),
                  op(tbc::BytOpcode::OP_2),
                  "<self.value20>",
                  op(tbc::BytOpcode::OP_ENDIF)},
                 {{"self.value", 20}}),
        {op(tbc::BytOpcode::OP_IF),
         op(tbc::BytOpcode::OP_1),
         op(tbc::BytOpcode::OP_ELSE),
         op(tbc::BytOpcode::OP_2),
         op(tbc::BytOpcode::OP_ENDIF),
         "<self.value20>"}
    );
    ok &= expectInstructions(
        "immutable suffix blocks local rewrite",
        optimize({op(tbc::BytOpcode::OP_RETURN),
                  op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_DROP)}),
        {op(tbc::BytOpcode::OP_RETURN),
         op(tbc::BytOpcode::OP_1),
         op(tbc::BytOpcode::OP_DROP)}
    );
    ok &= expectInstructions(
        "immutable suffix blocks duplicate push reuse",
        optimize({op(tbc::BytOpcode::OP_RETURN), "02aabb", "02aabb"}),
        {op(tbc::BytOpcode::OP_RETURN), "02aabb", "02aabb"}
    );
    ok &= expectInstructions(
        "immutable suffix blocks structured rewrite",
        optimize({op(tbc::BytOpcode::OP_RETURN),
                  op(tbc::BytOpcode::OP_IF),
                  op(tbc::BytOpcode::OP_1),
                  op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_ELSE),
                  op(tbc::BytOpcode::OP_2),
                  op(tbc::BytOpcode::OP_EQUAL),
                  op(tbc::BytOpcode::OP_ENDIF)}),
        {op(tbc::BytOpcode::OP_RETURN),
         op(tbc::BytOpcode::OP_IF),
         op(tbc::BytOpcode::OP_1),
         op(tbc::BytOpcode::OP_EQUAL),
         op(tbc::BytOpcode::OP_ELSE),
         op(tbc::BytOpcode::OP_2),
         op(tbc::BytOpcode::OP_EQUAL),
         op(tbc::BytOpcode::OP_ENDIF)}
    );
    ok &= expectInstructions(
        "typed RawSuffix OP_RETURN byte is not an executable boundary",
        optimizeTyped({
            typedInstruction(
                0,
                tbc::OpcodeInstruction{tbc::BytOpcode::OP_RETURN},
                tbc::ScriptRegion::Executable,
                "6a"
            ),
            typedInstruction(
                1,
                tbc::RawSuffixInstruction{"51"},
                tbc::ScriptRegion::ImmutableSuffix,
                "51"
            ),
            typedInstruction(
                2,
                tbc::RawSuffixInstruction{"6a"},
                tbc::ScriptRegion::ImmutableSuffix,
                "6a"
            ),
            typedInstruction(
                3,
                tbc::RawSuffixInstruction{"aabb"},
                tbc::ScriptRegion::ImmutableSuffix,
                "aabb"
            ),
        }),
        {"6a", "51", "6a", "aabb"}
    );
    ok &= expectInstructions(
        "typed suffix push DROP before raw 6a stays immutable",
        optimizeTyped({
            typedInstruction(
                0,
                tbc::OpcodeInstruction{tbc::BytOpcode::OP_RETURN},
                tbc::ScriptRegion::Executable,
                "6a"
            ),
            typedInstruction(
                1,
                tbc::RawSuffixInstruction{"51"},
                tbc::ScriptRegion::ImmutableSuffix,
                "51"
            ),
            typedInstruction(
                2,
                tbc::RawSuffixInstruction{"75"},
                tbc::ScriptRegion::ImmutableSuffix,
                "75"
            ),
            typedInstruction(
                3,
                tbc::RawSuffixInstruction{"6a"},
                tbc::ScriptRegion::ImmutableSuffix,
                "6a"
            ),
            typedInstruction(
                4,
                tbc::RawSuffixInstruction{"aabb"},
                tbc::ScriptRegion::ImmutableSuffix,
                "aabb"
            ),
        }),
        {"6a", "51", "75", "6a", "aabb"}
    );

    return ok;
}

#ifdef ENABLE_DEBUGGER
bool runProvenanceTest()
{
    PassContext context;
    auto bytecode = std::make_shared<BytData>(
        std::vector<std::string>{op(tbc::BytOpcode::OP_EQUAL),
                                 op(tbc::BytOpcode::OP_VERIFY),
                                 op(tbc::BytOpcode::OP_RETURN)},
        std::unordered_map<std::string, std::string>{}
    );
    context.set<BytData>("bytcode", bytecode);

    auto debugInfo = std::make_shared<apc_debug::DebugInfo>();
    debugInfo->sourceFilename = "peephole_test.ct";
    debugInfo->addInstruction(
        0,
        op(tbc::BytOpcode::OP_EQUAL),
        "",
        apc_debug::SourceLocation("peephole_test.ct", 10, 1)
    );
    debugInfo->addInstruction(
        1,
        op(tbc::BytOpcode::OP_VERIFY),
        "",
        apc_debug::SourceLocation("peephole_test.ct", 11, 1)
    );
    debugInfo->addInstruction(
        2,
        op(tbc::BytOpcode::OP_RETURN),
        "",
        apc_debug::SourceLocation("peephole_test.ct", 12, 1)
    );
    context.set<apc_debug::DebugInfo>("debug_info", debugInfo);

    BytecodePeepholePass pass;
    pass.execute(context);

    bool ok = expectInstructions(
        "provenance bytecode",
        bytecode->first,
        {op(tbc::BytOpcode::OP_EQUALVERIFY),
         op(tbc::BytOpcode::OP_RETURN)}
    );
    const auto fusedLoc = debugInfo->getSourceLocation(0);
    const auto returnLoc = debugInfo->getSourceLocation(1);
    if (fusedLoc.line != 11 || returnLoc.line != 12) {
        std::cerr << "replacement provenance must come from the second "
                     "instruction: got lines "
                  << fusedLoc.line << ", " << returnLoc.line << '\n';
        ok = false;
    }
    auto fusedInstruction = debugInfo->instructions.find(0);
    if (fusedInstruction == debugInfo->instructions.end() ||
        fusedInstruction->second.opcode !=
            op(tbc::BytOpcode::OP_EQUALVERIFY)) {
        std::cerr << "debug instruction opcode was not synchronized\n";
        ok = false;
    }
    return ok;
}

bool runDuplicatePushProvenanceTest()
{
    PassContext context;
    const std::vector<std::string> original{
        "02aabb", "02aabb", op(tbc::BytOpcode::OP_RETURN)
    };
    auto bytecode = std::make_shared<BytData>(
        original, std::unordered_map<std::string, std::string>{}
    );
    context.set<BytData>("bytcode", bytecode);

    auto debugInfo = std::make_shared<apc_debug::DebugInfo>();
    debugInfo->sourceFilename = "duplicate_push.ct";
    for (size_t pc = 0; pc < original.size(); ++pc) {
        debugInfo->addInstruction(
            pc,
            original[pc],
            "",
            apc_debug::SourceLocation(
                "duplicate_push.ct", static_cast<int>(pc + 20), 1
            )
        );
    }
    context.set<apc_debug::DebugInfo>("debug_info", debugInfo);

    BytecodePeepholePass pass;
    pass.execute(context);
    bool ok = expectInstructions(
        "duplicate push provenance bytecode",
        bytecode->first,
        {"02aabb",
         op(tbc::BytOpcode::OP_DUP),
         op(tbc::BytOpcode::OP_RETURN)}
    );
    for (size_t pc = 0; pc < original.size(); ++pc) {
        const auto location = debugInfo->getSourceLocation(pc);
        if (location.line != pc + 20) {
            std::cerr << "duplicate push provenance changed at PC " << pc
                      << '\n';
            ok = false;
        }
    }
    const auto duplicate = debugInfo->instructions.find(1);
    if (duplicate == debugInfo->instructions.end() ||
        duplicate->second.opcode != op(tbc::BytOpcode::OP_DUP)) {
        std::cerr << "duplicate push debug opcode was not synchronized\n";
        ok = false;
    }
    return ok;
}

bool runTripleProvenanceTest()
{
    auto checkCase = [](const std::string& name,
                        const std::vector<std::string>& original,
                        const std::string& expected,
                        size_t expectedOrigin) {
        PassContext context;
        auto bytecode = std::make_shared<BytData>(
            original, std::unordered_map<std::string, std::string>{}
        );
        context.set<BytData>("bytcode", bytecode);

        auto debugInfo = std::make_shared<apc_debug::DebugInfo>();
        debugInfo->sourceFilename = "triple_provenance.ct";
        for (size_t pc = 0; pc < original.size(); ++pc) {
            debugInfo->addInstruction(
                pc,
                original[pc],
                "",
                apc_debug::SourceLocation(
                    "triple_provenance.ct", pc + 40, 1
                )
            );
        }
        context.set<apc_debug::DebugInfo>("debug_info", debugInfo);

        BytecodePeepholePass pass;
        pass.execute(context);
        bool ok = expectInstructions(name, bytecode->first, {expected});
        const auto location = debugInfo->getSourceLocation(0);
        if (location.line != expectedOrigin + 40) {
            std::cerr << name << ": unexpected provenance line "
                      << location.line << '\n';
            ok = false;
        }
        const auto instruction = debugInfo->instructions.find(0);
        if (instruction == debugInfo->instructions.end() ||
            instruction->second.opcode != expected) {
            std::cerr << name << ": debug opcode was not synchronized\n";
            ok = false;
        }
        return ok;
    };

    bool ok = checkCase(
        "constant triple provenance",
        {"02aabb", "02aabb", op(tbc::BytOpcode::OP_EQUAL)},
        op(tbc::BytOpcode::OP_1),
        2
    );
    ok &= checkCase(
        "canonical producer provenance",
        {op(tbc::BytOpcode::OP_EQUAL),
         op(tbc::BytOpcode::OP_NOT),
         op(tbc::BytOpcode::OP_NOT)},
        op(tbc::BytOpcode::OP_EQUAL),
        0
    );
    return ok;
}

bool runStructuredDebugProvenanceTest()
{
    PassContext context;
    const std::vector<std::string> original{
        op(tbc::BytOpcode::OP_IF),
        op(tbc::BytOpcode::OP_1),
        op(tbc::BytOpcode::OP_EQUAL),
        op(tbc::BytOpcode::OP_ELSE),
        op(tbc::BytOpcode::OP_2),
        op(tbc::BytOpcode::OP_EQUAL),
        op(tbc::BytOpcode::OP_ENDIF)
    };
    auto bytecode = std::make_shared<BytData>(
        original, std::unordered_map<std::string, std::string>{}
    );
    context.set<BytData>("bytcode", bytecode);

    auto debugInfo = std::make_shared<apc_debug::DebugInfo>();
    debugInfo->sourceFilename = "structured_debug_guard.ct";
    for (size_t pc = 0; pc < original.size(); ++pc) {
        debugInfo->addInstruction(
            pc,
            original[pc],
            "",
            apc_debug::SourceLocation(
                "structured_debug_guard.ct", static_cast<int>(pc + 1), 1
            )
        );
    }
    context.set<apc_debug::DebugInfo>("debug_info", debugInfo);

    BytecodePeepholePass pass;
    pass.execute(context);
    bool ok = expectInstructions(
        "structured optimization with debug provenance",
        bytecode->first,
        {op(tbc::BytOpcode::OP_IF),
         op(tbc::BytOpcode::OP_1),
         op(tbc::BytOpcode::OP_ELSE),
         op(tbc::BytOpcode::OP_2),
         op(tbc::BytOpcode::OP_ENDIF),
         op(tbc::BytOpcode::OP_EQUAL)}
    );
    const auto origins = debugInfo->getOriginsForPC(5);
    if (origins.size() != 2 || origins[0].location.line != 3 ||
        origins[1].location.line != 6 || origins[0].path.size() != 1 ||
        origins[1].path.size() != 1 || origins[0].path[0].region != 0 ||
        origins[1].path[0].region != 0 ||
        origins[0].path[0].arm != apc_debug::BranchArm::Then ||
        origins[1].path[0].arm != apc_debug::BranchArm::Else) {
        std::cerr << "structured tail did not retain both branch origins\n";
        ok = false;
    }
    apc_debug::BranchTrace thenTrace{{0, apc_debug::BranchArm::Then}};
    apc_debug::BranchTrace elseTrace{{0, apc_debug::BranchArm::Else}};
    if (debugInfo->getSourceLocation(5, thenTrace).line != 3 ||
        debugInfo->getSourceLocation(5, elseTrace).line != 6 ||
        !debugInfo->hasActiveSourceOrigin(
            5, "structured_debug_guard.ct", 3, thenTrace
        ) ||
        debugInfo->hasActiveSourceOrigin(
            5, "structured_debug_guard.ct", 6, thenTrace
        )) {
        std::cerr << "structured tail branch resolution is incorrect\n";
        ok = false;
    }

    PassContext constantContext;
    const std::vector<std::string> constantVerify{
        op(tbc::BytOpcode::OP_1),
        op(tbc::BytOpcode::OP_VERIFY),
        op(tbc::BytOpcode::OP_1)
    };
    auto constantBytecode = std::make_shared<BytData>(
        constantVerify, std::unordered_map<std::string, std::string>{}
    );
    constantContext.set<BytData>("bytcode", constantBytecode);
    auto constantDebugInfo = std::make_shared<apc_debug::DebugInfo>();
    constantDebugInfo->sourceFilename = "constant_verify_debug_guard.ct";
    for (size_t pc = 0; pc < constantVerify.size(); ++pc) {
        constantDebugInfo->addInstruction(
            pc,
            constantVerify[pc],
            "",
            apc_debug::SourceLocation(
                "constant_verify_debug_guard.ct",
                static_cast<int>(pc + 1),
                1
            )
        );
    }
    constantContext.set<apc_debug::DebugInfo>(
        "debug_info", constantDebugInfo
    );
    pass.execute(constantContext);
    ok &= expectInstructions(
        "constant VERIFY retained with debug info",
        constantBytecode->first,
        constantVerify
    );
    return ok;
}
#endif
} // namespace

int main()
{
    Logger::GetInstance().Initialize(LogLevel::NONE, "", false);

    bool ok = runRewriteTests();
#ifdef ENABLE_DEBUGGER
    ok &= runProvenanceTest();
    ok &= runDuplicatePushProvenanceTest();
    ok &= runTripleProvenanceTest();
    ok &= runStructuredDebugProvenanceTest();
#endif

    if (!ok) {
        return 1;
    }
    std::cout << "Bytecode peephole pass tests passed.\n";
    return 0;
}
