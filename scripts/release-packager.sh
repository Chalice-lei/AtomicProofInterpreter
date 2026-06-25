#!/bin/bash

# =============================================================================
# 发布打包脚本
# =============================================================================
# 功能: 创建带版本信息的发布产物
# 支持: tar.gz, zip, deb, rpm 等多种格式
# 示例: 项目名-v2.0-alpha.tar.gz (解压后程序名为实际的可执行文件名)
# =============================================================================

set -e

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

BUILD_DIR="$PROJECT_ROOT/build"
RELEASE_DIR="$PROJECT_ROOT/releases"
DIST_DIR="$PROJECT_ROOT/dist"

# 显示帮助信息
show_help() {
    cat << EOF
${CYAN}${PROJECT_DISPLAY_NAME} 发布打包脚本${NC}

用法: $0 [选项] [格式]

${YELLOW}支持的格式:${NC}
    tar.gz          标准 Linux 发布包 (默认)
    zip             Windows/跨平台压缩包
    deb             Debian/Ubuntu 软件包 (需要 dpkg-deb)
    rpm             RedHat/CentOS 软件包 (需要 rpmbuild)
    appimage        便携式 Linux 应用 (需要 appimagetool)
    all             生成所有支持的格式

${YELLOW}选项:${NC}
    -h, --help      显示帮助信息
    -v, --verbose   显示详细信息
    -c, --clean     打包前清理旧文件
    --no-docs       不包含文档文件
    --minimal       最小化打包（仅可执行文件）

${YELLOW}示例:${NC}
    $0 tar.gz       # 生成 ${PROJECT_NAME}-v1.0.0-alpha.tar.gz
    $0 zip          # 生成 ${PROJECT_NAME}-v1.0.0-alpha.zip
    $0 all          # 生成所有格式
    $0 --clean tar.gz  # 清理后打包

${YELLOW}输出目录:${NC}
    发布产物将保存到: ${DIST_DIR}/

EOF
}

# 获取版本信息
get_version_info() {
    if [[ ! -f "$BUILD_DIR/bin/$EXECUTABLE_NAME" ]]; then
        log_error "可执行文件不存在，请先构建项目"
        log_info "运行: cd build && make"
        exit 1
    fi
    
    # 从可执行文件获取版本信息
    local version_output=$("$BUILD_DIR/bin/$EXECUTABLE_NAME" --version 2>/dev/null | head -10)
    
    # 解析版本号
    PROJECT_VERSION=$(echo "$version_output" | grep "^Version:" | awk '{print $2}')
    local parsed_name=$(echo "$version_output" | grep "^Compiler Name:" | awk '{print $3}')
    
    # 如果解析到的名称与配置不符，使用配置的名称
    if [[ -n "$parsed_name" && "$parsed_name" != "$PROJECT_NAME" ]]; then
        log_warning "可执行文件中的名称($parsed_name)与配置不符，使用配置名称($PROJECT_NAME)"
    fi
    
    if [[ -z "$PROJECT_VERSION" ]]; then
        log_error "无法获取版本信息"
        exit 1
    fi
    
    log_info "项目名称: $PROJECT_NAME"
    log_info "版本号: $PROJECT_VERSION"
}

