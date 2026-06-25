#!/bin/bash

# =============================================================================
# 跨平台编译脚本
# =============================================================================
# 功能: 在Linux上编译多个平台的目标文件
# 支持: Linux (native), Windows (cross-compile), 可扩展其他平台
# =============================================================================

set -e

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

BUILDS_DIR="$PROJECT_ROOT/builds"

# 显示帮助信息
show_help() {
    cat << EOF
${CYAN}${PROJECT_DISPLAY_NAME} 跨平台编译脚本${NC}

用法: $0 [选项] [平台]

${YELLOW}支持的平台:${NC}
    linux           本地Linux编译 (默认)
    windows-32      32位Windows交叉编译
    windows-64      64位Windows交叉编译 (推荐)
    all             编译所有支持的平台

${YELLOW}选项:${NC}
    -h, --help      显示帮助信息
    -c, --clean     编译前清理构建目录
    -r, --release   强制Release模式编译
    -j, --jobs N    并行编译作业数（默认CPU核心数）
    --check-deps    检查跨平台编译依赖

${YELLOW}示例:${NC}
    $0 linux        # 编译Linux版本
    $0 windows-64   # 编译Windows 64位版本  
    $0 all          # 编译所有平台
    $0 --clean --release windows-64  # 清理后Release编译Windows版本

${YELLOW}输出目录:${NC}
    编译产物保存到: ${BUILDS_DIR}/

EOF
}

# 检查跨平台编译依赖
check_dependencies() {
    log_step "检查跨平台编译依赖..."
    
    local missing_deps=()
    
    # 检查基础工具
    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi
    
    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi
    
    # 检查Windows交叉编译工具链
    if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
        log_warning "Windows 64位交叉编译工具链未找到"
        log_info "安装命令: sudo apt install gcc-mingw-w64-x86-64"
    else
        log_success "Windows 64位交叉编译工具链: ✓"
    fi
    
    if ! command -v i686-w64-mingw32-gcc &> /dev/null; then
        log_warning "Windows 32位交叉编译工具链未找到"
        log_info "安装命令: sudo apt install gcc-mingw-w64-i686"
    else
        log_success "Windows 32位交叉编译工具链: ✓"
    fi
    
    if [[ ${#missing_deps[@]} -gt 0 ]]; then
        log_error "缺少依赖: ${missing_deps[*]}"
        log_info "请安装缺少的依赖后重试"
        exit 1
    fi
    
    log_success "依赖检查完成"
}

# 编译单个平台
build_platform() {
    local platform="$1"
    local build_type="$2"
    local jobs="$3"
    
    log_step "编译 $platform 平台..."
    
    # 创建平台特定的构建目录
    local build_dir="$BUILDS_DIR/$platform"
    
    if [[ "$CLEAN_BUILD" == "true" ]]; then
        rm -rf "$build_dir"
    fi
    
    mkdir -p "$build_dir"
    cd "$build_dir"
    
    # 根据平台设置CMake参数
    local cmake_args=""
    if [[ -n "$build_type" ]]; then
        cmake_args="-DCMAKE_BUILD_TYPE=$build_type"
    fi
    
    case "$platform" in
        "linux")
            # 本地Linux编译，无需特殊参数
            ;;
        "windows-64")
            cmake_args="$cmake_args -DCROSS_COMPILE_TARGET=x86_64-w64-mingw32"
            ;;
        "windows-32")
            cmake_args="$cmake_args -DCROSS_COMPILE_TARGET=i686-w64-mingw32"
            ;;
        *)
            log_error "不支持的平台: $platform"
            return 1
            ;;
    esac
    
    log_info "CMake 参数: $cmake_args"
    
    # 配置项目
    if ! cmake "$PROJECT_ROOT" $cmake_args; then
        log_error "$platform 平台配置失败"
        return 1
    fi
    
    # 编译项目
    if ! make -j"$jobs"; then
        log_error "$platform 平台编译失败"
        return 1
    fi
    
    # 检查生成的可执行文件 - 使用动态名称
    local executable=""
    if [[ "$platform" == "linux" ]]; then
        executable=$(find "$build_dir/bin" -name "$EXECUTABLE_NAME" -type f -executable 2>/dev/null | head -1)
    else
        # Windows平台，查找.exe文件
        executable=$(find "$build_dir/bin" -name "${EXECUTABLE_NAME}.exe" -type f 2>/dev/null | head -1)
        if [[ -z "$executable" ]]; then
            # 兼容性检查：查找任何.exe文件
            executable=$(find "$build_dir/bin" -name "*.exe" -type f 2>/dev/null | head -1)
        fi
    fi
    
    if [[ -z "$executable" ]]; then
        log_error "$platform 平台未生成可执行文件（预期名称: $EXECUTABLE_NAME）"
        log_warning "尝试查找所有可执行文件："
        (find "$build_dir/bin" -type f -executable 2>/dev/null || find "$build_dir/bin" -name "*.exe" 2>/dev/null || echo "  无可执行文件") || true
        return 1
    fi
    
    local exec_name=$(basename "$executable")
    local exec_size=$(du -h "$executable" | cut -f1)
    
    log_success "$platform 编译完成: $exec_name ($exec_size)"
    
    # 显示文件信息
    if command -v file &> /dev/null; then
        local file_info=$(file "$executable" 2>/dev/null | cut -c1-100 || echo "无法获取文件信息")
        log_info "文件信息: $file_info"
    fi
    
    return 0
}

