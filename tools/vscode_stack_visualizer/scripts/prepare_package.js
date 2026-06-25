#!/usr/bin/env node

const fs = require("fs");
const path = require("path");

const extensionRoot = path.resolve(__dirname, "..");
const targetDir = path.join(extensionRoot, "stack_visualizer");
const target = path.join(targetDir, "index.html");

if (!fs.existsSync(target)) {
  throw new Error(`Bundled visualizer template not found: ${target}`);
}

console.log(`Using bundled visualizer template: ${path.relative(extensionRoot, target)}`);
