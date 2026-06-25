# Installation

---

UTXO_Compiler is best installed in one of two ways: building from source (recommended for developers — gives you the latest features) or using a pre-built release package (good for users who just want to try it).

---

## 📋 Supported Platforms

| Platform | Build from source | Pre-built package |
|----------|-------------------|-------------------|
| **Linux x86_64** | ✅ | ✅ |
| **Windows 64-bit** | ✅ (MinGW-w64 cross-compile) | ✅ |
| **Windows 32-bit** | ✅ (MinGW-w64 cross-compile) | ✅ |
| **macOS** | 🔄 planned | 🔄 planned |

---

## 🔨 Method 1: Build from Source (recommended)

### Dependencies

- **CMake** ≥ 3.28
- **C++20** compiler (GCC ≥ 11, Clang ≥ 14)
- **Git** (for cloning the repo and capturing version info)
- **nlohmann/json**: CMake auto-fetches it via `FetchContent`; no manual install required

### Basic flow

```bash
git clone <repo-url> AtomicProofCompiler
cd AtomicProofCompiler

mkdir build && cd build
cmake ..                        # Release by default
cmake --build . -j              # Parallel build

./bin/utxo_interpreter --version   # Sanity check
```

After a successful build, the executable lives at `build/bin/utxo_interpreter`.

### Common CMake options

| Option | Default | Purpose |
|--------|---------|---------|
| `-DCMAKE_BUILD_TYPE=Debug` | Release | Enable debug symbols, disable optimizations |
| `-DBUILD_DEBUGGER=OFF` | ON | Skip building the interactive debugger (smaller binary) |
| `-DUSE_GITEE_MIRROR=ON` | OFF | Pull nlohmann/json from a Gitee mirror (faster in CN) |
| `-DUSE_SYSTEM_JSON=ON` | OFF | Use a system-installed `nlohmann_json` instead of fetching |
| `-DJSON_LOCAL_PATH=/path` | — | Point at a locally pre-staged copy of nlohmann/json |
| `-DCROSS_COMPILE_WINDOWS=ON` | OFF | Cross-compile for Windows via MinGW-w64 |

Examples:

```bash
# Debug build during development
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Speed up dependency fetch in CN networks
cmake .. -DUSE_GITEE_MIRROR=ON
```

Build logs are mirrored to `utxo_interpreter.log` for diagnosing CMake-stage issues.

### Cross-platform build script

`scripts/cross-platform-builder.sh` wraps the multi-platform build into a single command. Its outputs land under `builds/{linux,windows-64,windows-32}/bin/` — that is the layout the `./builds/...` paths in the "Testing and Verification" section below assume.

---

## 📦 Method 2: Use a Pre-built Release Package

If you don't intend to modify the compiler source, just download a release package and unzip it.

### Release Package Structure

#### Linux Package Structure
```
utxo_interpreter-v1.0.0-linux/
├── utxo_interpreter        # Executable
├── doc/                 # Documentation directory
├── install.sh           # Installation script
└── VERSION              # Version information
```

#### Windows Package Structure
```
utxo_interpreter-v1.0.0-windows-64/32/
├── utxo_interpreter.exe    # Windows executable
├── libstdc++-6.dll      # ✨ C++ standard library
├── libgcc_s_seh-1.dll   # ✨ GCC runtime library
├── libwinpthread-1.dll  # ✨ Multi-threading support library
├── doc/                 # Documentation directory
├── install.bat          # Windows installation script
├── DEPENDENCIES.txt     # ✨ Dependency library description
└── VERSION              # Version information
```

---

## 🧪 Testing and Verification

### Verify a from-source build

```bash
# Inside the build directory
cd build

# Print version (includes git commit hash)
./bin/utxo_interpreter --version

# Compile a sample contract
./bin/utxo_interpreter ../test/contract_file/counter.ct
```

A successful run produces a `counter.json` in the current directory containing the compiled bytecode and ABI.

### Test the Linux release package

```bash
# Run directly
./builds/linux/bin/utxo_interpreter --version

# Test compilation
./builds/linux/bin/utxo_interpreter your-script.ct
```

### Test the Windows release package (with Wine on Linux)

```bash
# Install Wine (if not installed)
sudo apt install wine

# Test Windows 64-bit version
wine ./builds/windows-64/bin/utxo_interpreter.exe --version

# Test Windows 32-bit version
wine ./builds/windows-32/bin/utxo_interpreter.exe --version
```

### Verify Windows Package Dependencies

```bash
# Check DLL dependencies of the Windows executable
x86_64-w64-mingw32-objdump -p builds/windows-64/bin/utxo_interpreter.exe | grep "DLL Name"

# Verify DLLs included in the package
unzip -l dist/utxo_interpreter-v*-windows-64.zip | grep "\.dll"
```

---

## 🛠 Common Build Issues

**1. CMake cannot find `nlohmann/json`**

CMake fetches `nlohmann/json` online by default through `FetchContent`. In offline environments use `-DUSE_SYSTEM_JSON=ON` or `-DJSON_LOCAL_PATH=/path/to/json`.

**2. Dependency download stalls**

Pass `-DUSE_GITEE_MIRROR=ON` to switch to a Gitee mirror (faster from China).

**3. The compiler requires C++20**

Older GCC (< 11) or Clang (< 14) will report errors mentioning `coroutine` / `concepts`. Upgrade or install `g++-11` and re-run the build.

**4. Cross-compile a Windows binary on Linux**

Install MinGW-w64 (`sudo apt install mingw-w64` on Debian/Ubuntu), then re-run CMake with `-DCROSS_COMPILE_WINDOWS=ON`.

---

## Next Steps

- [Bitcoin Basics](./bitcoin-basics.md) — Learn how UTXO and BVM work

---

[🇨🇳 中文版](../zh/installation.md)
