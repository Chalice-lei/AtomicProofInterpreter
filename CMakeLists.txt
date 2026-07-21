# =============================================================================
# AtomicProof Compiler (apc) - CMake Configuration
# =============================================================================
# 项目描述: AtomicProof Compiler
# 版本: 1.0.0
# 语言: C++20
# 最低CMake版本: 3.28
#
# 🚀 构建选项说明:
#   -DUSE_SYSTEM_JSON=ON                    使用系统安装的 nlohmann/json
#   -DUSE_GITEE_MIRROR=ON                   使用 Gitee 镜像（国内用户推荐）
#   -DJSON_LOCAL_PATH=/path/to/json         指定本地 nlohmann/json 库路径
#   -DCMAKE_BUILD_TYPE=Debug                设置构建类型为 Debug
#   -DCMAKE_BUILD_TYPE=Release              设置构建类型为 Release
#   -DCROSS_COMPILE_WINDOWS=ON              交叉编译Windows目标
#   -DCROSS_COMPILE_TARGET=<target>         指定交叉编译目标
#
# 📖 使用示例:
#   mkdir build && cd build
#   cmake .. -DUSE_GITEE_MIRROR=ON                    # 使用镜像源
#   cmake .. -DUSE_SYSTEM_JSON=ON                     # 使用系统库
#   cmake .. -DJSON_LOCAL_PATH=/path/to/json-lib      # 使用本地库
#   cmake .. -DCMAKE_BUILD_TYPE=Debug                 # Debug模式
#   cmake .. -DCROSS_COMPILE_WINDOWS=ON               # 交叉编译Windows
#   cmake .. -DCROSS_COMPILE_TARGET=x86_64-w64-mingw32 # 指定交叉编译工具链
# =============================================================================

cmake_minimum_required(VERSION 3.28)

# =============================================================================
# 跨平台编译配置
# =============================================================================

# 交叉编译选项
option(CROSS_COMPILE_WINDOWS "Cross compile for Windows" OFF)
set(CROSS_COMPILE_TARGET "" CACHE STRING "Cross compilation target (e.g., x86_64-w64-mingw32)")

# 检测和配置交叉编译环境
if(CROSS_COMPILE_WINDOWS OR CROSS_COMPILE_TARGET)
    message(STATUS "🔧 配置跨平台编译...")
    
    # 自动检测可用的Windows交叉编译工具链
    if(NOT CROSS_COMPILE_TARGET)
        # 尝试常见的MinGW-w64工具链
        set(MINGW_TARGETS 
            "x86_64-w64-mingw32"
            "i686-w64-mingw32"
            "x86_64-w64-mingw32-gcc"
        )
        
        foreach(target ${MINGW_TARGETS})
            find_program(MINGW_COMPILER ${target}-gcc)
            if(MINGW_COMPILER)
                set(CROSS_COMPILE_TARGET ${target})
                message(STATUS "   🎯 自动检测到交叉编译工具链: ${target}")
                break()
            endif()
        endforeach()
        
        if(NOT CROSS_COMPILE_TARGET)
            message(FATAL_ERROR "❌ 未找到Windows交叉编译工具链！请安装 mingw-w64")
        endif()
    endif()
    
    # 设置交叉编译工具链
    set(CMAKE_SYSTEM_NAME Windows)
    set(CMAKE_C_COMPILER ${CROSS_COMPILE_TARGET}-gcc)
    set(CMAKE_CXX_COMPILER ${CROSS_COMPILE_TARGET}-g++)
    set(CMAKE_RC_COMPILER ${CROSS_COMPILE_TARGET}-windres)
    
    # 设置查找路径
    set(CMAKE_FIND_ROOT_PATH /usr/${CROSS_COMPILE_TARGET})
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    
    message(STATUS "   ✅ Windows交叉编译配置完成")
    message(STATUS "   🔨 C++ 编译器: ${CMAKE_CXX_COMPILER}")
    set(TARGET_PLATFORM "Windows")
else()
    set(TARGET_PLATFORM "Native")
endif()

# =============================================================================
# 版本配置系统
# =============================================================================

