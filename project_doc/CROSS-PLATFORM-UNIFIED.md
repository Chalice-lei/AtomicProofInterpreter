# 🌍 跨平台编译完整指南

---

## 🚀 快速开始（3步完成）

```bash
# 1️⃣ 安装交叉编译工具链（包括C++支持）
sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 g++-mingw-w64-x86-64 g++-mingw-w64-i686

# 2️⃣ 编译所有平台
./scripts/cross-platform-builder.sh all --release

# 3️⃣ 打包发布（自动包含Windows依赖库）
./scripts/cross-platform-packager.sh all
```

**获得的发布产物：**
```
dist/
├── Compiler-v1.0.0-linux.tar.gz           # Linux版本
├── Compiler-v1.0.0-windows-64.zip         # Windows 64位版本 ✨ 包含依赖库
├── Compiler-v1.0.0-windows-32.zip         # Windows 32位版本 ✨ 包含依赖库
└── Compiler-v1.0.0-multiplatform.tar.gz   # 多平台合集
```

**✨ 新功能：单平台独立构建**
```bash
# 只构建和打包Windows版本（无需先构建Linux）
./scripts/cross-platform-builder.sh windows-64
./scripts/cross-platform-packager.sh single --platform windows-64

# 只构建和打包Linux版本
./scripts/cross-platform-builder.sh linux
./scripts/cross-platform-packager.sh single --platform linux
```

---

## 📋 支持的平台和解决方案

### 支持的平台
- ✅ **Linux** (本地编译)
- ✅ **Windows 64位** (交叉编译)
- ✅ **Windows 32位** (交叉编译)
- 🔄 **macOS** (计划支持)

### 四种解决方案对比

| 方案 | 优点 | 缺点 | 适用场景 |
|------|------|------|----------|
| **交叉编译** ⭐ | 快速、一台机器、自动化友好 | 需安装工具链 | 日常开发、CI/CD |
| **Docker** | 环境隔离、无需安装依赖 | 需要Docker | 团队协作、一致环境 |
| **GitHub Actions** | 全自动、多平台并行 | 依赖网络 | 开源项目、发布管理 |
| **分平台编译** | 最兼容 | 需要多台机器 | 特殊需求 |

---

## 🔧 详细配置指南

### 1. 环境准备

#### Ubuntu/Debian系统

```bash
# 更新包管理器
sudo apt update

# 安装基础编译工具
sudo apt install build-essential cmake

# ⚠️ 重要：安装完整的Windows交叉编译工具链（包括C++支持）
sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 g++-mingw-w64-x86-64 g++-mingw-w64-i686

# 可选：安装zip工具（用于Windows包）
sudo apt install zip unzip

# 可选：安装wine（用于测试Windows可执行文件）
sudo apt install wine
```

#### CentOS/RHEL/Fedora系统

```bash
# Fedora
sudo dnf install gcc-c++ cmake mingw64-gcc-c++ mingw32-gcc-c++

# CentOS/RHEL (需要EPEL)
sudo yum install epel-release
sudo yum install gcc-c++ cmake mingw64-gcc-c++ mingw32-gcc-c++
```

### 2. 验证环境

```bash
# 检查交叉编译工具链
./scripts/cross-platform-builder.sh --check-deps

# 确认C++编译器可用
which x86_64-w64-mingw32-g++
which i686-w64-mingw32-g++
```

---

## 🎯 编译和打包指南

### 编译命令

```bash
# 编译单个平台
./scripts/cross-platform-builder.sh linux
./scripts/cross-platform-builder.sh windows-64    # 推荐
./scripts/cross-platform-builder.sh windows-32

# 编译所有平台
./scripts/cross-platform-builder.sh all

# Release模式编译
./scripts/cross-platform-builder.sh --release all

# 清理后编译
./scripts/cross-platform-builder.sh --clean --release all

# 指定并行作业数
./scripts/cross-platform-builder.sh --jobs 4 linux
```

### 打包命令

