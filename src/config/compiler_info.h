#ifndef COMPILER_INFO_H
#define COMPILER_INFO_H

#include <string>

namespace TBC
{

// 编译器核心信息 (编译时确定).
class CompilerInfo
{
public:
#ifdef EXECUTABLE_NAME
    static constexpr const char* NAME = EXECUTABLE_NAME;
#else
    static constexpr const char* NAME = "Compiler";  // fallback
#endif

#ifdef PROJECT_VERSION
    static constexpr const char* VERSION = PROJECT_VERSION;
#else
    static constexpr const char* VERSION = "1.0.0";  // fallback
#endif

#ifdef PROJECT_DESCRIPTION
    static constexpr const char* DESCRIPTION = PROJECT_DESCRIPTION;
#else
    static constexpr const char* DESCRIPTION = "UTXO_Compiler";  // fallback
#endif

    // Git 信息 (构建时固化)
#ifdef GIT_COMMIT_HASH
    static constexpr const char* BUILD_GIT_COMMIT_HASH = GIT_COMMIT_HASH;
#else
    static constexpr const char* BUILD_GIT_COMMIT_HASH = "unknown";  // fallback
#endif

#ifdef GIT_BRANCH_NAME
    static constexpr const char* BUILD_GIT_BRANCH_NAME = GIT_BRANCH_NAME;
#else
    static constexpr const char* BUILD_GIT_BRANCH_NAME = "unknown";  // fallback
#endif

    // 功能开关
    static constexpr bool SUPPORTS_AST_VALIDATION = false;
    static constexpr bool SUPPORTS_BYTECODE_OPTIMIZATION = false;
    static constexpr bool SUPPORTS_SCRIPT_VERIFICATION = false;

    static constexpr const char* DEFAULT_TARGET_ARCH = "TBC_3.1.1";

#ifdef BUILD_TYPE
    static constexpr const char* DEFAULT_BUILD_MODE = BUILD_TYPE;
#else
    static constexpr const char* DEFAULT_BUILD_MODE = "Release"; // fallback
#endif

    static std::string getShortInfo()
    {
        return std::string(NAME) + " v" + VERSION;
    }

    static bool isFeatureSupported(const std::string& feature)
    {
        if (feature == "ast_validation")
            return SUPPORTS_AST_VALIDATION;
        if (feature == "bytecode_optimization")
            return SUPPORTS_BYTECODE_OPTIMIZATION;
        if (feature == "script_verification")
            return SUPPORTS_SCRIPT_VERIFICATION;
        return false;
    }
};

} // namespace TBC

#endif // COMPILER_INFO_H