# 基础版本号（必须是纯数字格式，如 1.0.0）
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)

# 版本后缀（可选，如 alpha, beta, rc1, 空字符串表示正式版）
set(PROJECT_VERSION_SUFFIX "")

# 构建完整版本字符串
if(PROJECT_VERSION_SUFFIX)
    set(PROJECT_VERSION_FULL "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}-${PROJECT_VERSION_SUFFIX}")
else()
    set(PROJECT_VERSION_FULL "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")
endif()

# 项目定义 - 包含版本、描述和语言信息
project(utxo_interpreter
    VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}
    DESCRIPTION "AtomicProof Interpreter."
    LANGUAGES CXX
)

# 可执行文件使用对外名称，CMake 会自动处理平台特定的扩展名
set(EXECUTABLE_NAME "utxo_Interpreter")

# C++ 标准和编译器配置
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =============================================================================
# 配置摘要显示
# =============================================================================

# 确定实际的可执行文件名（用于显示）
set(DISPLAY_EXECUTABLE_NAME "${EXECUTABLE_NAME}")
if(WIN32 OR CROSS_COMPILE_WINDOWS)
    set(DISPLAY_EXECUTABLE_NAME "${EXECUTABLE_NAME}.exe")
endif()

message(STATUS "🔧 ${PROJECT_NAME} v${PROJECT_VERSION_FULL} 配置摘要:")
message(STATUS "   📁 项目名称: ${PROJECT_NAME}")
message(STATUS "   📍 可执行文件: ${DISPLAY_EXECUTABLE_NAME}")
message(STATUS "   🏷️  完整版本: ${PROJECT_VERSION_FULL}")
message(STATUS "   🔢 基础版本: ${PROJECT_VERSION}")
if(PROJECT_VERSION_SUFFIX)
    message(STATUS "   🏷️  版本后缀: ${PROJECT_VERSION_SUFFIX}")
endif()
message(STATUS "   🎯 目标平台: ${TARGET_PLATFORM}")
message(STATUS "   🔨 构建类型: ${CMAKE_BUILD_TYPE}")
message(STATUS "   📊 C++ 标准: C++${CMAKE_CXX_STANDARD}")
message(STATUS "   🏗️  编译器: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# 显示依赖库获取方式
if(JSON_LOCAL_PATH)
    message(STATUS "   📚 JSON库方式: 本地路径")
elseif(USE_SYSTEM_JSON)
    message(STATUS "   📚 JSON库方式: 系统安装")
else()
    if(USE_GITEE_MIRROR)
        message(STATUS "   📚 JSON库方式: 自动下载 (Gitee镜像)")
    else()
        message(STATUS "   📚 JSON库方式: 自动下载 (GitHub官方)")
    endif()
endif()
message(STATUS "")  # 空行分隔

# =============================================================================
# 构建类型配置 - 默认生产环境
# =============================================================================

# 设置默认构建类型为 Release（生产环境优先）
# 开发者可通过 cmake .. -DCMAKE_BUILD_TYPE=Debug 覆盖
message(STATUS "💡 开发者可通过命令: cmake .. -DCMAKE_BUILD_TYPE=Debug | Release | MinSizeRel | RelWithDebInfo 设置构建类型")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    message(STATUS "    🚀 当前构建类型: Release (默认生产环境)")
else()
    message(STATUS "    🚀 当前构建类型: ${CMAKE_BUILD_TYPE} ")
endif()


set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# =============================================================================
# Git信息获取（构建时固定）
# =============================================================================

# 获取Git提交哈希
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# 如果长哈希获取失败，尝试短哈希
if(NOT GIT_COMMIT_HASH)
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

# 获取Git分支名
execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BRANCH_NAME
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# 设置默认值（如果Git信息获取失败）
if(NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "unknown")
    message(STATUS "⚠️  Git提交哈希获取失败，使用默认值: unknown")
else()
    message(STATUS "🔖 Git提交哈希: ${GIT_COMMIT_HASH}")
endif()

if(NOT GIT_BRANCH_NAME)
    set(GIT_BRANCH_NAME "unknown")
    message(STATUS "⚠️  Git分支名获取失败，使用默认值: unknown")
else()
    message(STATUS "🌿 Git分支名: ${GIT_BRANCH_NAME}")
endif()

# 将项目信息和Git信息传递给C++代码
add_compile_definitions(
    BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    EXECUTABLE_NAME="${EXECUTABLE_NAME}"
    PROJECT_VERSION="${PROJECT_VERSION_FULL}"
    PROJECT_DESCRIPTION="${PROJECT_DESCRIPTION}"
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
    GIT_BRANCH_NAME="${GIT_BRANCH_NAME}"
)

# =============================================================================
# 第三方依赖管理
# =============================================================================

# 包含 FetchContent 模块
include(FetchContent)

# 设置 FetchContent 全局选项
set(FETCHCONTENT_QUIET OFF)  # 显示下载进度
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)  # 离线模式，避免每次都检查更新