```bash
# 打包所有平台（推荐）
./scripts/cross-platform-packager.sh all

# 打包单个平台
./scripts/cross-platform-packager.sh single --platform linux
./scripts/cross-platform-packager.sh single --platform windows-64
./scripts/cross-platform-packager.sh single --platform windows-32

# 创建多平台合集包
./scripts/cross-platform-packager.sh multi

# 最小化包（仅可执行文件）
./scripts/cross-platform-packager.sh --minimal all

# 不包含文档
./scripts/cross-platform-packager.sh --no-docs all
```

### 🌟 Windows依赖库自动包含

打包脚本现已支持**自动检测并包含Windows运行时依赖库**：

- `libstdc++-6.dll` - C++标准库
- `libgcc_s_seh-1.dll` - GCC运行时库  
- `libwinpthread-1.dll` - 多线程支持库

---

## 📁 发布产物结构

### Linux包结构
```
Compiler-v1.0.0-linux/
├── Compiler              # 可执行文件
├── doc/                  # 文档目录
├── install.sh           # 安装脚本
└── VERSION              # 版本信息
```

### Windows包结构
```
Compiler-v1.0.0-windows-64/
├── Compiler.exe          # Windows可执行文件
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

### 测试Linux版本

```bash
# 直接运行
./builds/linux/bin/Compiler --version

# 测试编译功能
./builds/linux/bin/Compiler your-script.ct
```

### 测试Windows版本（使用Wine）

```bash
# 安装Wine（如果未安装）
sudo apt install wine

# 测试Windows 64位版本
wine ./builds/windows-64/bin/Compiler.exe --version

# 测试Windows 32位版本
wine ./builds/windows-32/bin/Compiler.exe --version
```

### 验证Windows包依赖库

```bash
# 检查Windows可执行文件依赖的DLL
x86_64-w64-mingw32-objdump -p builds/windows-64/bin/Compiler.exe | grep "DLL Name"

# 验证打包包含的DLL
unzip -l dist/Compiler-v*-windows-64.zip | grep "\.dll"
```

---

## 🔄 其他解决方案

### 🐳 Docker方案（无需安装依赖）

```bash
# 一键Docker编译
./scripts/docker-build.sh
```

优点：
- ✅ 无需安装mingw-w64
- ✅ 环境一致性
- ✅ 团队协作友好

### ⚡ GitHub Actions自动化

```yaml
# .github/workflows/cross-platform-release.yml
name: Cross-Platform Release
on:
  release:
    types: [created]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      
      - name: Install cross-compilation tools
        run: |
          sudo apt update
          sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 g++-mingw-w64-x86-64 g++-mingw-w64-i686
      
      - name: Build all platforms
        run: ./scripts/cross-platform-builder.sh --release all
      
      - name: Package all platforms
        run: ./scripts/cross-platform-packager.sh all
      
      - name: Upload release assets
        uses: actions/upload-release-asset@v1
        with:
          upload_url: ${{ github.event.release.upload_url }}
          asset_path: ./dist/
```

推送标签自动发布：

```bash
# 创建发布标签
git tag v1.0.0
git push origin v1.0.0

# GitHub自动编译并发布所有平台版本
```

---

## 🎯 实际应用场景

### 场景1：为客户提供Windows版本

```bash
# 1. 在Linux服务器上编译Windows版本
./scripts/cross-platform-builder.sh --release windows-64

# 2. 打包Windows版本（自动包含依赖库）
./scripts/cross-platform-packager.sh single --platform windows-64

# 3. 交付给客户
# 客户下载 Compiler-v1.0.0-windows-64.zip
# 解压后直接运行 Compiler.exe（无需安装额外运行时）
```

### 场景2：开源项目发布

```bash
# 推送发布标签
git tag v2.0.0
git push origin v2.0.0

# GitHub Actions自动：
# 1. 编译Linux + Windows版本
# 2. 创建GitHub Release
# 3. 上传所有平台的安装包
```

### 场景3：离线环境部署

```bash
# 使用Docker在离线环境编译
docker build -f docker/Dockerfile -t compiler-cross .
docker run --rm -v $PWD/dist:/workspace/dist compiler-cross
```

### 场景4：完整发布流程

```bash
#!/bin/bash
# 完整的跨平台发布流程

echo "🚀 开始跨平台编译和发布流程..."

# 1. 更新版本号
echo "1. 更新版本号 (在CMakeLists.txt中)"
# 手动编辑 CMakeLists.txt 修改版本信息

