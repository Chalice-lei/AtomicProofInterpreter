# UTXO_Compiler Debugger User Manual

This document describes how to use the UTXO_Compiler built-in CLI debugger, suitable for interactive debugging of contract source code (`.ct`).

---

## 1. Overview

### 1.1 Features

- **UTXO_Compiler Debugger** runs a BVM (Bytecode Virtual Machine) simulator on compiled bytecode, supporting:
  - Breakpoints (by line, by function)
  - Single-step execution (step into, step over, step out)
  - Viewing source code, bytecode, stack, and call stack
  - Transaction data loading and viewing (for contract debugging scenarios)

### 1.2 Prerequisites

- The project must be built with **debugger enabled** (`BUILD_DEBUGGER`).
- Starting the debugger will first **implicitly compile** the source file, then enter the interactive REPL.

---

## 2. Starting the Debugger

### 2.1 Command Line

```bash
./utxo_interpreter <source_file.ct> --debug
```

Optional parameters (same as normal compilation):

| Option | Description |
| ------------------------------------- | -------------------------------------------------------- |
| `--debug` | Start the interactive debugger (also enables debug info) |
| `--allow-subscope-altstack` / `--asa` | Allow setAlt/setMain in sub-scopes (if/else, private functions) |
| `-l <level>` / `--log-level <level>` | Set log level (e.g., debug, info, warning) |

Examples:

```bash
./utxo_interpreter contract.ct --debug
./utxo_interpreter contract.ct --debug --asa
```

### 2.2 Startup Process

1. **Compile**: Compile the `source_file.ct`, generating bytecode and debug info; if compilation fails, output errors and exit.
2. **Functions and parameters (optional)**: If a function list exists in the debug info, you will be prompted to:
   - **Select the function to debug**: Enter a number `1`~`N` to debug only that function; **press Enter** to debug the entire file and input parameters for all **public functions** in sequence.
   - **Parameter input format** (for each parameter, choose one):
     - Integer: `42`, `-100`, `0x1a` (hexadecimal)
     - Hex bytes: `0x1234abcd`
     - String: `"hello"` (enclosed in double quotes)
     - Enter: use the default value for that parameter type
3. **Enter REPL**: Displays "UTXO_Compiler Debugger v1.0" and the loaded source filename; prompts `help` to view commands.

---

## 3. Command Overview

Commands are case-insensitive; most commands have abbreviations. The table below is a quick reference.

| Category | Command (abbreviation) | Description |
| ------------ | ------------------------------- | ------------------------------ |
| **Execution Control** | `run` / `r` | Start/restart execution |
| | `reset` | Reset VM to initial state |
| | `continue` / `c` | Continue from pause |
| | `step` / `s` | Step into |
| | `next` / `n` | Step over |
| | `finish` / `f` | Step out of current function |
| | `pause` | Pause execution |
| **Breakpoints** | `break` / `b` \<line\|\funcname\> | Set line or function breakpoint |
| | `delete` / `d` \<ID\> | Delete breakpoint |
| | `disable` \<ID\> | Disable breakpoint |
| | `enable` \<ID\> | Enable breakpoint |
| | `info breakpoints` / `info bp` | List all breakpoints |
| **Inspection** | `list` / `l` [line] | Display source code (defaults to current line area) |
| | `stack` | Display main and alt stacks (supports parameters) |
| | `backtrace` / `bt` | Display call stack |
| | `bytecode` / `bc` [N] | Display bytecode (optional context count N) |
| **Transaction** | `settxfile` \<file\> | Load transaction data from file |
| | `showtx` | Display current transaction data |
| **Other** | `help` / `h` | Display help |
| | `quit` / `q` / `exit` | Exit debugger |
| | `clear` | Clear screen |

---

## 4. Execution Control

### 4.1 run / r

- **Purpose**: Start execution or re-execute from the current state (does not reset VM).
- **Notes**: If an initial stack was set previously (e.g., function parameters), continues from the current PC; typically used with `reset` to "run it again."

### 4.2 reset

- **Purpose**: Reset the VM to its initial state (PC, stacks, etc. restored to the state when entering the REPL).
- **Notes**: Breakpoints are retained; you can then use `run` or `step`/`next` to re-execute.
- **Note**: `reset` does not clear breakpoints that have been set, nor does it clear transaction data loaded via `settxfile`.

### 4.3 continue / c

- **Purpose**: When the VM is in a **paused** state, continue execution from the current breakpoint/single-step position until another breakpoint is hit, a single-step completes, execution ends, or an error occurs.
- **Notes**: If the VM is not paused, prompts "VM is not paused, cannot continue."

### 4.4 step / s (step into)

- **Purpose**: Execute one instruction; if the instruction is a "call," enters the called function.
- **Notes**: Only valid when the VM is in **READY** or **PAUSED** state.

### 4.5 next / n (step over)

