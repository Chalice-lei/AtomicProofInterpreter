#ifndef PASS_TYPE_H
#define PASS_TYPE_H

#include <string>
#include <unordered_map>

enum class DependType {
    LexerPass,
    ParserPass,
    ASTToBytecodePass,
    BytecodePeepholePass,
    BytecodeFinalizePass,
    ExportResultsPass,
    Count
};

inline const std::string& DependTypeToString(DependType dt)
{
    static const std::string names[] = {"LexerPass",
                                        "ParserPass",
                                        "ASTToBytecodePass",
                                        "BytecodePeepholePass",
                                        "BytecodeFinalizePass",
                                        "ExportResultsPass"};
    size_t index = static_cast<size_t>(dt);
    if (index >= static_cast<size_t>(DependType::Count)) {
        static const std::string unknown = "Unknown";
        return unknown;
    }
    return names[index];
}
#endif // PASS_TYPE_H
