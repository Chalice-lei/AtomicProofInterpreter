#ifndef ERROR_TYPES_H
#define ERROR_TYPES_H

#include <string>
#include <vector>

enum class ErrorSeverity {
    NOTE,
    WARNING,
    ERROR,
    FATAL
};

enum class ErrorCategory {
    LEXICAL,
    SYNTAX,
    SEMANTIC,
    TYPE,
    LINKER,
    IO,
    INTERNAL
};

struct SourceLocation
{
    std::string filename;
    int line;
    int column;
    int length; // 错误标记长度

    SourceLocation(const std::string& file = "",
                   int l = 0,
                   int c = 0,
                   int len = 1)
        : filename(file), line(l), column(c), length(len)
    {}
};

struct ErrorInfo
{
    ErrorSeverity severity;
    ErrorCategory category;
    std::string code; // e.g. "E001"
    std::string message;
    SourceLocation location;
    std::string suggestion;
    std::vector<std::string> context;

    ErrorInfo(ErrorSeverity sev,
              ErrorCategory cat,
              const std::string& msg,
              const SourceLocation& loc = SourceLocation())
        : severity(sev), category(cat), message(msg), location(loc)
    {}
};

#endif // ERROR_TYPES_H