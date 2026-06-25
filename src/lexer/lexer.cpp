#include "lexer.h"

#include <cctype>
#include <regex>
#include <sstream>

#include "../error/error_manager.h"
#include "../log/logger.h"
#include "../util/byt_fun.h"

static const std::regex emptyOrCommentLine(R"(^[ \t]*(#.*)?$)");

Lexer::Lexer(const std::string& src,
             const std::string& filename,
             const SourceMap* sourceMap)
    : m_source(src), m_filename(filename), m_sourceMap(sourceMap)
{
    // 注册到 ErrorManager 以便错误时显示源码上下文
    if (!m_filename.empty()) {
        ErrorManager::getInstance().setSourceContent(m_filename, m_source);
    }
}

std::vector<Token> Lexer::tokenize()
{
    while (!isAtEnd()) {
        m_start = m_current;
        scanToken();
    }

    // 文件结尾若缺 NEWLINE，补一个隐式 NEWLINE
    if (!m_tokens.empty() && m_tokens.back().type != TokenType::TOKEN_NEWLINE) {
        addToken(TokenType::TOKEN_NEWLINE);
    }

    while (!m_indents.empty()) {
        m_indents.pop_back();
        addToken(TokenType::TOKEN_DEDENT);
    }
    addToken(TokenType::TOKEN_EOF);
    return m_tokens;
}

bool Lexer::isAtEnd() const
{
    return m_current >= m_source.length();
}

char Lexer::advance()
{
    m_column++;
    return m_source[m_current++];
}

char Lexer::peek() const
{
    return isAtEnd() ? '\0' : m_source[m_current];
}

char Lexer::peekNext() const
{
    return m_current + 1 < m_source.length() ? m_source[m_current + 1] : '\0';
}

std::string Lexer::getNextLogicalLine() const
{
    size_t start = m_current;
    while (start < m_source.length() &&
           (m_source[start] == ' ' || m_source[start] == '\t')) {
        start++;
    }
    size_t end = start;
    while (end < m_source.length() && m_source[end] != '\n' &&
           m_source[end] != '\r') {
        end++;
    }
    return m_source.substr(start, end - start);
}

void Lexer::addToken(TokenType type, const std::string& lexeme)
{
    auto value = lexeme.empty() ? m_source.substr(m_start, m_current - m_start)
                                : lexeme;
    tbc::escapeString(value);

    Position position(m_line, m_column);
    if (m_sourceMap) {
        const SourceLineMapping* mapping = m_sourceMap->lookup(m_line);
        if (mapping && mapping->isValid()) {
            position = Position(
                m_line, m_column, mapping->filename, mapping->line, m_column
            );
        }
    }

    m_tokens.emplace_back(type, value, position);
}

