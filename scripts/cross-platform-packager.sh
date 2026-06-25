#!/bin/bash

# =============================================================================
# 跨平台打包脚本
# =============================================================================
# 功能: 将多平台编译产物打包成发布包
# 支持: 单平台包和多平台合集包
# =============================================================================

set -e

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

BUILDS_DIR="$PROJECT_ROOT/builds"
DIST_DIR="$PROJECT_ROOT/dist"

# 显示帮助信息
show_help() {
    cat << EOF
${CYAN}${PROJECT_DISPLAY_NAME} 跨平台打包脚本${NC}

用法: $0 [选项] [模式]

${YELLOW}打包模式:${NC}
    single          单平台打包 (需要指定 --platform)
    multi           多平台合集包
    all             打包所有可用平台 (默认)

${YELLOW}选项:${NC}
    -h, --help         显示帮助信息
    -p, --platform P   指定平台 (linux, windows-32, windows-64)
    -c, --clean        打包前清理输出目录
    --no-docs          不包含文档文件
    --minimal          最小化打包（仅可执行文件）

${YELLOW}示例:${NC}
    $0 all                          # 打包所有可用平台
    $0 single --platform linux       # 仅打包Linux版本
    $0 single --platform windows-64  # 仅打包Windows 64位版本
    $0 multi                        # 创建多平台合集包
    $0 --clean all                  # 清理后打包所有平台

${YELLOW}输出目录:${NC}
    发布包保存到: ${DIST_DIR}/

EOF
}