# 添加镜像源选择选项（针对网络较慢的用户）
option(USE_GITEE_MIRROR "Use Gitee mirror for faster download in China" OFF)

# 设置下载源
if(USE_GITEE_MIRROR)
    set(JSON_GIT_REPOSITORY "https://gitee.com/mirrors/json.git")
    message(STATUS "🇨🇳 使用 Gitee 镜像源（中国用户推荐）")
else()
    set(JSON_GIT_REPOSITORY "https://github.com/nlohmann/json.git")
    message(STATUS "🌐 使用 GitHub 官方源")
endif()

# 添加依赖管理选项
option(USE_SYSTEM_JSON "Use system-installed nlohmann/json instead of downloading" OFF)
set(JSON_LOCAL_PATH "" CACHE PATH "Path to local nlohmann/json library (overrides other options)")

# =============================================================================
# nlohmann/json 依赖管理 - 支持三种方式
# =============================================================================

# 方式1: 使用本地路径（优先级最高）
if(JSON_LOCAL_PATH)
    message(STATUS "📁 正在使用本地 nlohmann/json 库...")
    message(STATUS "   📍 本地路径: ${JSON_LOCAL_PATH}")
    
    # 验证路径是否存在
    if(NOT EXISTS "${JSON_LOCAL_PATH}")
        message(FATAL_ERROR "❌ 错误: 指定的本地路径不存在: ${JSON_LOCAL_PATH}")
    endif()
    
    # 检查是否包含必要的文件
    if(EXISTS "${JSON_LOCAL_PATH}/include/nlohmann/json.hpp")
        # 标准安装结构
        set(JSON_INCLUDE_DIR "${JSON_LOCAL_PATH}/include")
        message(STATUS "   📋 检测到标准安装结构")
    elseif(EXISTS "${JSON_LOCAL_PATH}/single_include/nlohmann/json.hpp")
        # 单头文件结构
        set(JSON_INCLUDE_DIR "${JSON_LOCAL_PATH}/single_include")
        message(STATUS "   📋 检测到单头文件结构")
    elseif(EXISTS "${JSON_LOCAL_PATH}/json.hpp")
        # 直接头文件
        set(JSON_INCLUDE_DIR "${JSON_LOCAL_PATH}")
        message(STATUS "   📋 检测到直接头文件")
    else()
        message(FATAL_ERROR "❌ 错误: 在指定路径中未找到 nlohmann/json.hpp 文件")
    endif()
    
    # 创建导入目标
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
    target_include_directories(nlohmann_json::nlohmann_json INTERFACE "${JSON_INCLUDE_DIR}")
    message(STATUS "✅ 本地 nlohmann/json 配置完成！")
    
# 方式2: 使用系统安装的库
elseif(USE_SYSTEM_JSON)
    message(STATUS "🔍 正在查找系统安装的 nlohmann/json...")
    find_package(nlohmann_json 3.10.0 QUIET)
    
    if(nlohmann_json_FOUND)
        message(STATUS "✅ 找到系统 nlohmann/json ${nlohmann_json_VERSION}")
    else()
        message(STATUS "❌ 系统未找到 nlohmann/json，将自动下载")
        set(USE_SYSTEM_JSON OFF)
    endif()
