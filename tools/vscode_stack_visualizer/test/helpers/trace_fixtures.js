"use strict";

function stackValue(value, options = {})
{
    const intString = String(value);
    const bytes = Buffer.from(options.text ?? intString, "utf8");
    return {
        hex: options.hex || "0x" + bytes.toString("hex"),
        byteLength: bytes.length,
        intString,
        ascii: options.text,
        elementId: options.elementId || `e${intString}`,
        originStep: options.originStep || 0,
        originStack: options.originStack || "main",
        depth: options.depth || 0
    };
}

function effects(main = {}, alt = {}, moves = [])
{
    return {
        mainStack: {
            pushed: main.pushed || [],
            popped: main.popped || [],
            reordered: Boolean(main.reordered),
            sizeBefore: main.sizeBefore || 0,
            sizeAfter: main.sizeAfter || 0
        },
        altStack: {
            pushed: alt.pushed || [],
            popped: alt.popped || [],
            reordered: Boolean(alt.reordered),
            sizeBefore: alt.sizeBefore || 0,
            sizeAfter: alt.sizeAfter || 0
        },
        moves
    };
}

function step(index, options = {})
{
    const mainBefore = options.mainBefore || [];
    const mainAfter = options.mainAfter || mainBefore;
    const altBefore = options.altBefore || [];
    const altAfter = options.altAfter || altBefore;
    return {
        step: index,
        pc: options.pc ?? index,
        opcode: options.opcode || `OP_${index}`,
        instruction: options.instruction || options.opcode || `OP_${index}`,
        operand: options.operand || "",
        functionName: options.functionName || (index < 2 ? "first" : "second"),
        sourceFile: options.sourceFile || "synthetic.ct",
        sourceLine: options.sourceLine || index + 1,
        source: {
            file: options.sourceFile || "synthetic.ct",
            line: options.sourceLine || index + 1,
            column: 1
        },
        mainStackBefore: mainBefore,
        mainStackAfter: mainAfter,
        altStackBefore: altBefore,
        altStackAfter: altAfter,
        effects: options.effects || effects(
            {sizeBefore: mainBefore.length, sizeAfter: mainAfter.length},
            {sizeBefore: altBefore.length, sizeAfter: altAfter.length}
        ),
        ...(options.error ? {error: options.error} : {})
    };
}

function edgeCaseTrace()
{
    const positive = stackValue(42, {elementId: "positive", hex: "0x2a"});
    const negative = stackValue(-7, {elementId: "negative", hex: "0x87"});
    const zero = stackValue(0, {elementId: "zero", hex: "0x"});
    const huge = stackValue("123456789012345678901234567890", {
        elementId: "huge",
        hex: "0xd20a3f4eeee073c3f60fe98e01"
    });
    const spaced = stackValue(0, {
        elementId: "spaced",
        hex: "0x202068656c6c6f20776f726c642020",
        text: "  hello world  "
    });
    const unicode = stackValue(0, {
        elementId: "unicode",
        hex: "0xe59bbee781b5e993be",
        text: "图灵链"
    });
    const values = [positive, negative, zero, huge, spaced, unicode]
        .map((item, index, all) => ({...item, depth: all.length - index - 1}));
    const reordered = values.slice().reverse().map((item, index, all) => ({
        ...item,
        depth: all.length - index - 1
    }));
    return {
        version: "1.0",
        format: "apc-stack-trace",
        stackOrder: "bottom-to-top",
        top: "last element in each stack array",
        source: {
            file: "synthetic.ct",
            lines: ["first();", "branch true;", "branch false;", "loop();"]
        },
        debugInfo: {
            sourceFile: "synthetic.ct",
            contractName: "Synthetic",
            lineToPC: {"1": [0, 1], "2": [2], "3": [3], "4": [4, 5]},
            functions: [
                {name: "first", startPC: 0, endPC: 2, isPublic: true},
                {name: "second", startPC: 2, endPC: 6, isPublic: false}
            ]
        },
        lifecycle: {
            idFormat: "e<number>",
            matching: "test fixture",
            elements: values.map((item) => ({
                elementId: item.elementId,
                hex: item.hex,
                originStep: 0,
                originStack: "main",
                lastStep: 1,
                lastStack: "main",
                consumed: false,
                events: [{type: "push", step: 0, pc: 0, stack: "main"}]
            }))
        },
        steps: [
            step(0, {
                opcode: "OP_PUSH_EDGE_VALUES",
                sourceLine: 1,
                mainAfter: values,
                effects: effects({pushed: values, sizeBefore: 0, sizeAfter: values.length})
            }),
            step(1, {
                opcode: "OP_REORDER",
                sourceLine: 1,
                mainBefore: values,
                mainAfter: reordered,
                effects: effects({reordered: true, sizeBefore: values.length, sizeAfter: values.length})
            }),
            step(2, {opcode: "OP_IF_TRUE", sourceLine: 2, mainBefore: reordered}),
            step(3, {opcode: "OP_IF_FALSE", sourceLine: 3, mainBefore: reordered}),
            step(4, {opcode: "OP_LOOP", sourceLine: 4, mainBefore: reordered}),
            step(5, {opcode: "OP_LOOP", sourceLine: 4, mainBefore: reordered})
        ]
    };
}

function largeTrace(stepCount = 10000, stackSize = 1000)
{
    const values = Array.from({length: stackSize}, (_unused, index) => stackValue(index, {
        elementId: `large-${index}`,
        hex: "0x" + index.toString(16).padStart(4, "0"),
        depth: stackSize - index - 1
    }));
    const traceSteps = Array.from({length: stepCount}, (_unused, index) => step(index, {
        opcode: index % 2 ? "OP_LOOP" : "OP_NOP",
        sourceLine: index % 4 + 1,
        mainAfter: index === 0 ? values : [],
        effects: effects(
            index === 0
                ? {pushed: values, sizeBefore: 0, sizeAfter: values.length}
                : {sizeBefore: 0, sizeAfter: 0}
        )
    }));
    return {
        version: "1.0",
        format: "apc-stack-trace",
        source: {file: "large.ct", lines: ["a", "b", "c", "d"]},
        steps: traceSteps
    };
}

module.exports = {
    edgeCaseTrace,
    effects,
    largeTrace,
    stackValue,
    step
};