# 获取版本信息
get_version_info() {
    # 尝试从各个平台的可执行文件获取版本信息
    local version_exe=""
    local use_wine=false
    
    # 优先使用Linux版本
    if [[ -f "$BUILDS_DIR/linux/bin/$EXECUTABLE_NAME" ]]; then
        version_exe="$BUILDS_DIR/linux/bin/$EXECUTABLE_NAME"
        log_info "使用Linux版本获取版本信息"
    # 其次尝试Windows版本（优先不使用wine的方式）
    elif [[ -f "$BUILDS_DIR/windows-64/bin/${EXECUTABLE_NAME}.exe" ]]; then
        # 尝试通过objdump或strings获取版本信息（无需wine）
        if try_get_version_from_executable "$BUILDS_DIR/windows-64/bin/${EXECUTABLE_NAME}.exe"; then
            log_info "从Windows-64可执行文件中解析版本信息"
            return 0
        elif command -v wine &> /dev/null; then
            version_exe="wine $BUILDS_DIR/windows-64/bin/${EXECUTABLE_NAME}.exe"
            use_wine=true
            log_info "使用Wine运行Windows-64版本获取版本信息"
        else
            log_warning "无法直接获取Windows版本信息，尝试从CMake配置获取"
            if try_get_version_from_cmake; then
                return 0
            else
                log_error "无法获取版本信息"
                log_info "解决方案："
                log_info "1. 先构建Linux版本: ./scripts/cross-platform-builder.sh linux"
                log_info "2. 或安装wine: sudo apt install wine"
                log_info "3. 或确保CMakeLists.txt中有VERSION信息"
                exit 1
            fi
        fi
    # 向后兼容：检查旧的windows目录（已弃用）
    elif [[ -f "$BUILDS_DIR/windows/bin/${EXECUTABLE_NAME}.exe" ]]; then
        log_warning "检测到旧的windows构建目录，建议使用明确的windows-64参数"
        if try_get_version_from_executable "$BUILDS_DIR/windows/bin/${EXECUTABLE_NAME}.exe"; then
            log_info "从旧Windows可执行文件中解析版本信息"
            return 0
        elif command -v wine &> /dev/null; then
            version_exe="wine $BUILDS_DIR/windows/bin/${EXECUTABLE_NAME}.exe"
            use_wine=true
            log_info "使用Wine运行旧Windows版本获取版本信息"
        else
            log_warning "无法直接获取Windows版本信息，尝试从CMake配置获取"
            if try_get_version_from_cmake; then
                return 0
            else
                log_error "无法获取版本信息"
                log_info "解决方案："
                log_info "1. 先构建Linux版本: ./scripts/cross-platform-builder.sh linux"
                log_info "2. 或安装wine: sudo apt install wine"
                log_info "3. 或确保CMakeLists.txt中有VERSION信息"
                exit 1
            fi
        fi
    # 尝试Windows-32版本
    elif [[ -f "$BUILDS_DIR/windows-32/bin/${EXECUTABLE_NAME}.exe" ]]; then
        if try_get_version_from_executable "$BUILDS_DIR/windows-32/bin/${EXECUTABLE_NAME}.exe"; then
            log_info "从Windows-32可执行文件中解析版本信息"
            return 0
        elif command -v wine &> /dev/null; then
            version_exe="wine $BUILDS_DIR/windows-32/bin/${EXECUTABLE_NAME}.exe"
            use_wine=true
            log_info "使用Wine运行Windows-32版本获取版本信息"
        else
            log_warning "无法直接获取Windows版本信息，尝试从CMake配置获取"
            if try_get_version_from_cmake; then
                return 0
            else
                log_error "无法获取版本信息"
                log_info "解决方案："
                log_info "1. 先构建Linux版本: ./scripts/cross-platform-builder.sh linux"
                log_info "2. 或安装wine: sudo apt install wine"
                log_info "3. 或确保CMakeLists.txt中有VERSION信息"
                exit 1
            fi
        fi
    else
        log_error "无法找到任何可执行文件获取版本信息"
        log_info "请先运行构建脚本："
        log_info "  - Linux版本: ./scripts/cross-platform-builder.sh linux"
        log_info "  - Windows版本: ./scripts/cross-platform-builder.sh windows-64"
        log_info "  - 所有版本: ./scripts/cross-platform-builder.sh all"
        exit 1
    fi
    
    # 如果找到了可用的可执行文件，获取版本信息
    if [[ -n "$version_exe" ]]; then
        local version_output
        if [[ "$use_wine" == "true" ]]; then
            # Wine可能产生额外输出，需要过滤 - 支持中文和英文格式
            version_output=$($version_exe --version 2>/dev/null | grep -E "(版本号|编译器名称|Version|Compiler Name)" | head -10)
        else
            version_output=$($version_exe --version 2>/dev/null | head -10)
        fi
        
        # 解析版本号 - 支持中文和英文格式
        PROJECT_VERSION=$(echo "$version_output" | grep -E "(版本号:|Version:)" | sed -E 's/.*(版本号:|Version:)\s*([0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9]+)?).*/\2/' | head -1)
        local parsed_name=$(echo "$version_output" | grep -E "(编译器名称:|Compiler Name:)" | sed -E 's/.*(编译器名称:|Compiler Name:)\s*([a-zA-Z0-9_-]+).*/\2/' | head -1)
        
        # 如果解析到的名称与配置不符，使用配置的名称
        if [[ -n "$parsed_name" && "$parsed_name" != "$PROJECT_NAME" ]]; then
            log_warning "可执行文件中的名称($parsed_name)与配置不符，使用配置名称($PROJECT_NAME)"
        fi
        
        if [[ -z "$PROJECT_VERSION" ]]; then
            log_error "无法解析版本信息"
            exit 1
        fi
        
        log_info "项目名称: $PROJECT_NAME"
        log_info "版本号: $PROJECT_VERSION"
    fi
}

# 尝试从可执行文件中直接解析版本信息（无需wine）
try_get_version_from_executable() {
    local exe_file="$1"
    
    # 尝试使用strings命令提取版本信息
    if command -v strings &> /dev/null; then
        # 更宽松的版本号匹配，支持更多格式
        local version_string=$(strings "$exe_file" | grep -E "[0-9]+\.[0-9]+\.[0-9]+|version.*[0-9]+\.[0-9]+|编译器版本|项目版本" | head -3)
        if [[ -n "$version_string" ]]; then
            # 尝试提取版本号（支持多种格式）
            PROJECT_VERSION=$(echo "$version_string" | grep -oE "[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z]+)?" | head -1)
            if [[ -n "$PROJECT_VERSION" ]]; then
                PROJECT_NAME="$EXECUTABLE_NAME"  # 使用配置的名称
                log_info "从可执行文件提取版本: $PROJECT_VERSION"
                return 0
            fi
        fi
    fi
    
    # 尝试从CMake配置获取版本
    return 1
}