# 2. 清理并编译所有平台
echo "2. 编译所有平台..."
./scripts/cross-platform-builder.sh --clean --release all

# 3. 测试编译产物
echo "3. 测试编译产物..."
./builds/linux/bin/Compiler --version
wine ./builds/windows-64/bin/Compiler.exe --version 2>/dev/null || echo "Wine未安装，跳过Windows测试"

# 4. 打包所有平台（自动包含依赖库）
echo "4. 打包发布产物..."
./scripts/cross-platform-packager.sh --clean all

# 5. 验证发布包
echo "5. 验证发布包..."
ls -lh dist/

# 6. 验证Windows包包含依赖库
echo "6. 验证Windows依赖库..."
unzip -l dist/Compiler-v*-windows-64.zip | grep "\.dll"

echo "✅ 跨平台发布完成！"
echo "📁 发布产物位置: dist/"
```

---

## 🔍 故障排除

### 1. ❌ C++编译器未找到

**错误信息：**
```
CMAKE_CXX_COMPILER: x86_64-w64-mingw32-g++ is not a full path and was not found in the PATH
```

**解决方案：**
```bash
# Ubuntu/Debian - 安装完整的C++工具链
sudo apt install g++-mingw-w64-x86-64 g++-mingw-w64-i686

# Fedora
sudo dnf install mingw64-gcc-c++ mingw32-gcc-c++

# 验证安装
which x86_64-w64-mingw32-g++
which i686-w64-mingw32-g++
```

### 2. ❌ 跨平台API兼容性错误

**错误信息：**
```
'localtime_r' was not declared in this scope; did you mean 'localtime_s'?
```

**解决方案：**
在代码中使用条件编译处理平台差异：
```cpp
#ifdef _WIN32
    localtime_s(&tm, &now_time);
#else
    localtime_r(&now_time, &tm);
#endif
```

### 3. ❌ Windows运行时缺少DLL

**错误信息：**
```
找不到 libstdc++-6.dll
应用程序无法正常启动(0xc000007b)
```

**解决方案：**
- ✅ **推荐：** 使用我们的打包脚本，自动包含依赖库
- 或手动复制DLL文件到可执行文件目录
- 或在目标系统安装MinGW-w64运行时

### 4. ❌ Windows运行时库版本不匹配

**错误信息：**
```
无法定位程序输入点__gthr_win32_self于动态连接库 libgcc_s_seh-1.dll 上
```

**原因分析：**
- 编译器使用 `13-win32` 版本的工具链
- 但打包时复制了 `13-posix` 版本的DLL文件
- 两个版本的线程模型不兼容（win32 vs posix）

**解决方案：**
```bash
# 检查编译器版本
x86_64-w64-mingw32-gcc --version
# 输出: x86_64-w64-mingw32-gcc (GCC) 13-win32

# 确保打包脚本使用匹配的DLL版本
# 编辑 scripts/cross-platform-packager.sh，确保路径为：
# /usr/lib/gcc/x86_64-w64-mingw32/13-win32/libgcc_s_seh-1.dll
# 而不是：
# /usr/lib/gcc/x86_64-w64-mingw32/13-posix/libgcc_s_seh-1.dll

# 验证DLL包含正确的函数
x86_64-w64-mingw32-objdump -p /usr/lib/gcc/x86_64-w64-mingw32/13-win32/libgcc_s_seh-1.dll | grep "__gthr_win32_self"
```

**预防措施：**
我们的打包脚本已修复此问题，会自动选择正确版本的DLL文件。

### 5. ❌ 可执行文件双重扩展名

**问题：** 生成`Compiler.exe.exe`文件

**解决方案：**
在CMakeLists.txt中，让CMake自动处理扩展名：
```cmake
# ✅ 正确做法 - 指定基础名称，让 CMake 自动添加 .exe
set(EXECUTABLE_NAME "utxo_Interpreter")

