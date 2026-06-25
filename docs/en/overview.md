# Overview

---

## What is UTXO_Compiler

`UTXO_Compiler` (executable name `utxo_interpreter`) is a smart contract compiler targeting the **Bitcoin UTXO model**. It accepts high-level contract source files with the `.ct` extension and compiles them into BVM bytecode that can be embedded in Bitcoin transaction locking scripts.

The contract language syntax is derived from Python — using indentation to delimit code blocks and `def` to define functions — but adds mandatory type declarations and a **compile-time ownership check** mechanism to help developers catch data misuse issues before deployment.

---

## How Bitcoin Smart Contracts Work

### The UTXO Model

Unlike Ethereum's account model, Bitcoin uses the **UTXO (Unspent Transaction Output)** model. Every Bitcoin transaction consumes some existing UTXOs and produces new ones.

Each UTXO carries two things:

- **Satoshis**: how much Bitcoin this output locks
- **Locking script**: a piece of bytecode that specifies "under what conditions this money can be spent"

To spend a UTXO, the initiator must provide an **unlocking script** in the transaction input. Only when the unlocking script and locking script execute together with a true result is the spend recognized by the network.

```
  Previous transaction output             Current transaction
  ┌──────────────────┐           ┌──────────────────────┐
  │ value: 100000sat  │ ─────────▶│ Input: unlocking script (key) │
  │ Locking script (lock) │           │ Output: new locking script    │
  └──────────────────┘           └──────────────────────┘
         Lock                              Key + New Lock
```

Smart contracts means **writing custom logic into the locking script**: not just verifying a signature, but also verifying time, data format, multi-party conditions, and any other programmable conditions.

### What is BVM

BVM (Bitcoin Virtual Machine) is the underlying virtual machine that executes locking/unlocking scripts. It is a **stack-based state machine** with no heap, no global variables — only two stacks (main stack and alt stack) and a set of opcodes.

UTXO_Compiler translates your high-level contract code into opcode sequences that BVM can directly execute. Understanding BVM's stack model is key to understanding compiler behavior and the ownership system. See [Bitcoin Basics](./bitcoin-basics.md) for more.

---

## How UTXO_Compiler Works

```
  Contract source (.ct)
       │
       ▼
  ┌──────────┐
  │  Lexer   │  Tokenizes source into a token stream (keywords, identifiers, literals…)
  └──────────┘
       │
       ▼
  ┌──────────┐
  │  Parser  │  Builds an Abstract Syntax Tree (AST)
  └──────────┘
       │
       ▼
  ┌──────────────┐
  │ Semantic     │  Type checking · Ownership validation · Scope resolution
  │ Analysis     │
  └──────────────┘
       │
       ▼
  ┌──────────────┐
  │ Bytecode     │  AST Visitor traversal → BVM opcode sequence
  │ Generation   │
  └──────────────┘
       │
       ▼
  JSON-formatted bytecode (locking script content)
```

The entire compilation pipeline is driven by an internal **Pass Manager**, with each phase registered and executed as an independent Pass.

---

## Key Design Trade-offs of the Language

### Explicit Type Declarations

All function parameters and struct fields must declare types. There is no automatic type inference, no `var` / `auto`. This may seem verbose, but in a zero-tolerance BVM environment, "what you see in code is what happens on the stack" is more important than brevity.

### Compile-time Ownership Checks

After a local variable is passed to a built-in function or assigned to another variable, the original variable is "consumed" and cannot be referenced again. This mechanism directly corresponds to BVM's stack-pop semantics — at the bytecode level, using a variable means popping the value from the stack. The ownership system lets the compiler catch these errors at compile time rather than waiting for an on-chain execution failure.

### Contract State Fixed in Bytecode

Contract member variables are compiled directly into the bytecode itself, requiring users to substitute different data in the bytecode to generate different locking scripts. This is a direct manifestation of the UTXO model's "state-as-code" characteristic, fundamentally different from Ethereum's "contract address + on-chain storage" model.

### Single Contract Per File

Each `.ct` file contains exactly one contract. This naturally aligns with the model of "one UTXO corresponds to one locking script."

---

## Comparison with Other Solutions

|  | UTXO_Compiler `.ct` | sCrypt (BSV) | Bitcoin Script (native) |
|--|---------------------|-------------|------------------------|
| Abstraction level | High-level language | High-level language | Assembly-level |
| Syntax style | Python-like | TypeScript-like | Opcode sequence |
| Type system | Strongly typed | Strongly typed | Untyped |
| Ownership check | ✓ Compile-time | ✗ | — |
| Debugging tools | Built-in interactive debugger | Plugin support | None |
| Constructor | Manual embedding | ✓ | Manual embedding |

---

## Next Steps

- [Installation](./installation.md) — Set up the compilation environment
- [How to Write a Contract](./how-to-write-a-contract.md) — Learn `.ct` syntax, types, functions, structs, and imports
- [Tutorial 1: P2PKH](./tutorials/tutorial-1-p2pkh.md) — Write a practical signature contract
- [Example Contracts and Common Patterns](./tutorials/examples-and-patterns.md) — Browse repository examples by topic
- [How to Test a Contract](./how-to-test-a-contract.md) — Compile, debug, and organize test cases

---

[🇨🇳 中文版](../zh/overview.md)