# 尝试从CMake配置获取版本信息
try_get_version_from_cmake() {
    local cmake_file="$PROJECT_ROOT/CMakeLists.txt"
    
    # 解析CMake中的版本信息
    local major=$(grep -E "set\s*\(\s*PROJECT_VERSION_MAJOR" "$cmake_file" | grep -oE "[0-9]+" | head -1)
    local minor=$(grep -E "set\s*\(\s*PROJECT_VERSION_MINOR" "$cmake_file" | grep -oE "[0-9]+" | head -1)
    local patch=$(grep -E "set\s*\(\s*PROJECT_VERSION_PATCH" "$cmake_file" | grep -oE "[0-9]+" | head -1)
    local suffix=$(grep -E "set\s*\(\s*PROJECT_VERSION_SUFFIX" "$cmake_file" | sed -E 's/.*"([^"]*)".*/\1/' | head -1)
    
    if [[ -n "$major" && -n "$minor" && -n "$patch" ]]; then
        if [[ -n "$suffix" ]]; then
            PROJECT_VERSION="${major}.${minor}.${patch}-${suffix}"
        else
            PROJECT_VERSION="${major}.${minor}.${patch}"
        fi
        
        PROJECT_NAME="$EXECUTABLE_NAME"  # 使用配置的名称
        log_info "从CMake配置获取版本: $PROJECT_VERSION"
        return 0
    fi
    
    return 1
}

# 复制Windows依赖库
copy_windows_dependencies() {
    local target_dir="$1"
    local platform="$2"
    
    log_info "检测并复制Windows依赖库..."
    
    # 根据平台确定工具链路径
    local toolchain_base=""
    local pthread_lib=""
    case "$platform" in
        windows-64|windows*64*)
            toolchain_base="/usr/lib/gcc/x86_64-w64-mingw32/13-win32"
            pthread_lib="/usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll"
            ;;
        windows-32|windows*32*)
            toolchain_base="/usr/lib/gcc/i686-w64-mingw32/13-win32"
            pthread_lib="/usr/i686-w64-mingw32/lib/libwinpthread-1.dll"
            ;;
        windows)
            log_warning "参数'windows'已弃用，将作为64位Windows处理"
            toolchain_base="/usr/lib/gcc/x86_64-w64-mingw32/13-win32"
            pthread_lib="/usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll"
            ;;
        *)
            log_warning "未知的Windows平台: $platform，尝试使用64位库"
            toolchain_base="/usr/lib/gcc/x86_64-w64-mingw32/13-win32"
            pthread_lib="/usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll"
            ;;
    esac
    
    # 必要的DLL文件列表
    local required_dlls=(
        "$toolchain_base/libstdc++-6.dll"
        "$toolchain_base/libgcc_s_seh-1.dll"
        "$pthread_lib"
    )
    
    local copied_count=0
    for dll in "${required_dlls[@]}"; do
        if [[ -f "$dll" ]]; then
            cp "$dll" "$target_dir/"
            local dll_name=$(basename "$dll")
            log_success "已复制: $dll_name"
            copied_count=$((copied_count + 1))
        else
            log_warning "未找到依赖库: $dll"
        fi
    done
    
    if [[ $copied_count -gt 0 ]]; then
        log_success "已复制 $copied_count 个依赖库文件"
        
        # 创建依赖库说明文件
        {
            echo "Windows 依赖库说明"
            echo "=================="
            echo ""
            echo "此可执行文件需要以下DLL文件才能正常运行："
            echo ""
            for dll in "${required_dlls[@]}"; do
                if [[ -f "$dll" ]]; then
                    echo "- $(basename "$dll")"
                fi
            done
            echo ""
            echo "这些文件已包含在此发布包中，请确保："
            echo "1. 将所有DLL文件与可执行文件放在同一目录"
            echo "2. 或者将DLL文件放在系统PATH路径中"
            echo ""
            echo "如果运行时仍然出现DLL缺失错误，请："
            echo "1. 安装Microsoft Visual C++ Redistributable"
            echo "2. 或者安装完整的MinGW-w64运行时环境"
            echo ""
            echo "构建信息："
            echo "- 编译器: MinGW-w64 GCC 13"
            echo "- 目标平台: $platform"
            echo "- 构建时间: $(date '+%Y-%m-%d %H:%M:%S')"
        } > "$target_dir/DEPENDENCIES.txt"
    else
        log_warning "未找到任何依赖库，打包的程序可能无法在目标系统上运行"
    fi
}