# 主函数
main() {
    echo -e "${CYAN}"
    echo "======================================="
    echo "     跨平台编译脚本"
    echo "======================================="
    echo -e "${NC}"
    
    # 解析项目信息
    parse_project_info
    
    # 默认设置
    CLEAN_BUILD=false
    BUILD_TYPE=""  # 空值，让CMakeLists.txt决定默认构建类型
    JOBS=$(nproc)
    PLATFORMS=("linux")
    CHECK_DEPS_ONLY=false
    
    # 解析参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -c|--clean)
                CLEAN_BUILD=true
                shift
                ;;
            -r|--release)
                BUILD_TYPE="Release"
                shift
                ;;
            -j|--jobs)
                JOBS="$2"
                shift 2
                ;;
            --check-deps)
                CHECK_DEPS_ONLY=true
                shift
                ;;
            linux|windows-32|windows-64)
                PLATFORMS=("$1")
                shift
                ;;
            all)
                PLATFORMS=("linux" "windows-64" "windows-32")
                shift
                ;;
            *)
                log_error "未知参数: $1"
                show_help
                exit 1
                ;;
        esac
    done
    
    # 切换到项目根目录
    cd "$PROJECT_ROOT"
    
    # 检查依赖
    check_dependencies
    
    if [[ "$CHECK_DEPS_ONLY" == "true" ]]; then
        exit 0
    fi
    
    # 创建构建目录
    mkdir -p "$BUILDS_DIR"
    
    # 显示构建信息
    log_info "构建配置:"
    log_info "  平台: ${PLATFORMS[*]}"
    if [[ -n "$BUILD_TYPE" ]]; then
        log_info "  类型: $BUILD_TYPE (脚本指定)"
    else
        log_info "  类型: 使用CMakeLists.txt默认配置 (Release)"
    fi
    log_info "  并行: $JOBS 作业"
    log_info "  清理: $CLEAN_BUILD"
    echo ""
    
    # 编译各个平台
    local success_count=0
    local total_count=${#PLATFORMS[@]}
    
    for platform in "${PLATFORMS[@]}"; do
        echo ""
        if build_platform "$platform" "$BUILD_TYPE" "$JOBS"; then
            success_count=$((success_count + 1))
        fi
    done
    
    # 显示结果总结
    echo ""
    echo "======================================="
    log_info "编译结果总结:"
    log_info "  成功: $success_count/$total_count"
    
    if [[ $success_count -eq $total_count ]]; then
        log_success "所有平台编译成功！"
    else
        log_warning "部分平台编译失败"
    fi
    
    echo ""
    log_info "编译产物位置:"
    for platform in "${PLATFORMS[@]}"; do
        if [[ -d "$BUILDS_DIR/$platform/bin" ]]; then
            local executable=""
            if [[ "$platform" == "linux" ]]; then
                executable=$(find "$BUILDS_DIR/$platform/bin" -name "$EXECUTABLE_NAME" -type f 2>/dev/null | head -1)
            else
                executable=$(find "$BUILDS_DIR/$platform/bin" -name "${EXECUTABLE_NAME}.exe" -type f 2>/dev/null | head -1)
                if [[ -z "$executable" ]]; then
                    executable=$(find "$BUILDS_DIR/$platform/bin" -name "*.exe" -type f 2>/dev/null | head -1)
                fi
            fi
            if [[ -n "$executable" ]]; then
                echo "  $platform: $executable"
            fi
        fi
    done
    
    echo ""
    log_info "下一步: 使用 ./scripts/cross-platform-packager.sh 打包发布"
    
    # 根据编译结果设置退出码
    if [[ $success_count -eq $total_count ]]; then
        exit 0
    else
        exit 1
    fi
}

# 执行主函数
main "$@" 
