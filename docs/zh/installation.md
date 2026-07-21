# 安装

---

UTXO_Compiler 推荐通过两种方式获取：从源码构建（开发者首选，得到最新功能）和使用预编译发布包（适合只想试用的用户）。

---

## 📋 支持的平台

| 平台 | 源码构建 | 预编译包 |
|------|---------|---------|
| **Linux x86_64** | ✅ | ✅ |
| **Windows 64 位** | ✅（MinGW-w64 交叉编译） | ✅ |
| **Windows 32 位** | ✅（MinGW-w64 交叉编译） | ✅ |
| **macOS** | 🔄 计划支持 | 🔄 计划支持 |

---

## 🔨 方法一：从源码构建（推荐）

### 依赖

- **CMake** ≥ 3.28
- **C++20** 编译器（GCC ≥ 11、Clang ≥ 14）
- **Git**（用于克隆仓库及获取版本信息）
- **nlohmann/json**：CMake 默认会通过 `FetchContent` 自动下载，无需手动安装

### 基本流程

```bash
git clone <repo-url> AtomicProofCompiler
cd AtomicProofCompiler

mkdir build && cd build
cmake ..                        # 默认 Release 构建
cmake --build . -j              # 多核并行编译

./bin/utxo_Interpreter --version   # 验证可执行文件
```

构建成功后，可执行文件位于 `build/bin/utxo_Interpreter`。

### 常用 CMake 选项

| 选项 | 默认值 | 用途 |
|------|--------|------|
| `-DCMAKE_BUILD_TYPE=Debug` | Release | 启用调试符号、关闭优化 |
| `-DBUILD_DEBUGGER=OFF` | ON | 不编译交互式调试器（缩减二进制体积） |
| `-DUSE_GITEE_MIRROR=ON` | OFF | 在国内网络下使用 Gitee 镜像下载 nlohmann/json |
| `-DUSE_SYSTEM_JSON=ON` | OFF | 使用系统已安装的 `nlohmann_json` 而不是下载 |
| `-DJSON_LOCAL_PATH=/path` | — | 指向本地预放置的 nlohmann/json 源码 |
| `-DCROSS_COMPILE_WINDOWS=ON` | OFF | 通过 MinGW-w64 交叉编译出 Windows 可执行文件 |

示例：

```bash
# 开发期使用调试构建
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 国内环境加速依赖下载
cmake .. -DUSE_GITEE_MIRROR=ON
```

构建过程的日志会同时输出到 `utxo_Interpreter.log`，方便排查 CMake 阶段的问题。

### 跨平台构建脚本

仓库 `scripts/cross-platform-builder.sh` 封装了一键构建多个平台的流程，产物会按平台落到 `builds/{linux,windows-64,windows-32}/bin/` 目录下。下面"测试和验证"小节中出现的 `./builds/...` 路径就是这种产物布局。

---

## 📦 方法二：使用预编译发布包

如果你不打算修改编译器源码，可以直接下载发布包，解压后即可使用。

### 发布包结构

#### Linux 包结构
```
utxo_interpreter-v1.0.0-linux/
├── utxo_Interpreter        # 可执行文件
├── doc/                 # 文档目录
├── install.sh           # 安装脚本
└── VERSION              # 版本信息
```

#### Windows 包结构
```
utxo_interpreter-v1.0.0-windows-64/32/
├── utxo_Interpreter.exe    # Windows可执行文件
├── libstdc++-6.dll      # ✨ C++标准库
├── libgcc_s_seh-1.dll   # ✨ GCC运行时库
├── libwinpthread-1.dll  # ✨ 多线程支持库
├── doc/                 # 文档目录
├── install.bat          # Windows安装脚本
├── DEPENDENCIES.txt     # ✨ 依赖库说明
└── VERSION              # 版本信息
```

---

## 🧪 测试和验证

### 验证源码构建产物

```bash
# 进入构建目录
cd build

# 查看版本（含 Git 提交 hash）
./bin/utxo_Interpreter --version

# 编译一个示例合约
./bin/utxo_Interpreter ../test/contract_file/counter.ct
```

