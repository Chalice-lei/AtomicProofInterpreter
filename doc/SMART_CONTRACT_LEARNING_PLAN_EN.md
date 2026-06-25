# Smart Contract Learning Plan

This plan is for learners who have basic programming experience but are new to UTXO smart contracts and stack-based virtual machines. The schedule is compressed into one session every one to two days and can be completed in about two weeks. By the end, learners should understand the basic UTXO contract model, read and write simple contracts, and understand locking/unlocking scripts, ABI, bytecode, debugging workflows, testing ideas, and common error diagnosis.

## Reference Materials

- Source: [https://docs.utxocompiler.com/](https://docs.utxocompiler.com/)
- [Overview](https://docs.utxocompiler.com/en/overview)
- [Installation](https://docs.utxocompiler.com/en/installation)
- [Bitcoin Basics](https://docs.utxocompiler.com/en/bitcoin-basics)
- [How to Write a Contract](https://docs.utxocompiler.com/en/how-to-write-a-contract)
- [How to Test a Contract](https://docs.utxocompiler.com/en/how-to-test-a-contract)
- [How to Debug a Contract](https://docs.utxocompiler.com/en/how-to-debug-a-contract)
- [How to Deploy and Call a Contract](https://docs.utxocompiler.com/en/how-to-deploy-and-call-a-contract)
- [Tutorial 1: P2PKH Contract](https://docs.utxocompiler.com/en/tutorials/tutorial-1-p2pkh)
- [Tutorial 2: Counter Contract](https://docs.utxocompiler.com/en/tutorials/tutorial-2-counter)
- [Altstack and Multi-Function Collaboration](https://docs.utxocompiler.com/en/advanced/altstack-and-multi-function)
- [Ownership System](https://docs.utxocompiler.com/en/advanced/ownership-system)
- [Language Specification](https://docs.utxocompiler.com/en/references/language-specification)
- [Built-in Functions Reference](https://docs.utxocompiler.com/en/references/builtin-functions)
- [Built-in Objects Reference](https://docs.utxocompiler.com/en/references/builtin-objects)
- [Debugger User Manual](https://docs.utxocompiler.com/en/references/debugger_user_manual)

## How to Use This Plan

- After each session, keep three artifacts: the practice contract, the compilation result, and debugging or testing notes.
- Start with small, clear exercises, then gradually add state, signatures, hashes, and transaction context.
- If tutorials, tools, or standard library versions differ, follow the documentation and function signatures from the environment you are actually using.
- Real on-chain deployment, broadcasting, and signature construction usually require a wallet, node, SDK, or transaction-building tool. At the beginner stage, focus first on contract semantics, compilation output, and local verification.

## Learning Plan

| Time | Topic | Core Concepts | Recommended Reading Direction | Hands-on Exercise | Command-line Practice | Debugging Practice | Acceptance Criteria |
|---|---|---|---|---|---|---|---|
| Day 1 | UTXO, stack-based VM, and toolchain | UTXO, inputs, outputs, locking script, unlocking script, script concatenation and execution, main stack, altstack, compiler, debugger, standard library | UTXO basics, script execution model, installation guide, tool command guide | Draw how a transaction spends an old UTXO and creates new UTXOs; prepare a local learning environment | Check version, help output, and basic command options | On paper, trace how a signature and public key enter the main stack, then how P2PKH verifies the public key hash and signature | Can explain that a contract describes the conditions under which a UTXO can be spent, and can describe the responsibilities of the compiler, debugger, and standard library |
| Day 2 | First contract and basic syntax | Contract declaration, public functions, return values, indentation, explicit types, single-contract file rule | Contract language introduction, basic syntax | Write an always-success contract and a simple comparison contract, such as checking whether two integers are equal | Compile practice contracts and inspect output files | Enter the debugger and practice run, step, and stack inspection | Can compile a simple contract and find the ABI, locking script, and assembly information in the output |
| Day 4 | Type system, functions, structs, and ownership | Integers, strings, hex values, booleans, addresses, fixed-length bytes, structs, fixed-length arrays, field access, private functions, variable consumption, data copying | Type system, functions, structs, ownership system, language specification | Write a practice contract with structs, array fields, and private helper functions; intentionally write a contract that reuses consumed data, then fix it with copying | Compile type exercises, the failing contract, and the fixed contract | Observe how struct parameters are pushed onto the stack, how fields are accessed, and how copied data changes execution | Can explain array length constraints, how field access affects data use, and when data must be copied |
| Day 6 | Standard library and P2PKH contract | Hashing, hash comparison, signature verification, member variables, public key hash, unlocking parameters | Standard library notes, P2PKH tutorial | Hand-write a P2PKH contract, then rewrite it using standard library functions | Compile the P2PKH practice contract and handle standard library import issues | Practice function breakpoints, run, step, and stack inspection in the debugger | Can read the signature and public key order in unlocking parameters, and can explain the difference between public key hash failure and signature failure |
| Day 8 | Compilation output, ABI, bytecode, and debugger | Metadata, struct information, ABI, function list, locking script, unlocking script, parameter order, breakpoints, run, continue, step, backtrace, bytecode inspection | Compilation output notes, testing notes, debugger user manual | Compare the compilation results of two different contracts; create a complete debugging record with breakpoints, stepping, and stack snapshots | Inspect JSON, search ABI and script fields, and practice help, breakpoint listing, reset, and continue commands | Locate a hash comparison or signature verification failure point, then map nearby bytecode to the assembly output | Can explain that ABI is the unlocking parameter convention, that the locking script is the verification logic written into the output, and can locate common verification failure points |
| Day 10 | State contract basics | Member variables, suffix state, previous transaction, current transaction, transaction hash, state increment, state trust | State contract tutorial, deployment and call notes | Read a counter-style state contract and split it into two phases: reading old state and verifying new state | Compile a state contract exercise and inspect the output | Debug the old-state reading function and the new-state verification function, observing how data is passed between functions | Can explain how a state contract proves that the old state is trustworthy and verifies that the new output state matches expectations |
| Day 12 | Testing, error diagnosis, and deployment/call model | Smoke tests, success paths, rejection paths, boundary values, common compile-time and runtime errors, deployment data flow, call data flow, transaction construction, signing, broadcasting | Testing docs, debugging docs, deployment and call notes, transaction construction materials | Prepare normal, failing, and boundary inputs for practice contracts; outline the deployment/call data flow for P2PKH and state contracts | Compile success and failure path exercises, generate debugging information, and understand the responsibility boundaries between transaction construction, signing, and broadcasting tools | Set breakpoints on failing paths and observe where verification instructions terminate execution; if transaction data files are available, practice loading transaction context and inspecting transactions | Can distinguish syntax errors, type errors, ownership errors, import failures, and runtime verification failures, and can explain the responsibility boundaries between contract compilation, transaction construction, signing, and broadcasting |
| Day 14 | Comprehensive exercise | Requirement breakdown, contract writing, compilation output review, debugging, testing, error diagnosis, notes | Review all previous topics | Complete a small comprehensive exercise, such as "signature verification + counter state transition" or "conditional locking with a path parameter" | Compile the comprehensive exercise and inspect the output | Debug at least one success path and one rejection path; inspect the stack, backtrace, and bytecode at least once | Can independently complete a small contract exercise and explain the full path from contract source to locking script, unlocking parameters, and test verification |

## Final Capability Checklist

| Capability | Passing Standard |
|---|---|
| Prepare the development environment | Can run the contract compilation tool and view version or help information |
| Write basic contracts | Can write a contract with a contract declaration, public functions, private functions, type declarations, and return values |
| Understand UTXO and stack-based VM | Can explain UTXO, locking scripts, unlocking scripts, main stack, altstack, and script execution order |
| Use the compilation tool | Can compile a contract source file and generate structured output |
| Read compilation output | Can identify and explain metadata, struct information, ABI, function list, locking script, and unlocking script |
| Master the ownership system | Can fix compile errors caused by repeatedly consuming variables or fields, and can correctly copy data that needs to be reused |
| Use the standard library | Can import and call standard library functions, and can verify actual function signatures |
| Write a P2PKH contract | Can implement hash verification and signature verification, then debug both success and failure paths |
| Understand contract state | Can explain how member variables are fixed into scripts or state data |
| Understand state contracts | Can explain previous transactions, current transactions, state reading, and state increment verification |
| Use the debugger | Can use breakpoints, run, continue, step, next, finish, stack inspection, backtrace, and bytecode inspection |
| Write tests | Can prepare success paths, rejection paths, and boundary conditions, then verify them with the compiler and debugger |
| Diagnose common errors | Can distinguish syntax errors, type errors, ownership errors, import failures, and runtime verification failures |
| Complete a comprehensive exercise | Can deliver a practice contract, compilation result, test notes, debugging notes, and a short explanation |

## Capability Boundaries to Confirm

| Boundary | Notes |
|---|---|
| Real on-chain deployment and broadcasting | Usually requires a wallet, node, SDK, transaction-building tool, and network configuration. At the beginning stage, it is enough to understand the process before attempting a real broadcast |
| Source of transaction data files | Transaction context required by debuggers or testing tools is usually generated by transaction-building tools, test frameworks, or sample data |
| Constructor workflow | Different contract languages may define constructors, member variable initialization, and deployment parameters differently. Follow the actual language specification being used |
| Script wrapping method | Different chains or tools may use different script wrapping methods. The full deployment flow should be confirmed against the target runtime environment |