# 准备通用文件
prepare_common_files() {
    local target_dir="$1"
    
    # 复制文档（如果存在且未禁用）
    if [[ "$INCLUDE_DOCS" == "true" ]]; then
        if [[ -f "$PROJECT_ROOT/README.md" ]]; then
            cp "$PROJECT_ROOT/README.md" "$target_dir/"
        fi
        
        if [[ -d "$PROJECT_ROOT/doc" ]]; then
            cp -r "$PROJECT_ROOT/doc" "$target_dir/"
        fi
    fi
    
    # 创建版本信息文件
    {
        echo "项目名称: $PROJECT_NAME"
        echo "版本号: $PROJECT_VERSION"
        echo "构建时间: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "构建平台: 跨平台编译"
        echo "操作系统: 多平台支持"
    } > "$target_dir/VERSION"
}

# 打包单个平台
package_single_platform() {
    local platform="$1"
    local build_dir="$BUILDS_DIR/$platform"
    
    log_step "打包 $platform 平台..."
    
    # 向后兼容：如果是windows平台，检查旧目录
    if [[ "$platform" == "windows" ]]; then
        log_warning "参数'windows'已弃用，建议使用'windows-64'或'windows-32'"
        if [[ -d "$BUILDS_DIR/windows/bin" ]]; then
            log_info "使用旧的windows构建目录"
            build_dir="$BUILDS_DIR/windows"
        else
            log_error "未找到windows构建产物"
            log_info "请使用: ./scripts/cross-platform-builder.sh windows-64"
            return 1
        fi
    fi
    
    # 检查编译产物是否存在
    if [[ ! -d "$build_dir/bin" ]]; then
        log_error "$platform 平台编译产物不存在"
        log_info "请先运行: ./scripts/cross-platform-builder.sh $platform"
        return 1
    fi
    
    local executable=$(find "$build_dir/bin" -name "${EXECUTABLE_NAME}*" -type f 2>/dev/null | head -1)
    if [[ -z "$executable" ]]; then
        log_error "$platform 平台可执行文件不存在"
        return 1
    fi
    
    # 确定包名和文件扩展名
    local package_name="${PROJECT_NAME}-v${PROJECT_VERSION}-${platform}"
    local exec_name=$(basename "$executable")
    
    # 创建临时打包目录
    local temp_dir=$(mktemp -d)
    local pkg_dir="$temp_dir/$package_name"
    mkdir -p "$pkg_dir"

    # 按平台决定布局:
    #   Linux  : bin/exe + share/apc/stdlib/  (命中 exeDir/../share/apc/stdlib)
    #   Windows: 扁平 (exe + DLLs + stdlib/)  (命中 exeDir/stdlib, DLL 与 exe 同目录)
    case "$platform" in
        windows*|*windows*)
            cp "$executable" "$pkg_dir/"
            copy_windows_dependencies "$pkg_dir" "$platform"
            if [[ -d "$PROJECT_ROOT/stdlib" ]]; then
                cp -r "$PROJECT_ROOT/stdlib" "$pkg_dir/stdlib"
            else
                log_warning "stdlib 目录不存在: $PROJECT_ROOT/stdlib"
            fi
            ;;
        *)
            mkdir -p "$pkg_dir/bin" "$pkg_dir/share/apc"
            cp "$executable" "$pkg_dir/bin/"
            if [[ -d "$PROJECT_ROOT/stdlib" ]]; then
                cp -r "$PROJECT_ROOT/stdlib" "$pkg_dir/share/apc/stdlib"
            else
                log_warning "stdlib 目录不存在: $PROJECT_ROOT/stdlib"
            fi
            ;;
    esac
    
    # 准备通用文件
    prepare_common_files "$pkg_dir"
    
    # 创建平台特定的安装脚本
    if [[ "$MINIMAL_PACKAGE" != "true" ]]; then
        case "$platform" in
            linux|*linux*)
                cat > "$pkg_dir/install.sh" << EOF
#!/bin/bash
echo "$PROJECT_DISPLAY_NAME Linux 安装脚本"
echo "========================="

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
cp "\$PKG_DIR/bin/$exec_name" "\$INSTALL_DIR/"
chmod +x "\$INSTALL_DIR/$exec_name"

