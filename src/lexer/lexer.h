#ifndef LEXER_H
#define LEXER_H

#include <iostream>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../error/error_manager.h"
#include "../include/token_type.h"
#include "../source/source_map.h"

class Lexer
{
public:
    Lexer(const std::string& source,
          const std::string& filename = "",
          const SourceMap* sourceMap = nullptr);
    std::vector<Token> tokenize();

    bool isAtEnd() const;
    char advance();
    char peek() const;
    char peekNext() const;
    std::string getNextLogicalLine() const;
    void addToken(TokenType type, const std::string& lexeme = "");
    void scanToken();
    void handleIndentation();
    void readString();
    void readAlphaNumeric(char c);
    void skipWhitespace();
    void skipComment();

    TokenType getPreviousTokenType() const;

    // 是否在 -> {type1, type2} 类型列表上下文
    bool isInBraceTypeContext() const;

    bool isBitcoinAddress(const std::string& str) const;

private:
    std::string m_source;
    std::string m_filename;
    const SourceMap* m_sourceMap = nullptr;
    size_t m_start = 0;
    size_t m_current = 0;
    int m_line = 1;
    int m_column = 1;
    std::vector<Token> m_tokens;
    std::vector<int> m_indents;
};

#endif // LEXER_H
