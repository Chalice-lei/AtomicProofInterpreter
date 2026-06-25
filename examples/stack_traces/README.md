# Stack Trace Examples

These JSON files are ready to open with the VS Code Webview extension:

```text
AtomicProof: Open Stack Trace Visualizer
```

or:

```text
AtomicProof: Visualize Active Stack Trace
```

## Examples

| File | Source Contract | What to Watch |
| --- | --- | --- |
| `alt_roundtrip.json` | `test/debugger_regression/debug_stack_visualizer_alt.ct` | `OP_TOALTSTACK` and `OP_FROMALTSTACK`, showing main -> alt and alt -> main movement. |
| `arithmetic_line_mapping.json` | `test/debugger_regression/debug_line_mapping_basic.ct` | Arithmetic stack consumption and source line mapping. |
| `branch_loop_true.json` | `test/debugger_regression/debug_branch_loop_scope.ct` | Branch/loop execution with `flag = 1`. |
| `branch_loop_false.json` | `test/debugger_regression/debug_branch_loop_scope.ct` | Branch/loop execution with `flag = -1`. |
| `push_builtin.json` | `test/test_builtin_function_and_object/builtin_function/push_function_test.ct` | Multiple literal pushes, including int, string bytes, and hex bytes. |

Each trace includes synthetic lifecycle metadata. Stack elements carry an
`elementId`, and the top-level `lifecycle.elements` list records origin,
movement, and consumption events for clickable value history in the visualizer.

## Regenerate

From the repository root:

```bash
build/bin/utxo_interpreter run test/debugger_regression/debug_stack_visualizer_alt.ct test_alt_roundtrip 5 --stack-trace-output examples/stack_traces/alt_roundtrip.json
build/bin/utxo_interpreter run test/debugger_regression/debug_line_mapping_basic.ct test_line_mapping 1 2 3 --stack-trace-output examples/stack_traces/arithmetic_line_mapping.json
build/bin/utxo_interpreter run test/debugger_regression/debug_branch_loop_scope.ct test_branch_loop 1 --stack-trace-output examples/stack_traces/branch_loop_true.json
build/bin/utxo_interpreter run test/debugger_regression/debug_branch_loop_scope.ct test_branch_loop -1 --stack-trace-output examples/stack_traces/branch_loop_false.json
build/bin/utxo_interpreter run test/test_builtin_function_and_object/builtin_function/push_function_test.ct test_push --stack-trace-output examples/stack_traces/push_builtin.json
```