# 复制模板库
if [[ -d "\$PKG_DIR/share/apc/stdlib" ]]; then
    rm -rf "\$STDLIB_DIR"
    cp -r "\$PKG_DIR/share/apc/stdlib" "\$STDLIB_DIR"
    echo "📚 模板库已安装到: \$STDLIB_DIR"
else
    echo "⚠️  警告: 发布包中未找到 share/apc/stdlib, 跳过模板库安装"
fi

echo ""
echo "✅ 安装完成！"
echo "📁 可执行文件位置: \$INSTALL_DIR/$exec_name"

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
        echo "  \$INSTALL_DIR/$exec_name --version"
        echo ""
        echo "方案3: 创建系统链接 (需要sudo)"
        echo "  sudo ln -sf \$INSTALL_DIR/$exec_name /usr/local/bin/$exec_name"
        echo ""
        echo "推荐执行方案1后，就可以直接使用 '$exec_name' 命令了"
    else
        echo ""
        echo "✅ PATH配置正确，可以直接使用 '$exec_name' 命令"
    fi
fi

echo ""
echo "🚀 使用方法:"
if [[ "\$INSTALL_DIR" == "/usr/local/bin" ]] || echo "\$PATH" | grep -q "\$HOME/.local/bin"; then
    echo "  $exec_name --version"
    echo "  $exec_name --help"
else
    echo "  \$INSTALL_DIR/$exec_name --version"
    echo "  \$INSTALL_DIR/$exec_name --help"
    echo "  (或按上述方案配置PATH后直接使用 '$exec_name')"
fi
EOF
                chmod +x "$pkg_dir/install.sh"
                ;;
            windows*|*windows*)
                # 获取不带路径的可执行文件名
                local exec_basename=$(basename "$exec_name")
                
                cat > "$pkg_dir/install.bat" << 'BATCH_EOF'
@echo off
chcp 65001 >nul 2>&1
echo Windows Installation Script
echo =========================
echo.

REM Check administrator privileges
net session >nul 2>&1
if %errorLevel% == 0 (
    set "INSTALL_DIR=C:\Program Files\EXEC_PROJECT_NAME"
    echo Administrator privileges detected, installing to system directory
) else (
    set "INSTALL_DIR=%USERPROFILE%\EXEC_PROJECT_NAME"
    echo Installing to user directory
)

REM Create installation directory
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"

REM Copy executable file
copy "EXEC_FILE_NAME" "%INSTALL_DIR%\"
if %errorLevel% == 0 (
    echo Executable file copied successfully
) else (
    echo Error: Failed to copy executable file
    pause
    exit /b 1
)

REM Copy dependency DLL files
echo Copying dependency libraries...
copy *.dll "%INSTALL_DIR%\" >nul 2>&1
if %errorLevel% == 0 (
    echo Dependency libraries copied successfully
) else (
    echo Warning: No dependency libraries found, program may not run properly
)

REM Copy standard library (<install>\stdlib\ hit by resolver candidate exeDir\stdlib)
if exist "stdlib" (
    echo Copying standard library...
    if exist "%INSTALL_DIR%\stdlib" rmdir /s /q "%INSTALL_DIR%\stdlib"
    xcopy /E /I /Q "stdlib" "%INSTALL_DIR%\stdlib" >nul
    if %errorLevel% == 0 (
        echo Standard library copied successfully
    ) else (
        echo Warning: Failed to copy standard library
    )
) else (
    echo Warning: stdlib directory not found in package
)

echo.
echo Installation completed!
echo Installation location: %INSTALL_DIR%
echo Executable file: %INSTALL_DIR%\EXEC_FILE_NAME
echo.
echo Usage:
echo   "%INSTALL_DIR%\EXEC_FILE_NAME" --version
echo   "%INSTALL_DIR%\EXEC_FILE_NAME" --help
echo.
echo Note: If the program fails to run, please check DEPENDENCIES.txt file
echo.