# 准备发布目录
prepare_release_structure() {
    local temp_dir="$1"
    local package_name="$2"
    
    log_step "准备发布目录结构..."
    
    # 创建包目录 (布局与 cmake --install 一致: bin/ + share/apc/stdlib/)
    local pkg_dir="$temp_dir/$package_name"
    mkdir -p "$pkg_dir/bin"
    mkdir -p "$pkg_dir/share/apc"

    # 复制可执行文件到 bin/
    cp "$BUILD_DIR/bin/$EXECUTABLE_NAME" "$pkg_dir/bin/"

    # 复制模板库到 share/apc/stdlib/ (运行期通过 <exe_dir>/../share/apc/stdlib 命中)
    if [[ -d "$PROJECT_ROOT/stdlib" ]]; then
        cp -r "$PROJECT_ROOT/stdlib" "$pkg_dir/share/apc/stdlib"
    else
        log_warning "stdlib 目录不存在: $PROJECT_ROOT/stdlib (发布包将不含模板库)"
    fi

    # 复制文档（如果存在且未禁用）
    if [[ "$INCLUDE_DOCS" == "true" ]]; then
        if [[ -f "$PROJECT_ROOT/README.md" ]]; then
            cp "$PROJECT_ROOT/README.md" "$pkg_dir/"
        fi

        if [[ -d "$PROJECT_ROOT/doc" ]]; then
            cp -r "$PROJECT_ROOT/doc" "$pkg_dir/"
        fi
    fi
    
    # 创建版本信息文件
    cat > "$pkg_dir/VERSION" << EOF
项目名称: $PROJECT_NAME
版本号: $PROJECT_VERSION
构建时间: $(date '+%Y-%m-%d %H:%M:%S')
构建平台: $(uname -m)
操作系统: $(uname -s)
EOF
    
    # 创建安装脚本（如果需要）
    if [[ "$MINIMAL_PACKAGE" != "true" ]]; then
        cat > "$pkg_dir/install.sh" << EOF
#!/bin/bash
echo "$PROJECT_DISPLAY_NAME 安装脚本"
echo "========================"

# 脚本所在目录 (包根)
PKG_DIR="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"

# 检查权限并选择安装前缀 (布局: <prefix>/bin + <prefix>/share/apc/stdlib)
if [[ \$EUID -eq 0 ]]; then
    PREFIX="/usr/local"
    echo "检测到管理员权限，安装到系统前缀: \$PREFIX"
else
    PREFIX="\$HOME/.local"
    echo "安装到用户前缀: \$PREFIX"
fi

INSTALL_DIR="\$PREFIX/bin"
STDLIB_DIR="\$PREFIX/share/apc/stdlib"
mkdir -p "\$INSTALL_DIR"
mkdir -p "\$(dirname "\$STDLIB_DIR")"

# 复制可执行文件
cp "\$PKG_DIR/bin/$EXECUTABLE_NAME" "\$INSTALL_DIR/"
chmod +x "\$INSTALL_DIR/$EXECUTABLE_NAME"

# 复制模板库 (保持 <prefix>/share/apc/stdlib 布局, 运行期 exe 按 ../share/apc/stdlib 命中)
if [[ -d "\$PKG_DIR/share/apc/stdlib" ]]; then
    rm -rf "\$STDLIB_DIR"
    cp -r "\$PKG_DIR/share/apc/stdlib" "\$STDLIB_DIR"
    echo "📚 模板库已安装到: \$STDLIB_DIR"
else
    echo "⚠️  警告: 发布包中未找到 share/apc/stdlib, 跳过模板库安装"
fi

echo ""
echo "✅ 安装完成！"
echo "📁 可执行文件位置: \$INSTALL_DIR/$EXECUTABLE_NAME"

# 检查PATH配置
if [[ "\$INSTALL_DIR" == "\$HOME/.local/bin" ]]; then
    if ! echo "\$PATH" | grep -q "\$HOME/.local/bin"; then
        echo ""
        echo "⚠️  注意: \$HOME/.local/bin 不在您的PATH环境变量中"
        echo ""
        echo "🔧 解决方案 (选择其中一种):"
        echo ""
        echo "方案1: 添加到PATH (推荐)"
        echo "  echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.bashrc"
        echo "  source ~/.bashrc"
        echo ""
        echo "方案2: 使用完整路径运行"
        echo "  \$INSTALL_DIR/$EXECUTABLE_NAME --version"
        echo ""
        echo "方案3: 创建系统链接 (需要sudo)"
        echo "  sudo ln -sf \$INSTALL_DIR/$EXECUTABLE_NAME /usr/local/bin/$EXECUTABLE_NAME"
        echo ""
        echo "推荐执行方案1后，就可以直接使用 '$EXECUTABLE_NAME' 命令了"
    else
        echo ""
        echo "✅ PATH配置正确，可以直接使用 '$EXECUTABLE_NAME' 命令"
    fi
fi

echo ""
echo "🚀 使用方法:"
if [[ "\$INSTALL_DIR" == "/usr/local/bin" ]] || echo "\$PATH" | grep -q "\$HOME/.local/bin"; then
    echo "  $EXECUTABLE_NAME --version"
    echo "  $EXECUTABLE_NAME --help"
else
    echo "  \$INSTALL_DIR/$EXECUTABLE_NAME --version"
    echo "  \$INSTALL_DIR/$EXECUTABLE_NAME --help"
    echo "  (或按上述方案配置PATH后直接使用 '$EXECUTABLE_NAME')"
fi
EOF
        chmod +x "$pkg_dir/install.sh"
    fi
    
    echo "$pkg_dir"
}

# 创建 tar.gz 包
create_tar_package() {
    local package_name="$1"
    local temp_dir=$(mktemp -d)
    
    log_step "创建 tar.gz 发布包..."
    
    local pkg_dir=$(prepare_release_structure "$temp_dir" "$package_name")
    
    # 创建 tar.gz
    cd "$temp_dir"
    tar -czf "$DIST_DIR/${package_name}.tar.gz" "$package_name"
    
    # 清理临时目录
    rm -rf "$temp_dir"
    
    log_success "已创建: ${package_name}.tar.gz"
}

