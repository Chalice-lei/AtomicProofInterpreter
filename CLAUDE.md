# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

AtomicProof Compiler (`apc`, CMake target name `utxo_interpreter`) — a C++20 compiler that lowers a custom high-level language (`.ct` files) to a Bitcoin-Script-compatible bytecode. Includes an integrated CLI debugger and a BVM (bytecode VM) simulator.

## Build

Requires CMake ≥ 3.28 and a C++20 compiler. `nlohmann/json` is the only external dep and is auto-fetched by default.

```bash
mkdir build && cd build
cmake ..                                 # Release by default
cmake .. -DCMAKE_BUILD_TYPE=Debug        # dev builds
cmake .. -DUSE_GITEE_MIRROR=ON           # faster JSON fetch in CN
cmake .. -DUSE_SYSTEM_JSON=ON            # use installed nlohmann_json
cmake .. -DJSON_LOCAL_PATH=/path/to/json # use local copy
cmake .. -DBUILD_DEBUGGER=OFF            # exclude debugger sources
cmake .. -DCROSS_COMPILE_WINDOWS=ON      # MinGW-w64 cross build
cmake --build . -j
```

Executable: `build/bin/utxo_interpreter` (a.k.a. `apc`). Build logs to `utxo_interpreter.log`.

## Run

```bash
./utxo_interpreter code.ct                      # compile
./utxo_interpreter -l debug code.ct             # set log level (debug|info|warning|error|critical|none)
./utxo_interpreter --asa code.ct                # allow setAlt/setMain in subscopes (if/else, private funcs)
./utxo_interpreter code.ct -d                   # emit debug info
./utxo_interpreter code.ct --debug              # launch interactive debugger (requires BUILD_DEBUGGER=ON)
./utxo_interpreter --debug-output file.json code.ct
./utxo_interpreter -v                           # version + git info
```

Sample sources live in `test/` (`.ct`, `.txt`). There is no automated test runner — "tests" are driven by running the compiler against files in `test/`.

Release packaging and docker scripts live in `scripts/` and `docker/Dockerfile`.

## Architecture

### Pass pipeline
Compilation is structured as a `PassManager` ([src/pass/](src/pass/)) running ordered passes that share a `PassContext`. The canonical pipeline (registered in [main.cpp](main.cpp)) is:

1. **LexerPass** ([src/lexer_pass.h](src/lexer_pass.h), [src/lexer/](src/lexer/)) — resolves imports and tokenizes `.ct` source.
2. **ParserPass** ([src/parser_pass.h](src/parser_pass.h), [src/parser/](src/parser/)) — builds the AST ([src/ast/](src/ast/)).
3. **AstToBytecodePass** ([src/ast_to_bytecode_pass.h](src/ast_to_bytecode_pass.h)) — runs a sequence of AST visitors from [src/compiler/](src/compiler/), then bytecode emission.
4. **ExportResultsPass** ([src/export_results_pass.h](src/export_results_pass.h)) — writes bytecode / debug info outputs.

### AST visitors (compile-time analyses)
Run in this order inside the AST→bytecode stage:
- `CollectSymbolsVisitor` — populates symbol tables.
- `PreAnalysisVisitor` — pre-checks (see [project_doc/PRE_ANALYSIS_VISITOR_GUIDE.md](project_doc/PRE_ANALYSIS_VISITOR_GUIDE.md)).
- `StaticInfoVisitor` — static type / stack-shape information.
- `AstToBytecodeVisitor` — emits bytecode via `BytecodeGenerator`.

### Bytecode layer ([src/bytecode/](src/bytecode/))
`BytecodeGenerator` emits ops defined in `bytecode_opcodes.*` / `byt_defs.h`. Stack and scope state is tracked by `Scope` + `symtab` so that high-level variables are modeled as labeled stack positions — see [project_doc/DESIGN_PHILOSOPHY.md](project_doc/DESIGN_PHILOSOPHY.md) ("zero-cost rename" model: variable assignment is relabeling, not data movement) and [project_doc/STACK_MANAGEMENT_MECHANISM.md](project_doc/STACK_MANAGEMENT_MECHANISM.md). `TypeValidator` enforces static type rules; `ScriptDecoder` disassembles emitted bytecode. Built-in functions/structs are declared in `bytecode_builtin_*.h`.

### Debugger ([src/debugger/](src/debugger/), gated by `-DBUILD_DEBUGGER=ON` + `ENABLE_DEBUGGER`)
- `core/` — debugger core + enhanced debug-info model.
- `vm/bvm_simulator` — BVM execution simulator with `stack_state` tracking.
- `info/` — debug info generation consumed by `--debug-output`.
- `breakpoint/`, `inspector/` (variable / scope / expression evaluator), `interface/cli_debugger` — interactive CLI driven by `--debug`.

### Cross-cutting
- [src/config/](src/config/) — `ConfigManager` reads `user_preferences.json` (default log level, JSON indent, max errors, strict mode).
- [src/error/](src/error/) — `ErrorManager`; see [project_doc/ERROR_HANDLING_COMPLETE_GUIDE.md](project_doc/ERROR_HANDLING_COMPLETE_GUIDE.md).
- [src/log/logger](src/log/) — global logger; level set via `-l`/`--log-level` or `user_preferences.json`.
- CMake injects `PROJECT_VERSION`, `GIT_COMMIT_HASH`, `GIT_BRANCH_NAME`, `BUILD_TYPE`, `EXECUTABLE_NAME` as compile-time macros consumed by `CompilerInfo` / `Help`.

## Language & docs

Grammar and built-ins are documented under [doc/](doc/): `GRAMMAR_SPECIFICATION.md`, `GRAMMAR_EXAMPLES.md`, `BUILTIN_FUNCTION_DOC.md`, `BUILTIN_OBJECT_DOC.md`. Deeper design rationale (ownership model, negative-number handling, stack management, Bitcoin-Script compatibility) lives in [project_doc/](project_doc/).

## Conventions

- 所有面向用户的回答一律使用中文。
- 会话名称（ai-title / session title）一律使用中文。
- Comments and many log strings are in Chinese; follow the surrounding style when editing.
- When adding a source file, also register it in the `add_executable(...)` list in [CMakeLists.txt](CMakeLists.txt) (debugger files go in `DEBUGGER_SOURCES`, gated by `BUILD_DEBUGGER`).
- The `--asa` / `--allow-subscope-altstack` flag loosens a real safety check — don't flip its default without reading the stack-management doc.