REM Add to PATH (optional)
echo.
echo Do you want to add the installation directory to PATH? (Y/N)
set /p ADD_PATH="This will allow you to run 'EXEC_FILE_NAME' from anywhere: "
if /i "%ADD_PATH%"=="Y" (
    echo.
    echo Adding to PATH...
    
    REM Get current user PATH (safer than system PATH)
    for /f "tokens=2*" %%A in ('reg query "HKCU\Environment" /v PATH 2^>nul') do set "USER_PATH=%%B"
    
    REM Check if already in PATH
    echo %USER_PATH% | findstr /i "%INSTALL_DIR%" >nul
    if %errorLevel% == 0 (
        echo Directory already in PATH, skipping...
    ) else (
        REM Add to user PATH (safer than system PATH)
        if defined USER_PATH (
            setx PATH "%USER_PATH%;%INSTALL_DIR%" >nul 2>&1
        ) else (
            setx PATH "%INSTALL_DIR%" >nul 2>&1
        )
        
        if %errorLevel% == 0 (
            echo.
            echo ✓ PATH updated successfully!
            echo.
            echo IMPORTANT: You must restart your command prompt for changes to take effect.
            echo After restarting, you can use 'EXEC_FILE_NAME' from anywhere.
            echo.
            echo Alternative: Run this command in a new command prompt to test:
            echo   "%INSTALL_DIR%\EXEC_FILE_NAME" --version
        ) else (
            echo.
            echo ✗ Failed to update PATH automatically.
            echo.
            echo Manual solution:
            echo 1. Press Win+R, type 'sysdm.cpl', press Enter
            echo 2. Click 'Environment Variables'
            echo 3. Under 'User variables', find 'Path' and click 'Edit'
            echo 4. Click 'New' and add: %INSTALL_DIR%
            echo 5. Click OK to save
        )
    )
) else (
    echo.
    echo PATH not modified. You can run the program using:
    echo   "%INSTALL_DIR%\EXEC_FILE_NAME" --version
)

echo.
pause
BATCH_EOF

                # 替换占位符为实际值
                sed -i "s/EXEC_PROJECT_NAME/${PROJECT_NAME}/g" "$pkg_dir/install.bat"
                sed -i "s/EXEC_FILE_NAME/${exec_basename}/g" "$pkg_dir/install.bat"
                ;;
        esac
    fi
    
    # 创建包文件
    cd "$temp_dir"
    
    # 根据平台选择合适的压缩格式
    case "$platform" in
        linux|*linux*)
            tar -czf "$DIST_DIR/${package_name}.tar.gz" "$package_name"
            log_success "已创建: ${package_name}.tar.gz"
            ;;
        windows*|*windows*)
            if command -v zip &> /dev/null; then
                zip -r "$DIST_DIR/${package_name}.zip" "$package_name"
                log_success "已创建: ${package_name}.zip"
            else
                tar -czf "$DIST_DIR/${package_name}.tar.gz" "$package_name"
                log_success "已创建: ${package_name}.tar.gz"
            fi
            ;;
    esac
    
    # 清理临时目录
    rm -rf "$temp_dir"
    
    return 0
}