# 创建 zip 包
create_zip_package() {
    local package_name="$1"
    local temp_dir=$(mktemp -d)
    
    log_step "创建 zip 发布包..."
    
    if ! command -v zip &> /dev/null; then
        log_warning "zip 命令未找到，跳过 zip 包创建"
        return
    fi
    
    local pkg_dir=$(prepare_release_structure "$temp_dir" "$package_name")
    
    # 创建 zip
    cd "$temp_dir"
    zip -r "$DIST_DIR/${package_name}.zip" "$package_name"
    
    # 清理临时目录
    rm -rf "$temp_dir"
    
    log_success "已创建: ${package_name}.zip"
}

# 创建 deb 包
create_deb_package() {
    local package_name="$1"
    
    log_step "创建 deb 发布包..."
    
    if ! command -v dpkg-deb &> /dev/null; then
        log_error "dpkg-deb 未安装，无法创建 deb 包"
        log_info "安装命令: sudo apt install dpkg-dev"
        return 1
    fi
    
    local temp_dir=$(mktemp -d)
    local deb_dir="$temp_dir/${package_name}"
    
    # 创建 Debian 包结构 (<prefix>/bin + <prefix>/share/apc/stdlib)
    mkdir -p "$deb_dir/usr/local/bin"
    mkdir -p "$deb_dir/usr/local/share/apc"
    mkdir -p "$deb_dir/DEBIAN"

    # 复制可执行文件
    cp "$BUILD_DIR/bin/$EXECUTABLE_NAME" "$deb_dir/usr/local/bin/"
    chmod +x "$deb_dir/usr/local/bin/$EXECUTABLE_NAME"

    # 复制模板库
    if [[ -d "$PROJECT_ROOT/stdlib" ]]; then
        cp -r "$PROJECT_ROOT/stdlib" "$deb_dir/usr/local/share/apc/stdlib"
    else
        log_warning "stdlib 目录不存在, deb 包将不含模板库"
    fi
    
    # 创建控制文件
    cat > "$deb_dir/DEBIAN/control" << EOF
Package: $PROJECT_NAME
Version: $PROJECT_VERSION
Section: utils
Priority: optional
Architecture: amd64
Maintainer: $PROJECT_NAME Project Team
Description: $PROJECT_DISPLAY_NAME
 A comprehensive Bitcoin script compiler and development toolkit.
EOF
    
    # 构建 deb 包
    if dpkg-deb --build "$deb_dir" "$DIST_DIR/${package_name}.deb"; then
        log_success "已创建: ${package_name}.deb"
    else
        log_error "deb 包创建失败"
        rm -rf "$temp_dir"
        return 1
    fi
    
    # 清理临时目录
    rm -rf "$temp_dir"
}

# 主函数
main() {
    echo -e "${CYAN}"
    echo "======================================="
    echo "      发布打包脚本"
    echo "======================================="
    echo -e "${NC}"
    
    # 解析项目信息
    parse_project_info
    
    # 默认设置
    CLEAN_DIST=false
    INCLUDE_DOCS=true
    MINIMAL_PACKAGE=false
    VERBOSE=false
    FORMAT="tar.gz"
    
    # 解析参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -c|--clean)
                CLEAN_DIST=true
                shift
                ;;
            --no-docs)
                INCLUDE_DOCS=false
                shift
                ;;
            --minimal)
                MINIMAL_PACKAGE=true
                shift
                ;;
            tar.gz|zip|deb|rpm|appimage|all)
                FORMAT="$1"
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
    
    # 清理发布目录
    if [[ "$CLEAN_DIST" == "true" ]]; then
        log_step "清理发布目录..."
        rm -rf "$DIST_DIR"
    fi
    
    # 创建发布目录
    mkdir -p "$DIST_DIR"
    
    # 获取版本信息
    get_version_info
    
    local package_name="${PROJECT_NAME}-v${PROJECT_VERSION}"
    
    # 根据格式创建发布包
    case "$FORMAT" in
        tar.gz)
            create_tar_package "$package_name"
            ;;
        zip)
            create_zip_package "$package_name"
            ;;
        deb)
            create_deb_package "$package_name"
            ;;
        rpm)
            create_rpm_package "$package_name"
            ;;
        appimage)
            create_appimage_package "$package_name"
            ;;
        all)
            log_info "创建所有格式的发布包..."
            create_tar_package "$package_name"
            create_zip_package "$package_name"
            create_deb_package "$package_name"
            create_rpm_package "$package_name"
            create_appimage_package "$package_name"
            ;;
    esac
    
    # 显示结果
    echo ""
    log_success "发布包创建完成！"
    echo ""
    echo "生成的发布包："
    ls -lh "$DIST_DIR" | grep -v "^total" || echo "  (无文件)"
    
    echo ""
    log_info "验证步骤："
    echo "  1. 检查发布包大小和内容"
    echo "  2. 用户解压后运行 ./$EXECUTABLE_NAME --version 验证"
    echo "  3. 测试安装脚本是否正常工作"
}

# 执行主函数
main "$@" 