void Lexer::scanToken()
{
    skipWhitespace();
    if (isAtEnd())
        return;

    // 避免 token 包含前导空格
    m_start = m_current;

    char c = advance();

    switch (c) {
        case '(':
            addToken(TokenType::TOKEN_LPAREN);
            break;
        case ')':
            addToken(TokenType::TOKEN_RPAREN);
            break;
        case '[':
            addToken(TokenType::TOKEN_LBRACKET);
            break;
        case ']':
            addToken(TokenType::TOKEN_RBRACKET);
            break;
        case '{':
            addToken(TokenType::TOKEN_LBRACE);
            break;
        case '}':
            addToken(TokenType::TOKEN_RBRACE);
            break;
        case '+':
            addToken(TokenType::TOKEN_PLUS);
            break;
        case '-':
            peek() == '>' ? advance(), addToken(TokenType::TOKEN_ARROW)
                          : addToken(TokenType::TOKEN_MINUS);
            break;
        case '*':
            addToken(TokenType::TOKEN_STAR);
            break;
        case '/':
            addToken(TokenType::TOKEN_SLASH);
            break;
        case ':':
            addToken(TokenType::TOKEN_COLON);
            break;
        case '.':
            addToken(TokenType::TOKEN_DOT);
            break;
        case ',':
            addToken(TokenType::TOKEN_COMMA);
            break;
        case '=':
            peek() == '=' ? advance(), addToken(TokenType::TOKEN_EQUAL)
                          : addToken(TokenType::TOKEN_ASSIGN);
            break;
        case '!': {
            if (peek() == '=') {
                advance();
                addToken(TokenType::TOKEN_NOTEQUAL);
            } else {
                SourceLocation loc(m_filename, m_line, m_column - 1, 1);
                LEXICAL_ERROR(
                    "unexpected character '!'",
                    loc,
                    "Use the Not(...) built-in for logical negation, or use "
                    "'!=' for inequality"
                );
                LOG_ERROR(
                    "Lexical error at ",
                    m_filename,
                    ":",
                    m_line,
                    ":",
                    m_column - 1,
                    " - unexpected character '!'"
                );
            }
        } break;
        case '<':
            peek() == '=' ? advance(), addToken(TokenType::TOKEN_LESSEQUAL)
                          : addToken(TokenType::TOKEN_LESS);
            break;
        case '>':
            peek() == '=' ? advance(), addToken(TokenType::TOKEN_GREATEREQUAL)
                          : addToken(TokenType::TOKEN_GREATER);
            break;
        case '\r': {
            // 兼容 Windows \r\n
            if (peek() == '\n') {
                advance();
            }
            addToken(TokenType::TOKEN_NEWLINE);
            m_line++;
            m_column = 1;
            // 空行/注释行不触发 handleIndentation
            std::string nextLine = getNextLogicalLine();
            if (!std::regex_match(nextLine, emptyOrCommentLine)) {
                handleIndentation();
            }
        } break;
        case '\n': {
            addToken(TokenType::TOKEN_NEWLINE);
            m_line++;
            m_column = 1;
            std::string nextLine = getNextLogicalLine();
            if (!std::regex_match(nextLine, emptyOrCommentLine)) {
                handleIndentation();
            }
        } break;
        case '"':
            readString();
            break;
        default:
            readAlphaNumeric(c);
            break;
    }
}

void Lexer::handleIndentation()
{
    size_t lineStart = m_current;
    while (lineStart < m_source.length() &&
           (m_source[lineStart] == ' ' || m_source[lineStart] == '\t')) {
        lineStart++;
        m_column++;
    }

    // 空行/注释行不触发缩进
    if (lineStart >= m_source.length() || m_source[lineStart] == '\n' ||
        m_source[lineStart] == '\r' || m_source[lineStart] == '#') {
        m_line++;
        return;
    }

    int newIndent = static_cast<int>(lineStart - m_current);
    m_current = lineStart;

    if (m_indents.empty()) {
        if (newIndent > 0) {
            m_indents.push_back(newIndent);
            addToken(TokenType::TOKEN_INDENT);
        }
        return;
    }

    int lastIndent = m_indents.back();
    if (newIndent > lastIndent) {
        m_indents.push_back(newIndent);
        addToken(TokenType::TOKEN_INDENT);
    } else if (newIndent < lastIndent) {
        while (!m_indents.empty() && m_indents.back() > newIndent) {
            m_indents.pop_back();
            addToken(TokenType::TOKEN_DEDENT);
        }
        // 退到顶层时 m_indents 为空是正常的，残留对不齐才算不一致
        if ((m_indents.empty() && newIndent != 0) ||
            (!m_indents.empty() && m_indents.back() != newIndent)) {
            SourceLocation loc(m_filename, m_line, m_column);
            std::ostringstream oss;
            oss << "inconsistent dedent level. Expected indentation to match a "
                   "previous level, got "
                << newIndent << " spaces";

            LOG_ERROR(
                "Lexical error at ",
                m_filename,
                ":",
                m_line,
                ":",
                m_column,
                " - ",
                oss.str()
            );
            LEXICAL_ERROR(
                oss.str(),
                loc,
                "Check your indentation levels and ensure they match "
                "previous indentation"
            );
            return; // 不抛异常，让编译继续
        }
    }
}

