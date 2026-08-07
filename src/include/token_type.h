#ifndef TOKEN_TYPE_H
#define TOKEN_TYPE_H

#include <string>
#include <unordered_map>
#include <utility>

enum TokenType {
    TOKEN_INIT,    // __init__ 构造函数
    TOKEN_ARROW,   // -> 函数返回类型

    TOKEN_CONTRACT,
    TOKEN_LIBRARY, // 可被合约导入的自由函数 / 结构体集合
    TOKEN_GLOBAL,  // 合约头部的全局常量声明
    TOKEN_DEF,
    TOKEN_STRUCT,
    TOKEN_INDENT,
    TOKEN_DEDENT,
    TOKEN_NEWLINE,
    TOKEN_EOF,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_HEX,
    TOKEN_STRING,
    TOKEN_ADDRESS,
    TOKEN_TYPE,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_RETURN,       // 大写 Return: 带表达式的返回
    TOKEN_RETURN_VALUE, // 小写 return: 返回函数返回值
    TOKEN_FOR,
    TOKEN_IN,
    TOKEN_ASSIGN,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_COMMA,
    TOKEN_COLON,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_EQUAL,
    TOKEN_NOTEQUAL,
    TOKEN_LESS,
    TOKEN_GREATER,
    TOKEN_LESSEQUAL,
    TOKEN_GREATEREQUAL,
    TOKEN_DOT, // 成员访问
};

struct Position
{
    // line/column 保留 lexer 看到的 import-expanded 源码位置，兼容旧调用点.
    int line;
    int column;
    std::string sourceFilename;
    int sourceLine;
    int sourceColumn;
    bool hasSourceLocation;

    Position()
        : line(0), column(0), sourceLine(0), sourceColumn(0),
          hasSourceLocation(false)
    {}
    Position(int line, int column)
        : line(line), column(column), sourceLine(0), sourceColumn(0),
          hasSourceLocation(false)
    {}
    Position(int expandedLine,
             int expandedColumn,
             std::string filename,
             int mappedLine,
             int mappedColumn)
        : line(expandedLine), column(expandedColumn),
          sourceFilename(std::move(filename)), sourceLine(mappedLine),
          sourceColumn(mappedColumn), hasSourceLocation(!sourceFilename.empty())
    {}
};

struct Token
{
    TokenType type;
    std::string value;
    int length;
    Position position;

    Token(TokenType type, const std::string& value, Position pos = Position())
        : type(type), value(value), length(static_cast<int>(value.size())),
          position(pos)
    {
        position.column = position.column > length ? position.column - length
                                                   : 1;
        if (position.hasSourceLocation) {
            position.sourceColumn =
                position.sourceColumn > length ? position.sourceColumn - length
                                               : 1;
        }
    }
};

static const std::unordered_map<TokenType, std::string> TokenTypeToString = {
    {TOKEN_INIT, "TOKEN_INIT"},
    {TOKEN_ARROW, "TOKEN_ARROW"},
    {TOKEN_CONTRACT, "TOKEN_CONTRACT"},
    {TOKEN_LIBRARY, "TOKEN_LIBRARY"},
    {TOKEN_GLOBAL, "TOKEN_GLOBAL"},
    {TOKEN_DEF, "TOKEN_DEF"},
    {TOKEN_STRUCT, "TOKEN_STRUCT"},
    {TOKEN_INDENT, "TOKEN_INDENT"},
    {TOKEN_DEDENT, "TOKEN_DEDENT"},
    {TOKEN_NEWLINE, "TOKEN_NEWLINE"},
    {TOKEN_EOF, "TOKEN_EOF"},
    {TOKEN_IDENTIFIER, "TOKEN_IDENTIFIER"},
    {TOKEN_NUMBER, "TOKEN_NUMBER"},
    {TOKEN_HEX, "TOKEN_HEX"},
    {TOKEN_STRING, "TOKEN_STRING"},
    {TOKEN_ADDRESS, "TOKEN_ADDRESS"},
    {TOKEN_TYPE, "TOKEN_TYPE"},
    {TOKEN_IF, "TOKEN_IF"},
    {TOKEN_ELSE, "TOKEN_ELSE"},
    {TOKEN_RETURN, "TOKEN_RETURN"},
    {TOKEN_RETURN_VALUE, "TOKEN_RETURN_VALUE"},
    {TOKEN_FOR, "TOKEN_FOR"},
    {TOKEN_IN, "TOKEN_IN"},
    {TOKEN_ASSIGN, "TOKEN_ASSIGN"},
    {TOKEN_LPAREN, "TOKEN_LPAREN"},
    {TOKEN_RPAREN, "TOKEN_RPAREN"},
    {TOKEN_LBRACKET, "TOKEN_LBRACKET"},
    {TOKEN_RBRACKET, "TOKEN_RBRACKET"},
    {TOKEN_LBRACE, "TOKEN_LBRACE"},
    {TOKEN_RBRACE, "TOKEN_RBRACE"},
    {TOKEN_COMMA, "TOKEN_COMMA"},
    {TOKEN_COLON, "TOKEN_COLON"},
    {TOKEN_PLUS, "TOKEN_PLUS"},
    {TOKEN_MINUS, "TOKEN_MINUS"},
    {TOKEN_STAR, "TOKEN_STAR"},
    {TOKEN_SLASH, "TOKEN_SLASH"},
    {TOKEN_EQUAL, "TOKEN_EQUAL"},
    {TOKEN_NOTEQUAL, "TOKEN_NOTEQUAL"},
    {TOKEN_LESS, "TOKEN_LESS"},
    {TOKEN_GREATER, "TOKEN_GREATER"},
    {TOKEN_LESSEQUAL, "TOKEN_LESSEQUAL"},
    {TOKEN_GREATEREQUAL, "TOKEN_GREATEREQUAL"},
    {TOKEN_DOT, "TOKEN_DOT"},
};

#endif // TOKEN_TYPE_H