- **Purpose**: Execute to the next statement in the "current stack frame"; if the current statement contains a call, does not enter the called function.
- **Notes**: Only valid when the VM is in **READY** or **PAUSED** state.

### 4.6 finish / f (step out)

- **Purpose**: Continue execution until the current function returns; stops at the caller.
- **Notes**: Only valid when the VM is in **READY** or **PAUSED** state.

### 4.7 pause

- **Purpose**: Request a pause while the VM is **running** (e.g., during `run` or `continue`).
- **Notes**: If the VM is not running, prompts "VM is not running, cannot pause."

### 4.8 VM States and Lifecycle (informational)

- **READY**: Initial/post-reset state, execution has not yet started; suitable for starting debugging with `run`, `step`, `next`, or `finish`.
- **RUNNING**: Continuously executing a script; you can request a pause with `pause`, or wait for a breakpoint to be hit or execution to complete.
- **PAUSED**: Stopped due to a breakpoint hit, single-step completion, or manual `pause`; you can use `continue` or single-step again (`step`/`next`/`finish`).
- **STEP_MODE**: Internal temporary state used to handle single-step logic; users typically only see prompts like "step into/over/out complete" at the beginning and end.
- **FINISHED**: Reached the end of the current execution scope or bytecode; the CLI prints "Program execution complete." To run again, first `reset`, or simply `run` (which internally implicitly re-starts).
- **ERROR**: An error occurred during execution (e.g., stack underflow, unknown instruction, etc.); the CLI displays "Error: ..."; typically requires `reset` before retrying.

---

## 5. Breakpoint Management

### 5.1 Setting Breakpoints: break / b

- **By line**: `break <line_number>`
  Example: `break 11`, `b 20`
  Sets a breakpoint at the corresponding line in the source file; line numbers are resolved to bytecode positions (may correspond to multiple instructions).

- **By function**: `break <function_name>`
  Example: `break main`, `b myFunc`
  Sets a breakpoint at the function entry point.

- Successfully setting a breakpoint outputs the breakpoint ID and position; breakpoints are resolved to specific PCs via `resolveBreakpoints`.

### 5.2 Deleting Breakpoints: delete / d

- **Usage**: `delete <breakpoint_ID>`
  Example: `delete 1`, `d 2`
  Deletes a breakpoint by ID; prompts if the ID doesn't exist.

### 5.3 Disabling/Enabling: disable / enable

- **Disable**: `disable <breakpoint_ID>` — Breakpoint is retained but won't trigger.
- **Enable**: `enable <breakpoint_ID>` — Breakpoint becomes active again.

### 5.4 Viewing Breakpoints: info breakpoints / info bp

- Lists all breakpoints, including: ID, description (line breakpoint/function breakpoint), status (enabled/disabled/pending resolution), and hit count.

---

## 6. Viewing Information

### 6.1 list / l [line_number]

- **No parameter**: Centers on the source code line corresponding to the current PC; displays 10 lines before and after; current line is marked with `=>`.
- **With parameter**: `list <line_number>` centers display on that line.
- If source code is not loaded, displays "(source not loaded)."

### 6.2 stack

`stack` is used to view the current stack state of the VM, including the main and alt stacks.

#### Basic Usage

- `stack`
  Displays the current state of the **main stack** and **alt stack** (from top to bottom); each item is shown in hexadecimal.
  The number in `[...]` at the beginning of a line is the **absolute index from the stack bottom**, for reference against execution semantics.

- `stack main` / `stack alt` / `stack both`
  Show only the main stack / only the alt stack / both simultaneously.

#### Viewing by "Height (index, with stack bottom as 0)"

In the parameters below, **height/index** uniformly uses the definition of "stack bottom = 0":

- `index = 0` means the **stack bottom**;
- Higher `index` values are closer to the top;
- The `[...]` in CLI output is consistent with the index here; both are **absolute indices from the stack bottom**.

- `stack [main|alt|both] --near <H> [C]`
  Displays data near a certain height on the stack:
  - `H`: Target height (from stack bottom; `H = 0` is the bottom);
  - `C`: Context count (optional, default `5`); the actual display range is \[H − C, H + C], automatically clipped when out of range.

- `stack [main|alt|both] --range <A:B>` or `stack [main|alt|both] -r <A:B>`
  Displays elements within **height range \[A, B]** (inclusive):
  - `A`, `B` are both heights from the stack bottom (`0` is the bottom);
  - If `A > B`, the two are automatically swapped internally.

> Tips:
> - Without any parameters, equivalent to `stack both`, displaying all contents of both stacks;
> - The above parameters only affect "which elements to display" and do not change the stack contents.

### 6.3 backtrace / bt

- Displays the **call stack**: from the current frame to the outermost, showing frame number, function name, and call location (source file:line number) for each level.

### 6.4 bytecode / bc [N]

- **No parameter**: Lists all bytecode and marks the current PC (`=>`), format: `PC | source line | instruction`.
- **With parameter**: `bytecode <N>` only shows N instructions before and after the current PC.
- Source line numbers come from debug info; displays `-` when there is no corresponding line.