endif()

# 方式3: 自动下载（默认方式）
if(NOT JSON_LOCAL_PATH AND NOT USE_SYSTEM_JSON)
    message(STATUS "📦 正在配置 nlohmann/json v3.11.3...")
    message(STATUS "   📍 下载源: ${JSON_GIT_REPOSITORY}")
    message(STATUS "   ⏳ 首次下载可能需要几分钟，请耐心等待...")
    message(STATUS "   💡 提示: 如果下载很慢，可以使用 -DUSE_GITEE_MIRROR=ON 选项")
    message(STATUS "   💡 提示: 也可以使用 -DJSON_LOCAL_PATH=/path/to/json 指定本地库")
    
    # 配置 nlohmann/json 库
    FetchContent_Declare(
        json
        GIT_REPOSITORY ${JSON_GIT_REPOSITORY}
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE  # 只下载指定tag，减少下载量
        GIT_PROGRESS TRUE  # 显示Git下载进度
        TIMEOUT 300  # 5分钟超时
    )
    
    # 检查是否已经下载
    FetchContent_GetProperties(json)
    if(NOT json_POPULATED)
        message(STATUS "⬇️  开始下载 nlohmann/json...")
        FetchContent_Populate(json)
        message(STATUS "✅ nlohmann/json 下载完成！")
        
        message(STATUS "🔨 正在配置 nlohmann/json...")
        add_subdirectory(${json_SOURCE_DIR} ${json_BINARY_DIR})
        message(STATUS "✅ nlohmann/json 配置完成！")
    else()
        message(STATUS "✅ nlohmann/json 已存在，跳过下载")
    endif()
endif()

# =============================================================================
# 调试器支持配置
# =============================================================================
option(BUILD_DEBUGGER "Build debugger support" ON)

if(BUILD_DEBUGGER)
    message(STATUS "🐛 启用调试器支持")
    add_compile_definitions(ENABLE_DEBUGGER)
    
    # 调试器源文件
    set(DEBUGGER_SOURCES
        src/debugger/core/debugger_core.cpp
        src/debugger/info/debug_info.cpp
        src/debugger/info/debug_info_generator.cpp
        src/debugger/vm/stack_state.cpp
        src/debugger/vm/stack_argument.cpp
        src/debugger/vm/stack_trace.cpp
        src/debugger/vm/bvm_simulator.cpp
        src/debugger/vm/transaction_data_loader.cpp
        src/debugger/breakpoint/breakpoint.cpp
        src/debugger/breakpoint/breakpoint_manager.cpp
        src/debugger/inspector/variable_inspector.cpp
        src/debugger/inspector/scope_inspector.cpp
        src/debugger/inspector/expression_evaluator.cpp
        src/debugger/interface/cli_debugger.cpp
        src/debugger/protocol/live_debug_server.cpp
        src/interpreter/bytecode_runner.cpp
    )
else()
    message(STATUS "⚠️  调试器支持已禁用")
    set(DEBUGGER_SOURCES "")
endif()

find_library(READLINE_LIBRARY NAMES readline libreadline.so.8)
if(READLINE_LIBRARY)
    message(STATUS "✅ 找到 readline: ${READLINE_LIBRARY}")
else()
    message(STATUS "⚠️  未找到 readline，REPL 将使用基础 getline 输入")
endif()