void Lexer::readString()
{
    auto start = m_current;
    while (!isAtEnd()) {
        char c = peek();

        if (c == '"') {
            break;
        } else if (c == '\\') {
            advance(); // 反斜杠 + 被转义字符
            if (!isAtEnd()) {
                if (peek() == '\n') {
                    m_line++;
                    m_column = 1;
                }
                advance();
            }
        } else {
            if (c == '\n') {
                m_line++;
                m_column = 1;
            }
            advance();
        }
    }

    if (isAtEnd()) {
        SourceLocation loc(m_filename, m_line, m_column - (m_current - start));
        LOG_ERROR(
            "Lexical error at ",
            m_filename,
            ":",
            m_line,
            ":",
            m_column - (m_current - start),
            " - unterminated string literal"
        );
        LEXICAL_ERROR(
            "unterminated string literal",
            loc,
            "Add closing quote (\") to terminate the string"
        );
        return;
    }

    auto end = m_current;
    advance(); // closing "
    std::string strValue = m_source.substr(start, end - start);

    // 比特币地址：仅支持 1 开头的 Base58 P2PKH（34 字符）
    if (isBitcoinAddress(strValue)) {
        addToken(TokenType::TOKEN_ADDRESS, strValue);
    } else {
        addToken(TokenType::TOKEN_STRING, strValue);
    }
}

void Lexer::readAlphaNumeric(char c)
{
    auto shouldStop = [](char ch) {
        return std::isspace(ch) || (std::ispunct(ch) && ch != '_');
    };

    bool isNumber = std::isdigit(c);
    bool isHexNumber = false;

    // 0x/0X 开头：十六进制
    if (c == '0' && (peek() == 'x' || peek() == 'X')) {
        isHexNumber = true;
        advance();
        isNumber = false;
    }

    while (!isAtEnd() && !shouldStop(peek())) {
        char nextChar = peek();

        if (isNumber && !std::isdigit(nextChar)) {
            isNumber = false;
        }

        if (isHexNumber && !std::isxdigit(nextChar)) {
            isHexNumber = false;
        }

        advance();
    }

    std::string text = m_source.substr(m_start, m_current - m_start);

    // 优先级：数字 > 十六进制 > 关键字 > 标识符/类型
    if (isNumber) {
        addToken(TokenType::TOKEN_NUMBER);
        return;
    }

    if (isHexNumber) {
        // 0x 后须至少一个十六进制字符，否则视作标识符
        if (text.length() > 2) {
            addToken(TokenType::TOKEN_HEX);
            return;
        }
    }

    static std::unordered_map<std::string, TokenType> keywords =
        {{"def", TokenType::TOKEN_DEF},
         {"Struct", TokenType::TOKEN_STRUCT},
         {"Contract", TokenType::TOKEN_CONTRACT},
         {"Library", TokenType::TOKEN_LIBRARY},
         {"if", TokenType::TOKEN_IF},
         {"else", TokenType::TOKEN_ELSE},
         // Return：返回指定表达式；return：返回当前函数的返回值
         {"Return", TokenType::TOKEN_RETURN},
         {"return", TokenType::TOKEN_RETURN_VALUE},
         {"for", TokenType::TOKEN_FOR},
         {"in", TokenType::TOKEN_IN}};

    auto it = keywords.find(text);
    if (it != keywords.end()) {
        addToken(it->second);
        return;
    }

    // 由上下文决定 类型 vs 标识符
    TokenType prevToken = getPreviousTokenType();
    bool isTypeContext = false;

    if (prevToken == TokenType::TOKEN_COLON) {
        isTypeContext = true;
    } else if (prevToken == TokenType::TOKEN_ARROW) {
        isTypeContext = true;
    } else if (prevToken == TokenType::TOKEN_LBRACE && isInBraceTypeContext()) {
        // 返回类型大括号内：'{' 后是类型
        isTypeContext = true;
    } else if (prevToken == TokenType::TOKEN_COMMA && isInBraceTypeContext()) {
        // 返回类型大括号内：',' 后是类型
        isTypeContext = true;
    }

    TokenType tokenType = isTypeContext ? TokenType::TOKEN_TYPE
                                        : TokenType::TOKEN_IDENTIFIER;

    if (tokenType == TokenType::TOKEN_IDENTIFIER) {
        static const std::regex identifierPattern("^[_a-zA-Z][_a-zA-Z0-9]*$");
        if (!std::regex_match(text, identifierPattern)) {
            SourceLocation loc(
                m_filename, m_line, m_column - text.length(), text.length()
            );
            std::ostringstream oss;
            oss << "invalid identifier '" << text << "'";

            bool hasNonAscii = false;
            for (char ch : text) {
                if (static_cast<unsigned char>(ch) > 127) {
                    hasNonAscii = true;
                    break;
                }
            }

            std::string suggestion =
                hasNonAscii
                    ? "Identifiers cannot contain non-ASCII characters. Use "
                      "ASCII letters, digits, and underscores only"
                    : "Identifiers must start with a letter or underscore, "
                      "followed by letters, digits, or underscores";

            LOG_ERROR(
                "Lexical error at ",
                m_filename,
                ":",
                m_line,
                ":",
                m_column - text.length(),
                " - ",
                oss.str()
            );
            LEXICAL_ERROR(oss.str(), loc, suggestion);
            return; // 不抛异常，让编译继续
        }
    }

    addToken(tokenType);
}

