"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const test = require("node:test");
const Ajv2020 = require("ajv/dist/2020");
const {edgeCaseTrace} = require("../helpers/trace_fixtures");

const extensionRoot = path.resolve(__dirname, "..", "..");
const repoRoot = path.resolve(extensionRoot, "..", "..");
const schema = JSON.parse(fs.readFileSync(
    path.join(extensionRoot, "schemas", "apc-stack-trace.schema.json"),
    "utf8"
));
const ajv = new Ajv2020({allErrors: true, strict: false});
const validate = ajv.compile(schema);
const exampleDir = path.join(repoRoot, "examples", "stack_traces");
const exampleNames = [
    "alt_roundtrip.json",
    "arithmetic_line_mapping.json",
    "branch_loop_false.json",
    "branch_loop_true.json",
    "push_builtin.json"
];

function readExample(name)
{
    return JSON.parse(fs.readFileSync(path.join(exampleDir, name), "utf8"));
}

function schemaErrors()
{
    return ajv.errorsText(validate.errors, {separator: "\n"});
}

test("Schema 严格验证 5 个现有 Trace 和边界值夹具", () => {
    for (const name of exampleNames) {
        assert.strictEqual(validate(readExample(name)), true, `${name}: ${schemaErrors()}`);
    }
    const edge = edgeCaseTrace();
    assert.strictEqual(validate(edge), true, schemaErrors());
    const values = edge.steps[0].mainStackAfter;
    assert(values.some((item) => item.intString === "42"));
    assert(values.some((item) => item.intString === "-7"));
    assert(values.some((item) => item.intString === "0"));
    assert(values.some((item) => item.intString === "123456789012345678901234567890"));
    assert(values.some((item) => item.ascii === "  hello world  "));
    assert(values.some((item) => item.ascii === "图灵链"));
    assert(values.some((item) => item.hex === "0xd20a3f4eeee073c3f60fe98e01"));
});

test("Schema 拒绝错误 format、缺字段、错误字段类型和旧格式", () => {
    const invalid = [
        {format: "wrong", steps: []},
        {format: "apc-stack-trace"},
        {format: "apc-stack-trace", steps: [{step: 0}]},
        {format: "apc-stack-trace", steps: [{step: "0", pc: "x"}]},
        {version: "0.1", trace: []}
    ];
    for (const value of invalid) {
        assert.strictEqual(validate(value), false, JSON.stringify(value));
        assert(validate.errors.length > 0);
    }
});

test("空 Trace 与单步 Trace 的 Schema 边界保持明确", () => {
    assert.strictEqual(validate({format: "apc-stack-trace", steps: []}), true, schemaErrors());
    assert.strictEqual(validate({
        format: "apc-stack-trace",
        steps: [{step: 0, pc: 0}]
    }), true, schemaErrors());
});

test("所有示例的 elementId 在生产、移动和消费期间保持一致", () => {
    for (const name of exampleNames) {
        const trace = readExample(name);
        const lifecycle = new Map(
            (trace.lifecycle?.elements || []).map((item) => [item.elementId, item])
        );
        assert(lifecycle.size > 0, `${name}: missing lifecycle`);

        for (const step of trace.steps) {
            const stacks = [
                ...(step.mainStackBefore || []),
                ...(step.mainStackAfter || []),
                ...(step.altStackBefore || []),
                ...(step.altStackAfter || [])
            ];
            for (const item of stacks) {
                assert(item.elementId, `${name} step ${step.step}: value has no elementId`);
                assert(lifecycle.has(item.elementId), `${name}: unknown ${item.elementId}`);
                assert.strictEqual(lifecycle.get(item.elementId).hex, item.hex);
            }

            for (const move of step.effects?.moves || []) {
                assert(move.elementId, `${name} step ${step.step}: move has no elementId`);
                assert.strictEqual(move.element.elementId, move.elementId);
                const before = move.from === "main"
                    ? step.mainStackBefore
                    : step.altStackBefore;
                const after = move.to === "main"
                    ? step.mainStackAfter
                    : step.altStackAfter;
                assert(before.some((item) => item.elementId === move.elementId));
                assert(after.some((item) => item.elementId === move.elementId));
                const life = lifecycle.get(move.elementId);
                assert(life.events.some((event) =>
                    event.type === "move" &&
                    event.step === step.step &&
                    event.from === move.from &&
                    event.to === move.to
                ));
            }

            for (const item of step.effects?.mainStack?.pushed || []) {
                assert(lifecycle.has(item.elementId));
            }
            for (const item of step.effects?.mainStack?.popped || []) {
                const life = lifecycle.get(item.elementId);
                assert(life?.consumed, `${name}: popped ${item.elementId} is not consumed`);
                assert.strictEqual(life.consumedAt, step.step);
            }
        }
    }
});

test("现有 Trace 覆盖分支双向、循环、同源码行多 PC、重排和 main/alt 往返", () => {
    const branchTrue = readExample("branch_loop_true.json");
    const branchFalse = readExample("branch_loop_false.json");
    assert.strictEqual(branchTrue.steps[0].mainStackBefore[0].intString, "1");
    assert.strictEqual(branchFalse.steps[0].mainStackBefore[0].intString, "-1");
    assert.strictEqual(branchTrue.steps.at(-1).mainStackAfter[0].intString, "4");
    assert.strictEqual(branchFalse.steps.at(-1).mainStackAfter[0].intString, "5");
    assert.notDeepStrictEqual(
        branchTrue.steps.map((item) => item.mainStackAfter.map((value) => value.intString)),
        branchFalse.steps.map((item) => item.mainStackAfter.map((value) => value.intString))
    );
    assert(branchTrue.steps.filter((item) => item.sourceLine === 17).length >= 4);
    assert(branchFalse.steps.filter((item) => item.sourceLine === 17).length >= 4);

    const arithmetic = readExample("arithmetic_line_mapping.json");
    assert(arithmetic.debugInfo.lineToPC["8"].length > 1);
    assert(arithmetic.steps.some((item) => item.effects.mainStack.reordered));

    const alt = readExample("alt_roundtrip.json");
    const directions = alt.steps.flatMap((item) => item.effects.moves || [])
        .map((move) => `${move.from}->${move.to}`);
    assert(directions.includes("main->alt"));
    assert(directions.includes("alt->main"));
});

test("非法 JSON 给出原生可诊断错误", () => {
    assert.throws(() => JSON.parse('{"format":'), /JSON|Unexpected/);
});
