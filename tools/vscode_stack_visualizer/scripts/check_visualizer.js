#!/usr/bin/env node

const fs = require("fs");
const path = require("path");

const extensionRoot = path.resolve(__dirname, "..");
const repoRoot = path.resolve(extensionRoot, "..", "..");
const visualizerPath = path.join(extensionRoot, "stack_visualizer", "index.html");
const schemaPath = path.join(extensionRoot, "schemas", "apc-stack-trace.schema.json");
const examplesDir = path.join(repoRoot, "examples", "stack_traces");

function readText(file) {
  return fs.readFileSync(file, "utf8");
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function parseJson(file) {
  return JSON.parse(readText(file));
}

function checkHtml() {
  const html = readText(visualizerPath);
  const script = html.match(/<script>([\s\S]*)<\/script>/);
  assert(script, "visualizer inline script not found");
  new Function(script[1]);

  const requiredIds = [
    "traceFile",
    "stepSummary",
    "searchBox",
    "filterMode",
    "traceOverview",
    "eventRail",
    "detailTabs",
    "diagnosticsPanel",
    "explainBody",
    "copyExplainBtn",
    "copyMarkdownBtn",
    "copyTraceMarkdownBtn",
    "presentationBtn",
    "lifecyclePanel",
    "toggleJsonBtn",
    "copyJsonBtn",
    "jsonBox",
    "mainStackList",
    "altStackList"
  ];

  for (const id of requiredIds) {
    assert(
      html.includes(`id="${id}"`),
      `required visualizer element #${id} is missing`
    );
  }

  console.log("visualizer HTML/script ok");
}

function checkSchema() {
  parseJson(schemaPath);
  console.log("trace JSON schema ok");
}

function checkExamples() {
  const files = fs.readdirSync(examplesDir)
    .filter((file) => file.endsWith(".json"))
    .sort();
  assert(files.length > 0, "no example stack traces found");

  for (const file of files) {
    const fullPath = path.join(examplesDir, file);
    const trace = parseJson(fullPath);
    assert(trace.format === "apc-stack-trace", `${file}: unexpected format`);
    assert(Array.isArray(trace.steps), `${file}: missing steps array`);
    assert(trace.steps.length > 0, `${file}: empty trace`);
    const firstStep = trace.steps[0];
    assert(firstStep.opcode || firstStep.instruction, `${file}: first step has no opcode`);
  }

  console.log(`example traces ok (${files.length})`);
}

checkHtml();
checkSchema();
checkExamples();
