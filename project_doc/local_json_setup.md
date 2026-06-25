# 使用本地 nlohmann/json 库的完整指南

## 📋 概述

本指南展示如何手动下载 nlohmann/json 库并在 AtomicProof Compiler 项目中使用，这对以下场景特别有用：
- 🔒 离线开发环境
- 🚀 CI/CD 构建流水线
- 🎯 特定版本控制需求
- ⚡ 避免每次重新下载

## 🚀 方法一：下载完整源码

### 1. 下载源码
```bash
# 方式1: 使用 git clone
git clone https://github.com/nlohmann/json.git
cd json
git checkout v3.11.3  # 切换到指定版本

# 方式2: 下载压缩包
wget https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
tar -xzf v3.11.3.tar.gz
```

### 2. 使用本地库构建项目
```bash
# 进入 Compiler 项目目录
cd /path/to/compiler

# 创建构建目录
mkdir build && cd build

# 使用本地 JSON 库路径配置
cmake .. -DJSON_LOCAL_PATH=/path/to/json

# 示例：
cmake .. -DJSON_LOCAL_PATH=/home/user/downloads/json
```

## 📄 方法二：仅下载头文件

### 1. 下载单头文件
```bash
# 创建目录
mkdir -p ~/local_libs/nlohmann-json

# 下载单头文件
wget https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
     -O ~/local_libs/nlohmann-json/json.hpp
```

### 2. 使用单头文件构建
```bash
cd /path/to/compiler
mkdir build && cd build

# 使用单头文件路径
cmake .. -DJSON_LOCAL_PATH=/home/user/local_libs/nlohmann-json
```

## 🎯 支持的目录结构

CMake 配置会自动检测以下三种目录结构：

### 结构1: 标准安装结构
```
/path/to/json/
├── include/
│   └── nlohmann/
│       └── json.hpp
└── ...
```

### 结构2: 源码单头文件结构
```
/path/to/json/
├── single_include/
│   └── nlohmann/
│       └── json.hpp
└── ...
```

### 结构3: 直接头文件
```
/path/to/json/
├── json.hpp
└── ...
```

## 💡 完整使用示例

```bash
#!/bin/bash
# 完整的离线构建脚本

# 1. 准备本地 JSON 库
echo "📦 下载 nlohmann/json..."
mkdir -p ~/local_deps
cd ~/local_deps
git clone https://github.com/nlohmann/json.git
cd json && git checkout v3.11.3 && cd ..

# 2. 构建 Compiler 项目
echo "🔨 构建 Compiler..."
cd /path/to/compiler
mkdir -p build && cd build

# 使用本地库构建
cmake .. \
    -DJSON_LOCAL_PATH=~/local_deps/json \
    -DCMAKE_BUILD_TYPE=Release

# 编译
make -j$(nproc)

echo "✅ 构建完成！"
```

## 🎯 真实成功案例

基于实际验证的成功案例：

```bash
# 示例路径：/home/user/compiler/json
# 目录结构：包含 include/nlohmann/json.hpp

cd /home/user/compiler
mkdir build && cd build

# 配置（成功验证）
cmake .. -DJSON_LOCAL_PATH=/home/user/compiler/json

# 实际输出：
# -- 📁 正在使用本地 nlohmann/json 库...
# --    📍 本地路径: /home/user/compiler/json
# --    📋 检测到标准安装结构
# -- ✅ 本地 nlohmann/json 配置完成！

# 并行构建（成功验证）
cmake --build . --parallel

# 结果：生成 bin/Compiler (1.3MB)
# 测试：./bin/Compiler --version ✅
```

## 🔧 高级配置选项

### 组合使用各种选项
```bash
# Debug 模式 + 本地库
cmake .. -DJSON_LOCAL_PATH=/path/to/json -DCMAKE_BUILD_TYPE=Debug

# 结合其他选项（注意：本地路径优先级最高）
cmake .. \
    -DJSON_LOCAL_PATH=/path/to/json \
    -DUSE_SYSTEM_JSON=ON \          # 会被忽略
    -DUSE_GITEE_MIRROR=ON           # 会被忽略
```

### 验证配置
构建时会看到类似输出：
```
🔧 Compiler v1.0.0 配置摘要:
   📁 项目名称: Compiler
   📍 可执行文件: Compiler
   🔨 构建类型: Release
   📊 C++ 标准: C++20
   🏗️  编译器: GNU 11.4.0
   📚 JSON库方式: 本地路径

📁 正在使用本地 nlohmann/json 库...
   📍 本地路径: /path/to/json
   📋 检测到标准安装结构        # 根据实际目录结构显示不同类型
✅ 本地 nlohmann/json 配置完成！
```

**注意**：检测信息会根据实际目录结构显示：
- `📋 检测到标准安装结构` - 当路径包含 `include/nlohmann/json.hpp`
- `📋 检测到单头文件结构` - 当路径包含 `single_include/nlohmann/json.hpp`  
- `📋 检测到直接头文件` - 当路径直接包含 `json.hpp`

## ⚠️ 常见问题

### 1. 路径不存在错误
```
❌ 错误: 指定的本地路径不存在: /wrong/path
```
**解决**: 检查路径是否正确，使用绝对路径。

### 2. 找不到头文件错误
```
❌ 错误: 在指定路径中未找到 nlohmann/json.hpp 文件
```
**解决**: 确保路径中包含正确的 json.hpp 文件。

### 3. 权限问题
**解决**: 确保对指定路径有读取权限。

## 🎉 优势

✅ **离线构建**: 无需网络连接  
✅ **版本控制**: 精确控制库版本  
✅ **构建速度**: 避免重复下载  
✅ **自定义修改**: 可以使用修改过的库版本  
✅ **CI/CD友好**: 适合自动化构建流程 