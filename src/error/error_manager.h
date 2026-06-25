#ifndef ERROR_MANAGER_H
#define ERROR_MANAGER_H

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "error_types.h"
#include "../source/source_map.h"

class ErrorManager
{
public:
    static ErrorManager& getInstance();

    void reportError(const ErrorInfo& error);
    void reportError(ErrorSeverity severity,
                     ErrorCategory category,
                     const std::string& message,
                     const SourceLocation& location = SourceLocation(),
                     const std::string& suggestion = "");

    void lexicalError(const std::string& message,
                      const SourceLocation& location,
                      const std::string& suggestion = "");
    void syntaxError(const std::string& message,
                     const SourceLocation& location,
                     const std::string& suggestion = "");
    void semanticError(const std::string& message,
                       const SourceLocation& location,
                       const std::string& suggestion = "");
    void typeError(const std::string& message,
                   const SourceLocation& location,
                   const std::string& suggestion = "");
    void internalError(const std::string& message,
                       const SourceLocation& location,
                       const std::string& suggestion = "");

    void warning(const std::string& message,
                 const SourceLocation& location = SourceLocation());
    void note(const std::string& message,
              const SourceLocation& location = SourceLocation());

    // 提供源代码内容用于上下文显示.
    void setSourceContent(const std::string& filename,
                          const std::string& content);
    void setSourceMap(const SourceMap& sourceMap);

    bool hasErrors() const
    {
        return errorCount_ > 0;
    }
    bool hasFatalErrors() const
    {
        return fatalErrorCount_ > 0;
    }
    int getErrorCount() const
    {
        return errorCount_;
    }
    int getWarningCount() const
    {
        return warningCount_;
    }

    void printAllErrors() const;
    void clear();

    void setColorOutput(bool enable)
    {
        colorOutput_ = enable;
    }
    void setShowContext(bool enable)
    {
        showContext_ = enable;
    }
    void setMaxErrors(int max)
    {
        maxErrors_ = max;
    }

private:
    ErrorManager() = default;

    SourceLocation resolveLocation(const SourceLocation& location) const;
    std::string resolveMessageLocation(
        const std::string& message,
        const SourceLocation& originalLocation,
        const SourceLocation& resolvedLocation
    ) const;
    void printError(const ErrorInfo& error) const;
    void printContext(const ErrorInfo& error) const;
    std::string getSeverityString(ErrorSeverity severity) const;
    std::string getCategoryString(ErrorCategory category) const;
    std::string getColorCode(ErrorSeverity severity) const;

    std::vector<ErrorInfo> errors_;
    std::unordered_map<std::string, std::string> sourceFiles_;
    SourceMap sourceMap_;
    bool hasSourceMap_ = false;

    int errorCount_ = 0;
    int warningCount_ = 0;
    int fatalErrorCount_ = 0;
    int maxErrors_ = 20;
    bool colorOutput_ = true;
    bool showContext_ = true;
};

#define LEXICAL_ERROR(msg, loc, ...)                                           \
    ErrorManager::getInstance().lexicalError(msg, loc, ##__VA_ARGS__)

#define SYNTAX_ERROR(msg, loc, ...)                                            \
    ErrorManager::getInstance().syntaxError(msg, loc, ##__VA_ARGS__)

#define SEMANTIC_ERROR(msg, loc, ...)                                          \
    ErrorManager::getInstance().semanticError(msg, loc, ##__VA_ARGS__)

#define TYPE_ERROR(msg, loc, ...)                                              \
    ErrorManager::getInstance().typeError(msg, loc, ##__VA_ARGS__)

#define INTERNAL_ERROR(msg, loc, ...)                                          \
    ErrorManager::getInstance().internalError(msg, loc, ##__VA_ARGS__)

#define COMPILER_WARNING(msg, ...)                                             \
    ErrorManager::getInstance().warning(msg, ##__VA_ARGS__)

#define COMPILER_NOTE(msg, ...)                                                \
    ErrorManager::getInstance().note(msg, ##__VA_ARGS__)

#endif // ERROR_MANAGER_H
