#include "runtime_self_test.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../ast/ast.h"
#include "environment.h"
#include "runtime_context.h"
#include "runtime_error.h"
#include "runtime_slot.h"
#include "runtime_value.h"

namespace apc_interpreter
{
namespace
{

struct SelfTestState
{
    std::ostream& err;
    int failures = 0;

    void expect(bool condition, const std::string& message)
    {
        if (!condition) {
            ++failures;
            err << "FAIL: " << message << std::endl;
        }
    }
};

template <typename Fn>
void expectRuntimeError(SelfTestState& state, Fn&& fn, const std::string& message)
{
    try {
        fn();
        state.expect(false, message);
    } catch (const RuntimeError&) {
    }
}

void testRuntimeValue(SelfTestState& state)
{
    RuntimeValue intValue = RuntimeValue::fromInt(128);
    state.expect(intValue.toHexString(true) == "0x8000", "int 128 script bytes");
    state.expect(intValue.toScriptNum() == 128, "int 128 round trip");

    RuntimeValue negative = RuntimeValue::fromInt(-1);
    state.expect(negative.toHexString(true) == "0x81", "int -1 script bytes");

    RuntimeValue bytes = RuntimeValue::fromHexString("0x2a");
    state.expect(bytes.toScriptNum() == 42, "bytes 0x2a to int");
    state.expect(bytes.truthy(), "bytes truthy");

    RuntimeValue boolean = RuntimeValue::fromBool(false);
    state.expect(!boolean.truthy(), "false is falsy");

    RuntimeValue array = RuntimeValue::fromArray(
        {RuntimeValue::fromInt(1), RuntimeValue::fromInt(2)}
    );
    state.expect(array.toHexString(true) == "0x0102", "array byte flatten");
}

void testEnvironment(SelfTestState& state)
{
    auto root = Environment::createRoot();
    root->define(
        "x",
        RuntimeSlot(RuntimeValue::fromInt(7), "int", StorageClass::MainStack)
    );

    auto child = root->createChild("block");
    state.expect(child->resolve("x").value.toScriptNum() == 7, "parent lookup");

    child->assign("x", RuntimeValue::fromInt(9));
    state.expect(root->resolve("x").value.toScriptNum() == 9, "parent assign");

    child->define(
        "local",
        RuntimeSlot(RuntimeValue::fromString("ok"), "string")
    );
    state.expect(child->containsLocal("local"), "local define");
    state.expect(!root->containsLocal("local"), "local not in parent");

    expectRuntimeError(
        state,
        [&]() {
            child->define(
                "local",
                RuntimeSlot(RuntimeValue::fromString("dup"), "string")
            );
        },
        "duplicate define should fail"
    );

    child->markConsumed("local");
    expectRuntimeError(
        state,
        [&]() { child->resolve("local"); },
        "consumed variable should fail"
    );

    root->markDeleted("x");
    expectRuntimeError(
        state,
        [&]() { child->resolve("x"); },
        "deleted variable should fail"
    );
}

void testRuntimeContext(SelfTestState& state)
{
    ContractNode contract("Smoke");
    contract.members.push_back(
        std::make_unique<FunctionNode>("main", std::vector<ParameterInfo>{})
    );
    contract.members.push_back(std::make_unique<StructDefNode>(
        "Box",
        std::vector<std::pair<std::string, StructFieldType>>{
            {"value", StructFieldType("int")}
        }
    ));

    RuntimeContext context;
    context.registerContract(contract);
    state.expect(context.contract() == &contract, "contract registration");
    state.expect(context.findFunction("main") != nullptr, "function lookup");
    state.expect(context.findStruct("Box") != nullptr, "struct lookup");

    RuntimeCallFrame frame;
    frame.functionName = "main";
    frame.environment = context.globalEnvironment()->createChild("main");
    context.pushFrame(std::move(frame));
    state.expect(context.callDepth() == 1, "push frame");
    state.expect(context.currentFrame()->functionName == "main", "current frame");
    context.popFrame();
    state.expect(context.callDepth() == 0, "pop frame");
}

} // namespace

bool runRuntimeSelfTest(std::ostream& out, std::ostream& err)
{
    SelfTestState state{err};

    testRuntimeValue(state);
    testEnvironment(state);
    testRuntimeContext(state);

    if (state.failures == 0) {
        out << "Runtime self-test passed" << std::endl;
        return true;
    }

    err << "Runtime self-test failed with " << state.failures << " failure(s)"
        << std::endl;
    return false;
}

} // namespace apc_interpreter