正常情况下你会在当前目录看到一个 `counter.json`，里面包含编译输出的字节码与 ABI。

### 常用命令行参数

`utxo_Interpreter` 的命令格式是：

```bash
utxo_Interpreter [options] filename.ct
```

常用参数如下：

| 参数 | 用途 |
|------|------|
| `-h`, `--help` | 显示帮助信息 |
| `-v`, `--version` | 显示版本、目标架构和能力信息 |
| `-l`, `--log-level <level>` | 设置日志级别：`debug`、`info`、`warning`、`error`、`critical`、`none` |
| `-d` | 生成调试信息，但不进入交互式调试器 |
| `--debug` | 编译并启动交互式调试器 |
| `--debug-output <file>` | 指定调试信息输出文件 |
| `--allow-subscope-altstack`, `--asa` | 允许在 `if/else`、私有函数等子作用域中使用 `SetAlt` / `SetMain` |

示例：

```bash
# 编译合约，输出 counter.json
./bin/utxo_Interpreter ../test/contract_file/counter.ct

# 输出详细日志，便于排查语义分析或字节码生成问题
./bin/utxo_Interpreter --log-level debug ../test/contract_file/counter.ct

# 进入交互式调试器
./bin/utxo_Interpreter --debug ../test/contract_file/counter.ct
```

编译输出文件默认写入当前工作目录，文件名取自输入文件名。例如编译 `counter.ct` 会生成 `counter.json`。

### 测试 Linux 发布包

```bash
# 直接运行
./builds/linux/bin/utxo_Interpreter --version

# 测试编译功能
./builds/linux/bin/utxo_Interpreter your-script.ct
```

### 测试 Windows 版本（在 Linux 下用 Wine）

```bash
# 安装Wine（如果未安装）
sudo apt install wine

# 测试Windows 64位版本
wine ./builds/windows-64/bin/utxo_Interpreter.exe --version

# 测试Windows 32位版本
wine ./builds/windows-32/bin/utxo_Interpreter.exe --version
```

### 验证 Windows 包依赖库

```bash
# 检查 Windows 可执行文件依赖的 DLL
x86_64-w64-mingw32-objdump -p builds/windows-64/bin/utxo_Interpreter.exe | grep "DLL Name"

# 验证打包包含的 DLL
unzip -l dist/utxo_interpreter-v*-windows-64.zip | grep "\.dll"
```

---

## 🛠 常见构建问题

**1. CMake 报告找不到 `nlohmann/json`**

CMake 默认会通过 `FetchContent` 在线下载 `nlohmann/json`。若你在离线环境，请改用 `-DUSE_SYSTEM_JSON=ON` 或 `-DJSON_LOCAL_PATH=/path/to/json`。

**2. 构建过程中下载依赖卡住**

国内网络下可加 `-DUSE_GITEE_MIRROR=ON` 切换到 Gitee 镜像。

**3. 编译器要求 C++20**

老版本 GCC（< 11）或 Clang（< 14）会报告 `coroutine` / `concepts` 相关错误，请升级编译器或安装 `g++-11` 之后再次构建。

**4. 想在 Linux 上交叉编译出 Windows 可执行文件**

安装 `mingw-w64`（Debian/Ubuntu：`sudo apt install mingw-w64`），然后用 `-DCROSS_COMPILE_WINDOWS=ON` 重新配置 CMake。

**5. `import std.p2pkh` 报找不到标准库**

编译器会按以下顺序寻找 `stdlib`：`APC_STDLIB_PATH` 环境变量、`user_preferences.json` 中的 `paths.stdlib`、可执行文件周边目录、当前工作目录下的 `stdlib`。如果你移动了二进制文件，请同时移动 `stdlib/`，或显式设置：

```bash
export APC_STDLIB_PATH=/path/to/AtomicProofCompiler/stdlib
```

---

## 下一步

- [比特币基础](./bitcoin-basics.md) — 了解 UTXO 和 BVM 的工作原理

---

[🇬🇧 English version](../en/installation.md)
