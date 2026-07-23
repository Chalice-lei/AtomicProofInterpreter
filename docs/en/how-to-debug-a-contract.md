# How to Debug a Contract

---

UTXO_Compiler includes an interactive command-line debugger that single-steps through compiled bytecode on a local BVM simulator. You can set breakpoints, inspect stack state, trace function calls, and load real transaction data to simulate the on-chain environment.

---

## Starting the Debugger

```bash
# Basic start
./utxo_Interpreter my_contract.ct --debug

# Contract uses sub-scope alt stack operations (--asa mode)
./utxo_Interpreter my_contract.ct --debug --asa
```

After starting the debugger, you first select a language:
```bash
Select CLI language [zh/en] (default: zh):
```

---

## Selecting the Debug Target

If the contract has multiple public functions, the debugger will prompt you to select:

```
=====================================
Debuggable function list:
=====================================
[1] getCountFromPreTX (public, 1 parameter)
[2] verifyCurrentTX (public, 1 parameter)

Please select the function to debug (enter number 1-2, or press Enter to skip): _
```

- Enter a number to debug only that function
- Press Enter to input parameters for all public functions and debug them in sequence

Then input function parameters:

```
=====================================
Debugging function: getCountFromPreTX (public)
Provide initial values for the following parameters:
=====================================

Parameter 1/1: pretx (struct: PreTX, 17 fields)
```

**Parameter input format**:

| Type | Input method |
|------|-------------|
| Integer | `42`, `-10`, `0x1a` (hexadecimal integer) |
| Hex bytes | `0x1234abcd` |
| String | `"hello"` (enclosed in quotes) |
| Default value | Press Enter to use the type's default value |

---

## Command Reference

After selecting the function to debug and entering parameters, the REPL prompt appears:

```
=====================================
Pushed 17 parameters onto the main stack
=====================================
=====================================
  UTXO_Compiler Debugger v1.0
=====================================
Loaded: ../../contract_file/counter.ct
Type 'help' to see available commands.

(apc-debug) _
```

Type `help` to see all available commands.

### Execution Control

| Command | Short | Description |
|---------|-------|-------------|
| `run` | `r` | Start execution (or continue from current state) |
| `continue` | `c` | Continue from a breakpoint or pause |
| `step` | `s` | Step into (enters function calls) |
| `next` | `n` | Step over (doesn't enter function calls; treats whole call as one step) |
| `finish` | `f` | Step out of current function, execute to the next line in the caller |
| `reset` | — | Completely reset the VM to the initial state when entering the REPL |
| `pause` | — | Pause running execution |

### Breakpoint Management

| Command | Short | Description |
|---------|-------|-------------|
| `break <line>` | `b <N>` | Set a breakpoint at the specified source line |
| `break <funcname>` | `b <name>` | Set a breakpoint at a function entry |
| `delete <breakpoint ID>` | `d <ID>` | Delete the specified breakpoint |
| `disable <breakpoint ID>` | — | Disable a breakpoint (doesn't trigger, but retained) |
| `enable <breakpoint ID>` | — | Re-enable a breakpoint |
| `info breakpoints` | `info bp` | List all breakpoints and their status |

### State Inspection

| Command | Short | Description |
|---------|-------|-------------|
| `list [line]` | `l` | Display source code; defaults to centering on the current execution line |
| `stack` | — | Display current contents of main stack and alt stack |
| `backtrace` | `bt` | Display function call stack |
| `bytecode [N]` | `bc [N]` | Display bytecode near the current execution position; N is context lines |

### Transaction Data

| Command | Description |
|---------|-------------|
| `settxfile <path>` | Load transaction data from a JSON file (for simulating `BVM.*` fields) |
| `showtx` | Display currently loaded transaction data |

### Other

| Command | Description |
|---------|-------------|
| `help` / `h` | Display command help |
| `quit` / `q` / `exit` | Exit the debugger |
| `clear` | Clear the screen |

---

## Generating Debug Info (Without Starting the Debugger)

If you only need to generate debug info files for use with other tools, use the `-d` flag:

```bash
./utxo_Interpreter my_contract.ct -d
```

The debug info contains source-to-bytecode mappings, the function symbol table, and the scope hierarchy for third-party tools. `scopeNestingValid` reports whether scope enter/exit events were balanced during generation; the debugger rejects debug info when it is `false` or when scope ranges or parent relationships are invalid.

---

## Next Steps

- [Tutorial 1: P2PKH Contract Introduction](./tutorials/tutorial-1-p2pkh.md) — Trace a real signature contract with the debugger

---

[🇨🇳 中文版](../zh/how-to-debug-a-contract.md)