void Lexer::skipWhitespace()
{
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t') {
            advance();
        } else if (c == '#') {
            skipComment();
        } else {
            break;
        }
    }
}

void Lexer::skipComment()
{
    while (peek() != '\n' && !isAtEnd()) {
        advance();
    }
}

TokenType Lexer::getPreviousTokenType() const
{
    if (m_tokens.empty()) {
        return TOKEN_EOF;
    }
    return m_tokens.back().type;
}

bool Lexer::isInBraceTypeContext() const
{
    // 大括号类型上下文：函数返回类型 -> {type1, type2}
    if (m_tokens.size() < 2) {
        return false;
    }

    // 从后往前找最近的 TOKEN_LBRACE
    for (int i = static_cast<int>(m_tokens.size()) - 1; i >= 0; i--) {
        TokenType tokenType = m_tokens[i].type;

        if (tokenType == TOKEN_LBRACE) {
            // '{' 前是 ARROW 才是返回类型上下文
            if (i > 0 && m_tokens[i - 1].type == TOKEN_ARROW) {
                // '{' 与当前位置之间出现 TOKEN_RETURN 说明已进入函数体
                for (int j = i + 1; j < static_cast<int>(m_tokens.size());
                     j++) {
                    if (m_tokens[j].type == TOKEN_RETURN) {
                        return false;
                    }
                }
                return true;
            }
            return false;
        }

        // 这些 token 表示已离开当前大括号上下文
        if (tokenType == TOKEN_RBRACE || tokenType == TOKEN_NEWLINE ||
            tokenType == TOKEN_COLON) {
            break;
        }
    }

    return false;
}

bool Lexer::isBitcoinAddress(const std::string& str) const
{
    if (str.empty()) {
        return false;
    }

    // 仅支持 1 开头的 Base58 P2PKH 地址（34 字符）
    if (str.length() == 34 && str[0] == '1') {
        static const std::regex base58Pattern(
            "^1[123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]{33}"
            "$"
        );
        if (std::regex_match(str, base58Pattern)) {
            return true;
        }
    }

    return false;
}