set(CORE_SOURCES
        src/util/op_stack.cpp
        src/util/compiler_placeholder.cpp
        src/util/help.cpp
        src/log/logger.cpp
        src/pass/pass_manager.cpp
        src/ast/ast.cpp
        src/lexer/lexer.cpp
        src/lexer/import_resolver.cpp
        src/parser/parser.cpp
        src/debug/printvisit.cpp
        src/bytecode/bytecode_opcodes.cpp
        src/bytecode/bytecode_generator.cpp
        src/bytecode/symtab.cpp
        src/bytecode/type_validator.cpp
        src/bytecode/scope.cpp
        src/bytecode/script_decoder.cpp
        src/crypto/hash_utils.cpp
        src/compiler/ast_to_bytecode_visitor.cpp
        src/compiler/static_info_visitor.cpp
        src/compiler/collect_symbols_visitor.cpp
        src/compiler/pre_analysis_visitor.cpp
        src/compiler/constant_folder.cpp
        src/compiler/library_merger.cpp
        src/compiler/bytecode_pipeline.cpp
        src/compiler/compiler_driver.cpp
        src/compiler/frontend_pipeline.cpp
        src/config/config_manager.cpp
        src/error/error_manager.cpp
        src/interpreter/ast_interpreter.cpp
        src/interpreter/ast_self_test.cpp
        src/interpreter/builtin_runtime.cpp
        src/interpreter/environment.cpp
        src/interpreter/function_selection.cpp
        src/interpreter/runtime_argument.cpp
        src/interpreter/runtime_context.cpp
        src/interpreter/runtime_codec.cpp
        src/interpreter/runtime_error.cpp
        src/interpreter/runtime_self_test.cpp
        src/interpreter/runtime_slot.cpp
        src/interpreter/runtime_value.cpp
        src/interpreter/transaction_context.cpp
        src/repl/repl_session.cpp
        src/repl/repl_shell.cpp
        ${DEBUGGER_SOURCES}
)

set(COMPILER_REPL_SOURCES
        src/compiler_repl/repl_session.cpp
        src/compiler_repl/cell_compiler.cpp
        src/compiler_repl/magic_commands.cpp
        src/compiler_repl/repl_frontend.cpp
)

add_library(apc_core STATIC ${CORE_SOURCES} ${COMPILER_REPL_SOURCES})
target_include_directories(apc_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(apc_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

target_link_libraries(apc_core PUBLIC nlohmann_json::nlohmann_json)
if(READLINE_LIBRARY)
    target_compile_definitions(apc_core PUBLIC APC_ENABLE_READLINE)
    target_link_libraries(apc_core PUBLIC ${READLINE_LIBRARY})
endif()

add_executable(${EXECUTABLE_NAME} main.cpp)
target_link_libraries(${EXECUTABLE_NAME} PRIVATE apc_core)
target_compile_options(${EXECUTABLE_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# =============================================================================
# Stdlib (.ct 模板库) 安装与路径注入
# =============================================================================
# 安装路径: <prefix>/share/apc/stdlib
# 运行时查找顺序: APC_STDLIB_PATH 环境变量 > user_preferences.json paths.stdlib
#                 > APC_DEFAULT_STDLIB_PATH (下方注入的安装路径) > ./stdlib
set(APC_STDLIB_INSTALL_DIR "share/apc/stdlib")
install(DIRECTORY ${CMAKE_SOURCE_DIR}/stdlib/
        DESTINATION ${APC_STDLIB_INSTALL_DIR}
        FILES_MATCHING PATTERN "*.ct")
target_compile_definitions(apc_core PUBLIC
    APC_DEFAULT_STDLIB_PATH="${CMAKE_INSTALL_PREFIX}/${APC_STDLIB_INSTALL_DIR}")

enable_testing()
add_test(
    NAME ast-self-test
    COMMAND $<TARGET_FILE:${EXECUTABLE_NAME}> test ast
)
add_test(
    NAME repl-smoke
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/repl/run_repl_smoke.sh $<TARGET_FILE:${EXECUTABLE_NAME}>
)
add_test(
    NAME ast-regression
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/interpreter/run_ast_regression.sh
)

# =============================================================================
# 调试器功能已集成到主程序（通过 debug 子命令）
# 使用 ./apc debug example.ct 启动调试器
# =============================================================================

# =============================================================================
# 配置完成提示
# =============================================================================
message(STATUS "")
message(STATUS "🎉 ${PROJECT_NAME} 配置完成！")
message(STATUS "   🚀 下一步: 运行 make 或 cmake --build . 来构建项目")
message(STATUS "   📁 可执行文件将生成到: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "   📝 日志文件: ${DISPLAY_EXECUTABLE_NAME}.log")
message(STATUS "")
