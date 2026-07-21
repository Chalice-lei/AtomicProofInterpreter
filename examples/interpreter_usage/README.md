# Interpreter Usage Example

This example shows how to run the AST interpreter with positional arguments,
named struct parameters, contract instance fields, and BVM runtime fields.

From the project root:

```bash
./build/bin/utxo_Interpreter -l none ast examples/interpreter_usage/interpreter_usage.ct \
  verify \
  0xabcd \
  0x02 \
  --txfile examples/interpreter_usage/context.json
```

Expected result:

```text
AST interpretation result
  status: finished
  function: verify
Return values (1)
  [0] true hex=0x01 int=1
Error: <none>
```

The example uses:

- Positional arguments for `sig` and `pubKey`.
- `--txfile` to load `params.pretx`, `self.pubKeyHash`, `self.minAmount`,
  `bvm.unlockingInput`, and `bvm.checkSigResult`.
- `BVM.checkSigResult=true` to make AST-mode signature verification deterministic.

You can also provide the same runtime values directly on the command line:

```bash
./build/bin/utxo_Interpreter -l none ast examples/interpreter_usage/interpreter_usage.ct \
  verify \
  0xabcd \
  0x02 \
  --self pubKeyHash=0xa6bb94c8792c395785787280dc188d114e1f339b \
  --self minAmount=40 \
  --bvm unlockingInput=0x010203 \
  --bvm checkSigResult=true \
  --param 'pretx.Outputs[1].LockingScript.Size=0x03000000' \
  --param 'pretx.Outputs[1].Value=0x2a000000'
```

## Interactive Shell contract example

`shell_contract.ct` is a complete contract that can be loaded directly by
`shell`. It demonstrates structs, helper functions, branches, loops, and
interactive function calls:

```bash
./build/bin/utxo_Interpreter shell examples/interpreter_usage/shell_contract.ct -l none <<'EOF'
%who
quote(100, 3, 5, true)
canUnlock(275, 275)
canUnlock(200, 275)
sampleTotal()
exit
EOF
```

Expected highlights:

```text
Out[1]: 275
Out[2]: true
Out[3]: false
Out[4]: 55
```
