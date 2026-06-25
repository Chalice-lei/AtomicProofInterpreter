#include "error_manager.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace
{
void replaceAll(std::string& text,
                const std::string& from,
                const std::string& to)
{
    if (from.empty()) {
        return;
    }

    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.length(), to);
        pos += to.length();
    }
}
} // namespace

ErrorManager& ErrorManager::getInstance()
{
    static ErrorManager instance;
    return instance;
}

void ErrorManager::reportError(const ErrorInfo& error)
{
    errors_.push_back(error);

    switch (error.severity) {
        case ErrorSeverity::ERROR:
        case ErrorSeverity::FATAL:
            errorCount_++;
            if (error.severity == ErrorSeverity::FATAL) {
                fatalErrorCount_++;
            }
            break;
        case ErrorSeverity::WARNING:
            warningCount_++;
            break;
        default:
            break;
    }

    printError(error);

    if (errorCount_ >= maxErrors_) {
        std::cerr << "Too many errors (" << errorCount_
                  << "), stopping compilation." << std::endl;
        exit(1);
    }

    if (error.severity == ErrorSeverity::FATAL) {
        std::cerr << "Fatal error encountered, compilation terminated."
                  << std::endl;
        exit(1);
    }
}

void ErrorManager::reportError(ErrorSeverity severity,
                               ErrorCategory category,
                               const std::string& message,
                               const SourceLocation& location,
                               const std::string& suggestion)
{
    ErrorInfo error(severity, category, message, location);
    error.suggestion = suggestion;
    reportError(error);
}

void ErrorManager::lexicalError(const std::string& message,
                                const SourceLocation& location,
                                const std::string& suggestion)
{
    reportError(ErrorSeverity::ERROR,
                ErrorCategory::LEXICAL,
                message,
                location,
                suggestion);
}

void ErrorManager::syntaxError(const std::string& message,
                               const SourceLocation& location,
                               const std::string& suggestion)
{
    reportError(ErrorSeverity::ERROR,
                ErrorCategory::SYNTAX,
                message,
                location,
                suggestion);
}

void ErrorManager::semanticError(const std::string& message,
                                 const SourceLocation& location,
                                 const std::string& suggestion)
{
    reportError(ErrorSeverity::ERROR,
                ErrorCategory::SEMANTIC,
                message,
                location,
                suggestion);
}

void ErrorManager::typeError(const std::string& message,
                             const SourceLocation& location,
                             const std::string& suggestion)
{
    reportError(ErrorSeverity::ERROR,
                ErrorCategory::TYPE,
                message,
                location,
                suggestion);
}

void ErrorManager::internalError(const std::string& message,
                                 const SourceLocation& location,
                                 const std::string& suggestion)
{
    reportError(ErrorSeverity::FATAL,
                ErrorCategory::INTERNAL,
                message,
                location,
                suggestion);
}

void ErrorManager::warning(const std::string& message,
                           const SourceLocation& location)
{
    reportError(ErrorSeverity::WARNING,
                ErrorCategory::SYNTAX,
                message,
                location);
}

void ErrorManager::note(const std::string& message,
                        const SourceLocation& location)
{
    reportError(ErrorSeverity::NOTE, ErrorCategory::SYNTAX, message, location);
}

void ErrorManager::setSourceContent(const std::string& filename,
                                    const std::string& content)
{
    sourceFiles_[filename] = content;
}

void ErrorManager::setSourceMap(const SourceMap& sourceMap)
{
    sourceMap_ = sourceMap;
    hasSourceMap_ = !sourceMap_.empty();
}

SourceLocation
ErrorManager::resolveLocation(const SourceLocation& location) const
{
    if (!hasSourceMap_ || location.line <= 0 || !location.filename.empty()) {
        return location;
    }

    const SourceLineMapping* mapping = sourceMap_.lookup(location.line);
    if (!mapping || !mapping->isValid()) {
        return location;
    }

    SourceLocation resolved = location;
    resolved.filename = mapping->filename;
    resolved.line = mapping->line;
    return resolved;
}

std::string ErrorManager::resolveMessageLocation(
    const std::string& message,
    const SourceLocation& originalLocation,
    const SourceLocation& resolvedLocation
) const
{
    if (originalLocation.line <= 0 ||
        originalLocation.line == resolvedLocation.line) {
        return message;
    }

    std::string resolvedMessage = message;
    replaceAll(
        resolvedMessage,
        "line " + std::to_string(originalLocation.line) + ", column " +
            std::to_string(originalLocation.column),
        "line " + std::to_string(resolvedLocation.line) + ", column " +
            std::to_string(resolvedLocation.column)
    );
    return resolvedMessage;
}