# 创建多平台合集包
package_multi_platform() {
    log_step "创建多平台合集包..."
    
    local package_name="${PROJECT_NAME}-v${PROJECT_VERSION}-multiplatform"
    local temp_dir=$(mktemp -d)
    local pkg_dir="$temp_dir/$package_name"
    mkdir -p "$pkg_dir"
    
    # 为每个平台创建子目录
    local found_platforms=()
    
    for platform in linux windows-64 windows-32; do
        local build_dir="$BUILDS_DIR/$platform"
        if [[ -d "$build_dir/bin" ]]; then
            local executable=$(find "$build_dir/bin" -name "${EXECUTABLE_NAME}*" -type f 2>/dev/null | head -1)
            if [[ -n "$executable" ]]; then
                local platform_dir="$pkg_dir/$platform"
                mkdir -p "$platform_dir"

                # 按平台决定布局, 与单平台打包保持一致
                case "$platform" in
                    windows*|*windows*)
                        cp "$executable" "$platform_dir/"
                        copy_windows_dependencies "$platform_dir" "$platform"
                        if [[ -d "$PROJECT_ROOT/stdlib" ]]; then
                            cp -r "$PROJECT_ROOT/stdlib" "$platform_dir/stdlib"
                        fi
                        ;;
                    *)
                        mkdir -p "$platform_dir/bin" "$platform_dir/share/apc"
                        cp "$executable" "$platform_dir/bin/"
                        if [[ -d "$PROJECT_ROOT/stdlib" ]]; then
                            cp -r "$PROJECT_ROOT/stdlib" "$platform_dir/share/apc/stdlib"
                        fi
                        ;;
                esac

                found_platforms+=("$platform")
                log_info "添加 $platform 平台"
            fi
        fi
    done
    
    if [[ ${#found_platforms[@]} -eq 0 ]]; then
        log_error "未找到任何平台的编译产物"
        rm -rf "$temp_dir"
        return 1
    fi
    
    # 准备通用文件
    prepare_common_files "$pkg_dir"
    
    # 创建多平台使用说明
    cat > "$pkg_dir/README-MultiPlatform.txt" << EOF
${PROJECT_NAME} v${PROJECT_VERSION} - 多平台版本
=====================================

此包包含以下平台的可执行文件：
$(printf '  - %s\n' "${found_platforms[@]}")

使用方法：
---------

Linux用户：
  cd linux/
  ./bin/$EXECUTABLE_NAME --version
  # 模板库在 linux/share/apc/stdlib/, 自动查找, 无需设置环境变量

Windows用户：
  cd windows-64/    (64位系统)
  cd windows-32/    (32位系统)
  ${EXECUTABLE_NAME}.exe --version
  # 模板库在当前目录的 stdlib/ 下, 自动查找

通用说明：
---------
1. 选择适合您操作系统的目录
2. 运行对应的可执行文件
3. 查看 VERSION 文件了解构建信息
4. 查看 doc/ 目录了解使用文档

EOF
    
    # 打包
    cd "$temp_dir"
    tar -czf "$DIST_DIR/${package_name}.tar.gz" "$package_name"
    
    # 清理临时目录
    rm -rf "$temp_dir"
    
    log_success "已创建: ${package_name}.tar.gz"
    return 0
}

# 主函数
main() {
    echo -e "${CYAN}"
    echo "======================================="
    echo "     跨平台打包脚本"
    echo "======================================="
    echo -e "${NC}"
    
    # 默认设置
    CLEAN_DIST=false
    INCLUDE_DOCS=true
    MINIMAL_PACKAGE=false
    MODE="all"
    PLATFORM=""
    
    # 解析参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -c|--clean)
                CLEAN_DIST=true
                shift
                ;;
            -p|--platform)
                PLATFORM="$2"
                shift 2
                ;;
            --no-docs)
                INCLUDE_DOCS=false
                shift
                ;;
            --minimal)
                MINIMAL_PACKAGE=true
                shift
                ;;
            single|multi|all)
                MODE="$1"
                shift
                ;;
            *)
                log_error "未知参数: $1"
                show_help
                exit 1
                ;;
        esac
    done
    
    # 验证参数
    if [[ "$MODE" == "single" && -z "$PLATFORM" ]]; then
        log_error "single 模式需要指定 --platform"
        exit 1
    fi
    
    # 切换到项目根目录
    cd "$PROJECT_ROOT"
    
    # 解析项目信息
    parse_project_info
    
    # 清理输出目录
    if [[ "$CLEAN_DIST" == "true" ]]; then
        log_step "清理输出目录..."
        rm -rf "$DIST_DIR"
    fi
    
    # 创建输出目录
    mkdir -p "$DIST_DIR"
    
    # 获取版本信息
    get_version_info
    
    # 根据模式执行打包
    case "$MODE" in
        single)
            package_single_platform "$PLATFORM"
            ;;
        multi)
            package_multi_platform
            ;;
        all)
            log_info "打包模式: 所有可用平台"
            local packaged_count=0
            
            # 打包各个单平台版本
            for platform in linux windows-64 windows-32; do
                if [[ -d "$BUILDS_DIR/$platform/bin" ]]; then
                    if package_single_platform "$platform"; then
                        packaged_count=$((packaged_count + 1))
                    fi
                fi
            done
            
            # 创建多平台合集包
            if package_multi_platform; then
                packaged_count=$((packaged_count + 1))
            fi
            
            log_info "共打包 $packaged_count 个版本"
            ;;
    esac
    
    # 显示结果
    echo ""
    log_success "打包完成！"
    log_info "发布产物位置: $DIST_DIR/"
    echo ""
    echo "生成的文件:"
    ls -lh "$DIST_DIR" | grep -v "^total" || echo "  (无文件)"
    
    echo ""
    log_info "使用说明:"
    echo "  1. 分发适合的平台包给用户"
    echo "  2. 用户解压后直接运行程序"
    echo "  3. 多平台包适合需要支持多个系统的场景"
}

# 执行主函数
main "$@" 
