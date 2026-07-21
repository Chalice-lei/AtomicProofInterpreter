# Example Contracts and Common Patterns

---

This page maps the repository's `.ct` examples to practical learning goals. After [Tutorial 1: P2PKH](./tutorial-1-p2pkh.md) and [Tutorial 2: Counter](./tutorial-2-counter.md), use this as the next reading path.

---

## 1. Basic Syntax Examples

The small examples under `test/` are useful when you want to confirm one language feature in isolation.

| Directory | Focus |
|-----------|-------|
| `test/test_basic_statement/test_variable_declaration/` | Basic declarations and initialization |
| `test/test_basic_statement/test_assignment_statement/` | Literal, variable, and function-result assignment |
| `test/test_basic_statement/test_operator/` | Arithmetic and comparison operators |
| `test/test_function/` | Public functions, private functions, parameters, returns |
| `test/test_loop/` | Fixed `Range(start, stop, step)` loops |
| `test/test_array_and_struct/` | Structs, arrays, and chained field access |

Compile a single file when checking a feature:

```bash
./utxo_Interpreter test/test_basic_statement/test_operator/test_operator_add/operator_add_two_operands.ct
```

---

## 2. Standard Library Templates

The standard library lives under `stdlib/`.

| File | Purpose |
|------|---------|
| `stdlib/std/p2pkh.ct` | Standard P2PKH signature verification |
| `stdlib/std/schnorr.ct` | Schnorr verification helpers |

Example:

```python
import std.p2pkh

Contract Wallet:
    def main(signature: hex, pubKey: hex):
        ok = verifyP2PKH(signature, pubKey)
        Return ok
```

Library functions may access host-contract `self.X` fields. For example, `std.p2pkh` expects `self.pubKeyHash` to be provided when the locking script is instantiated.

---

## 3. Transaction-Level Contracts

`test/contract_file/` contains larger examples that model previous transactions, current outputs, and locking-script suffix data.

| File | Pattern |
|------|---------|
| `counter.ct` | Read old state from a previous transaction and verify incremented state |
| `price_oracle.ct` | Oracle-related data and output hash verification |
| `collateral_lending.ct` | Multi-party lending output constraints |
| `orderBook.ct` / `orderBook_*.ct` | Order book, change, tax, buyer/seller outputs |
| `OrderBookSell.ct` / `orderBookSell*.ct` | Sell-order paths and FT/TBC output checks |

When reading these files, first inspect `Struct` layout, then the public function that reconstructs previous state, and finally the `EqualVerify` checks around `BVM.unlockingInput` and `BVM.outputsHash`.

---

## 4. sCrypt Rewrite Examples

`scrypt_rewrites/` contains examples rewritten from suitable sCrypt boilerplate contracts.

| File | Good For |
|------|----------|
| `helloWorld.ct` | Minimal hash/string checks |
| `hashLock.ct` | Hash locks |
| `timeLock.ct` | Time locks |
| `multiSigPayment.ct` | Multisig payments |
| `accumulatorMultiSig.ct` | Accumulator-style multisig |
| `atomicSwap.ct` | Atomic swaps |
| `coinToss.ct` | Hash commitments and two-party flows |
| `matrix2x2.ct` | Fixed arrays and fixed loops |
| `modExp.ct` | Arithmetic computation |

One important difference: sCrypt supports multiple public methods, while this compiler executes all non-underscore functions in source order. Multi-path contracts are usually rewritten as one public entry with a `path` argument.

---

## Next Steps

- [How to Write a Contract](../how-to-write-a-contract.md)
- [Ownership System](../advanced/ownership-system.md)
- [Alt Stack and Multi-Function Cooperation](../advanced/altstack-and-multi-function.md)

---

[🇨🇳 中文版](../../zh/tutorials/examples-and-patterns.md)