void ErrorManager::printError(const ErrorInfo& error) const
{
    ErrorInfo resolvedError = error;
    resolvedError.location = resolveLocation(error.location);
    resolvedError.message = resolveMessageLocation(
        error.message, error.location, resolvedError.location
    );

    std::string colorStart = colorOutput_ ? getColorCode(error.severity) : "";
    std::string colorEnd = colorOutput_ ? "\033[0m" : "";

    // 格式: filename:line:column: severity: message
    std::string filename = resolvedError.location.filename;

    // 缺 filename 但有位置时, 退化到第一个已知源文件
    if (filename.empty() && resolvedError.location.line > 0 &&
        !sourceFiles_.empty()) {
        filename = sourceFiles_.begin()->first;
    }

    if (!filename.empty()) {
        std::cerr << filename;
        if (resolvedError.location.line > 0) {
            std::cerr << ":" << resolvedError.location.line;
            if (resolvedError.location.column > 0) {
                std::cerr << ":" << resolvedError.location.column;
            }
        }
        std::cerr << ": ";
    }

    std::cerr << colorStart << getSeverityString(error.severity) << ": "
              << resolvedError.message << colorEnd << std::endl;

    if (showContext_) {
        printContext(resolvedError);
    }

    if (!error.suggestion.empty()) {
        std::cerr << colorStart << "note: " << error.suggestion << colorEnd
                  << std::endl;
    }

    std::cerr << std::endl;
}

void ErrorManager::printContext(const ErrorInfo& error) const
{
    ErrorInfo resolvedError = error;
    resolvedError.location = resolveLocation(error.location);

    if (resolvedError.location.line <= 0) {
        return;
    }

    std::string filename = resolvedError.location.filename;

    if (filename.empty() && !sourceFiles_.empty()) {
        filename = sourceFiles_.begin()->first;
    }

    if (filename.empty()) {
        return;
    }

    auto it = sourceFiles_.find(filename);
    if (it == sourceFiles_.end()) {
        return;
    }

    const std::string& content = it->second;
    std::istringstream iss(content);
    std::string line;
    int currentLine = 1;

    while (std::getline(iss, line) &&
           currentLine < resolvedError.location.line) {
        currentLine++;
    }

    if (currentLine == resolvedError.location.line) {
        std::cerr << "    " << line << std::endl;

        if (resolvedError.location.column > 0) {
            std::string indicator(
                resolvedError.location.column + 3, ' '
            ); // +3 for " "
            indicator +=
                std::string(std::max(1, resolvedError.location.length), '^');
            std::cerr << indicator << std::endl;
        }
    }
}

std::string ErrorManager::getSeverityString(ErrorSeverity severity) const
{
    switch (severity) {
        case ErrorSeverity::NOTE:
            return "note";
        case ErrorSeverity::WARNING:
            return "warning";
        case ErrorSeverity::ERROR:
            return "error";
        case ErrorSeverity::FATAL:
            return "fatal error";
        default:
            return "unknown";
    }
}

std::string ErrorManager::getCategoryString(ErrorCategory category) const
{
    switch (category) {
        case ErrorCategory::LEXICAL:
            return "lexical";
        case ErrorCategory::SYNTAX:
            return "syntax";
        case ErrorCategory::SEMANTIC:
            return "semantic";
        case ErrorCategory::TYPE:
            return "type";
        case ErrorCategory::LINKER:
            return "linker";
        case ErrorCategory::IO:
            return "io";
        case ErrorCategory::INTERNAL:
            return "internal";
        default:
            return "unknown";
    }
}

std::string ErrorManager::getColorCode(ErrorSeverity severity) const
{
    switch (severity) {
        case ErrorSeverity::NOTE:
            return "\033[36m"; // 青色
        case ErrorSeverity::WARNING:
            return "\033[33m"; // 黄色
        case ErrorSeverity::ERROR:
            return "\033[31m"; // 红色
        case ErrorSeverity::FATAL:
            return "\033[35m"; // 紫色
        default:
            return "";
    }
}

void ErrorManager::printAllErrors() const
{
    if (errors_.empty()) {
        return;
    }

    std::cerr << "\n=== Compilation Summary ===" << std::endl;
    std::cerr << "Errors: " << errorCount_ << std::endl;
    std::cerr << "Warnings: " << warningCount_ << std::endl;

    if (errorCount_ > 0) {
        std::cerr << "Compilation failed." << std::endl;
    } else {
        std::cerr << "Compilation completed successfully." << std::endl;
    }
}

void ErrorManager::clear()
{
    errors_.clear();
    sourceFiles_.clear();
    sourceMap_ = SourceMap();
    hasSourceMap_ = false;
    errorCount_ = 0;
    warningCount_ = 0;
    fatalErrorCount_ = 0;
}
