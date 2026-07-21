# How to Test a Contract

---

Before deploying a contract on-chain, thoroughly testing all execution paths is essential. This section covers methods for local testing using tools provided by UTXO_Compiler, along with practical recommendations for organizing test cases.

---

## Testing Strategy Overview

The testing focus for UTXO contracts differs somewhat from Ethereum contracts. Since all contract logic executes on-chain, the core objectives of local testing are:

1. **Verify happy paths**: valid inputs should make the contract return true (spend allowed)
2. **Verify rejection paths**: invalid inputs should be rejected by the contract (returns false or terminates)
3. **Verify boundary conditions**: values exactly at condition boundaries
4. **Verify all public functions**: each spending path tested independently
5. **Verify bytecode output**: compilation result matches expected opcode sequence

---

## Method 1: Compiler Smoke Test

The most basic test is to directly compile the contract file with the compiler, confirming no syntax errors or ownership errors:

```bash
./utxo_Interpreter my_contract.ct
```

If it outputs JSON bytecode without errors, the contract is at least syntactically and semantically correct.

If you maintain a project with multiple contracts, it's recommended to use a batch script for quick smoke checks:

```bash
# Compile contracts in contracts/ directory one by one
COMPILER="${COMPILER:-utxo_Interpreter}"
for f in contracts/**/*.ct; do
    echo "Testing: $f"
    "$COMPILER" "$f" > /dev/null && echo "  OK" || echo "  FAILED"
done
```

---

## Method 2: Manual Testing with the Debugger

The [built-in debugger](./how-to-debug-a-contract.md) is currently the most direct means of execution verification. Start the debugger, input a set of parameters, let the contract run to completion, and observe whether the final value on the stack is true:

```bash
./utxo_Interpreter my_contract.ct --debug
```

### Test Matrix

For each public function, it's recommended to prepare test inputs according to the following matrix:

| Scenario | Expected Result |
|----------|----------------|
| Correct signature + matching public key | Stack top = 1, pass |
| Wrong signature | Execution terminates or stack top = 0 |
| Public key hash mismatch | `EqualVerify` fails, terminates |
| Empty data / zero value | Depends on contract logic |
| Boundary values (e.g., time exactly at deadline) | Verify `>=` / `>` semantics |

---

## Method 3: Writing Test Contract File Sets

For projects requiring continuous integration, it's recommended to organize test cases as independent `.ct` files in a `tests/` directory and run them in bulk with a shell script:

### Batch Test Script

```bash
#!/bin/bash
# run_tests.sh

COMPILER="${COMPILER:-utxo_Interpreter}"
PASS=0
FAIL=0

for f in tests/**/*.ct; do
    output=$($COMPILER "$f" 2>&1)
    if [ $? -eq 0 ]; then
        echo "PASS: $f"
        ((PASS++))
    else
        echo "FAIL: $f"
        echo "  Error: $output"
        ((FAIL++))
    fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
```

---

## Method 4: Debug Info-Assisted Testing

Compile with the `-d` flag to generate debug info files, then use the debugger to check each line's execution:

```bash
utxo_Interpreter my_contract.ct -d
```

Debug info contains a mapping from source line numbers to bytecode offsets, which can be used to:

- Confirm whether a piece of code was executed
- Check whether loop unrolling is correct
- Verify jump logic for conditional branches

---

## Common Testing Pitfalls

**Pitfall 1: Ownership errors only trigger at runtime**

Ownership checks are compile-time — the compiler reports errors during the `utxo_Interpreter` phase. When testing, you only need to confirm compilation passes; there is no need to worry about "runtime ownership errors."

**Pitfall 2: `EqualVerify` failure doesn't return false — it terminates**

`EqualVerify`, `CheckSigVerify`, and other `*Verify` functions terminate execution directly when they fail (equivalent to script execution failure). When testing rejection scenarios, failures triggered by these functions do not leave `0` on the stack — they abort the entire execution. The debugger will show that execution was terminated.

**Pitfall 3: Contract member variables are part of the bytecode**

When testing different parameters for the same contract logic, recompilation is not necessary, because contract member variables are embedded directly in the bytecode and must be manually replaced by the user.

**Pitfall 4: Loop count is a compile-time constant**

The parameters to `Range()` must be values determinable at compile time (literals or constant expressions). Variables cannot be used to control loop counts.

---

## Next Steps

- [How to Debug a Contract](./how-to-debug-a-contract.md) — Deeply inspect execution with the interactive debugger

---

[🇨🇳 中文版](../zh/how-to-test-a-contract.md)