# ❌ 错误做法 - 手动添加.exe会导致双重扩展名
# if(WIN32)
#     set(EXECUTABLE_NAME "${PROJECT_NAME}.exe")
# endif()
```

### 6. ❌ nlohmann/json交叉编译问题

**错误信息：**
```
Could not find nlohmann_json for Windows target
```

**解决方案：**
项目已配置为自动下载源码，无需额外配置。如遇网络问题：
```bash
# 使用Gitee镜像
cmake .. -DUSE_GITEE_MIRROR=ON -DCROSS_COMPILE_TARGET=x86_64-w64-mingw32
```

### 7. ❌ Wine测试失败

**错误信息：**
```
wine: command not found
```

**解决方案：**
```bash
# 安装Wine（可选，仅用于测试）
sudo apt install wine

# 或者在实际Windows环境中测试
```

### 8. ❌ 可执行文件过大

**问题：** Windows可执行文件体积较大

**解决方案：**
```bash
# 使用Release模式并启用优化
./scripts/cross-platform-builder.sh --release windows

# 检查文件大小
ls -lh builds/windows-64/bin/

# 可选：使用strip减小体积（会移除调试信息）
x86_64-w64-mingw32-strip builds/windows-64/bin/Compiler.exe
```

### 9. ❌ 打包脚本要求必须先构建Linux版本

**错误信息：**
```
[ERROR] 无法找到可执行文件获取版本信息
[INFO] 请先运行: ./scripts/cross-platform-builder.sh
```

**问题原因：**
- 旧版本的打包脚本硬性依赖Linux版本的可执行文件来获取版本信息
- 当用户只想打包Windows版本时（`./scripts/cross-platform-builder.sh windows-64`），打包脚本无法工作

**解决方案：**
✅ **已修复！** 新版本的打包脚本支持多种版本获取方式：

1. **优先使用Linux版本**（如果存在）
2. **从Windows可执行文件中解析版本信息**（无需wine）
3. **使用Wine运行Windows版本**（如果安装了wine）
4. **从CMakeLists.txt中获取版本信息**（备用方案）

现在可以直接使用：
```bash
# 只构建和打包Windows版本
./scripts/cross-platform-builder.sh windows-64
./scripts/cross-platform-packager.sh single --platform windows-64
```

**版本获取策略：**
```bash
# 方式1: 从可执行文件字符串中提取（使用strings命令）
strings builds/windows-64/bin/Compiler.exe | grep -E "v[0-9]+\.[0-9]+\.[0-9]+"

# 方式2: 从CMakeLists.txt中解析
grep "VERSION" CMakeLists.txt

# 方式3: 使用Wine运行（如果可用）
wine builds/windows-64/bin/Compiler.exe --version
```

### 🆘 调试技巧

#### 详细编译日志

```bash
# 启用详细输出
./scripts/cross-platform-builder.sh --verbose windows-64

# 或直接使用CMake
cd builds/windows-64
make VERBOSE=1
```

---

## 📚 参考资料

### 交叉编译技术文档
- [MinGW-w64官方文档](https://www.mingw-w64.org/)
- [CMake交叉编译指南](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)

### 相关工具
- **MinGW-w64**: Windows交叉编译工具链
- **Wine**: Linux上运行Windows程序
- **CMake**: 跨平台构建系统
- **objdump**: 查看二进制文件信息

### 平台兼容性编程
- [Microsoft Windows API文档](https://docs.microsoft.com/en-us/windows/win32/)
- [POSIX标准参考](https://pubs.opengroup.org/onlinepubs/9699919799/)

---

## ✨ 总结

**🎉 恭喜！您现在可以在Linux环境中编译生产就绪的Windows程序了！**

**您现在可以：**

1. ✅ **在Linux上编译Windows程序**（交叉编译）
2. ✅ **一键生成多平台发布包**（自动包含Windows依赖库）
3. ✅ **自动化CI/CD发布流程**
4. ✅ **为用户提供即用的安装包**

**✨ 新功能亮点：**
- 自动包含Windows运行时依赖库
- 详细的故障排除指南  
- 完整的平台兼容性支持
- 修复了线程库版本匹配问题
- ✅ **支持单平台独立构建和打包**（无需先构建Linux版本）
- 🔧 **智能版本信息获取**（支持从CMake、可执行文件等多种方式获取）

**无需Windows机器，无需复杂配置！** 🎉

有任何问题请查看故障排除部分或提交Issue。