---

## 7. Auto-Display When Stopped

- When stopping due to a **breakpoint hit**, **single-step pause**, or **step completion**, if the "display source on stop" option is not disabled, the debugger will automatically:
  - Output the reason for stopping;
  - Show the "current location": source position, PC, current instruction, and source code near the current line (`showCurrentLocation`).

---

## 8. Transaction Data Management (Contract Debugging)

Applicable to contracts that depend on "transaction context" (e.g., those using OP_PUSH_META).

### 8.1 settxfile \<filename\>

- **Purpose**: Load transaction-related data from a text file into the VM for use during execution (mainly for debugging transaction metadata with `OP_PUSH_META`).
- **File format**: One key-value pair `KEY:VALUE` per line; `#` after it is a comment; keys are case-insensitive.
- **Supported keys (corresponding to `OP_PUSH_META` condition values)**:

| Key | Description |
| ---------------- | ---------------------- |
| `version` | Transaction version |
| `locktime` | Lock time |
| `inputCount` | Number of inputs |
| `outputCount` | Number of outputs |
| `inputsHash` | Hash of input data |
| `unlockingInput` | Current unlocking input data |
| `outputsHash` | Hash of output data |

- Values are in `0x` hexadecimal; `version`, `locktime`, `inputCount`, `outputCount` are 4 bytes, little-endian; after successful loading, use `showtx` to view.
- `settxfile` does not automatically reset or re-execute the script; generally, after loading or updating transaction data, you need to re-debug with `reset` / `run` or single-step commands.

Example file:

```text
version: 02000000
locktime: 20a10700
inputCount: 02000000
outputCount: 01000000
inputsHash: a1b2c3d4e5f6789012345678901234567890abcdef1234567890abcdef
unlockingInput: 02ff01ab
outputsHash: 0000111122223333444455556666777788889999aaaabbbbccccddddeeeeffff
```

### 8.2 showtx

- Displays currently loaded transaction data: version, locktime, inputCount, outputCount, inputsHash, unlockingInput, outputsHash; binary fields are printed in hexadecimal.
- If not loaded, prompts to use `settxfile <filename>` to load.

---

## 9. Other Commands

- **help / h**: Display the complete command list and brief descriptions within the REPL.
- **quit / q / exit**: Exit the debugger.
- **clear**: Clear the screen (outputs multiple blank lines).

---

## 10. Command History and Input

- The debugger keeps a history of approximately the last 100 commands (for internal recording only).
- Leading and trailing spaces in input are ignored; command names are converted to lowercase before matching, so they are case-insensitive.

---

## 11. Error and Status Messages

- **Execution/single-step**: If the VM is not ready or not paused, the corresponding status is reported (e.g., "VM is not paused, cannot continue," "VM is not ready, cannot single-step").
- **Breakpoints**: Invalid breakpoint IDs or unresolvable line numbers/function names result in clear errors or warnings.
- **Transaction file**: When the file cannot be opened, key-value format errors, or value parsing failures occur, an error is output explaining the line number or key name.
- **Execution complete**: When the execution scope or script end is reached, "Program execution complete" is output; the stack top/contents at this point are the final execution result, viewable via `stack`.
- **Runtime errors**: When a VM internal exception occurs (e.g., unknown instruction, illegal stack operation), "Error: ..." is output and the error state is entered; typically requires `reset` before re-execution.

---

## 12. Typical Usage Flow Example

1. **Start and select function/parameters**
   ```bash
   ./apc my.ct --debug
   ```
   Follow prompts to select the function to debug (or press Enter to enter parameters for all public functions).

2. **Set breakpoints and run**
   ```text
   (apc-debug) break 10
   (apc-debug) break main
   (apc-debug) info breakpoints
   (apc-debug) run
   ```

3. **Inspect after stopping**
   ```text
   (apc-debug) list
   (apc-debug) stack
   (apc-debug) backtrace
   ```

4. **Single-step and continue**
   ```text
   (apc-debug) next
   (apc-debug) step
   (apc-debug) continue
   ```

5. **When transaction data is needed**
   ```text
   (apc-debug) settxfile test/tx_transaction_test.txt
   (apc-debug) showtx
   (apc-debug) run
   ```

6. **Re-run**
   ```text
   (apc-debug) reset
   (apc-debug) run
   ```

7. **Exit**
   ```text
   (apc-debug) quit
   ```

---

## 13. Relationship with Build/Compile

- Debugger mode (`--debug`) **automatically enables debug info** and **implicitly compiles** the specified source file; there is no need to separately pass `-d` or `--debug-output`.
- If you only need to generate debug info in **non-debug** mode (e.g., export to a file), use `-d` or `--debug-output <file>`; see compiler help: `apc -h`.

---

[🇨🇳 中文版](../../zh/references/debugger_user_manual.md)
