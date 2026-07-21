# Debugger Regression Contracts

These contracts are small manual probes for the CLI debugger.

Run examples:

```bash
build/bin/utxo_Interpreter compile test/debugger_regression/debug_line_mapping_basic.ct -d --debug-output /tmp/apc_line_mapping.debug
build/bin/utxo_Interpreter debug test/debugger_regression/debug_line_mapping_basic.ct
build/bin/utxo_Interpreter run test/debugger_regression/debug_stack_visualizer_alt.ct test_alt_roundtrip 5 --stack-trace-output /tmp/apc_stack_trace.json
test/debugger_regression/run_debugger_regression.sh
```

What to check:

- `pcToSource` and `lineToPC` should be populated in generated debug info.
- `break <line>` should resolve instead of staying `pending`.
- `bytecode` should show source line numbers beside PCs.
- `print` and `locals` should show the correct values for variables near the stack top.
- `step`, `next`, and function breakpoints should behave sensibly around private functions.
- Branch and loop body lines should remain breakable after bytecode optimization.
- `tools/vscode_stack_visualizer` should open stack trace JSON in a VS Code Webview and show pushed, popped, and main/alt moved values.
