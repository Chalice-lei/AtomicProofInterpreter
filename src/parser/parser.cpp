#include "parser.h"

#include <sstream>
#include <stdexcept>
#include <vector>

#include "../error/error_manager.h"
#include "../log/logger.h"

Token Parser::current()
{
    if (isAtEnd()) {
        SourceLocation loc(
            "",
            m_pos < m_tokens.size() ? m_tokens[m_pos].position.line : 0,
            m_pos < m_tokens.size() ? m_tokens[m_pos].position.column : 0
        );
        SYNTAX_ERROR(
            "Unexpected end of input",
            loc,
            "Check if the source file is complete"
        );
        LOG_ERROR(
            "Parser: Unexpected end of input at position " +
            std::to_string(m_pos)
        );
        throw std::runtime_error("Unexpected end of input");
    }
    return m_tokens[m_pos];
}

Token Parser::previous()
{
    if (m_pos == 0) {
        SourceLocation loc("", 1, 1);
        INTERNAL_ERROR(
            "Parser internal error: unexpected m_pos value",
            loc,
            "This is likely a parser bug"
        );
        LOG_ERROR("Parser: Unexpected m_pos value 0 when calling previous()");
        throw std::runtime_error("Unexpected m_pos");
    }
    return m_tokens[m_pos - 1];
}

// 不移动位置
Token Parser::peekNext()
{
    if (isAtEnd() || m_pos + 1 >= m_tokens.size()) {
        SourceLocation loc(
            "",
            m_pos < m_tokens.size() ? m_tokens[m_pos].position.line : 0,
            m_pos < m_tokens.size() ? m_tokens[m_pos].position.column : 0
        );
        SYNTAX_ERROR(
            "Unexpected end of input when peeking next token",
            loc,
            "Check if the source file is complete"
        );
        LOG_ERROR(
            "Parser: Unexpected end of input when calling peekNext() at "
            "position " +
            std::to_string(m_pos)
        );
        throw std::runtime_error("Unexpected end of input");
    }
    return m_tokens[m_pos + 1];
}

Token Parser::advance()
{
    if (!isAtEnd()) {
        m_pos++;
    }
    return previous();
}

bool Parser::isAtEnd()
{
    return m_pos >= m_tokens.size() || m_tokens[m_pos].type == TOKEN_EOF;
}

bool Parser::check(const TokenType type, int add)
{
    if (isAtEnd() || m_pos + add >= m_tokens.size()) {
        return false;
    }
    return m_tokens[m_pos + add].type == type;
}

bool Parser::match(const TokenType type)
{
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

void Parser::consume(const TokenType type, const std::string& message)
{
    if (!match(type)) {
        std::string errorMsg = message;

        if (m_pos < m_tokens.size()) {
            const auto& currentToken = m_tokens[m_pos];
            errorMsg += " at line " +
                        std::to_string(currentToken.position.line) +
                        ", column " +
                        std::to_string(currentToken.position.column);

            if (TokenTypeToString.find(type) != TokenTypeToString.end()) {
                errorMsg += ". Expected token: " + TokenTypeToString.at(type);
            }

            if (TokenTypeToString.find(currentToken.type) !=
                TokenTypeToString.end()) {
                errorMsg += ", but found: " +
                            TokenTypeToString.at(currentToken.type);
                if (!currentToken.value.empty()) {
                    errorMsg += " ('" + currentToken.value + "')";
                }
            }
        } else if (m_pos >= m_tokens.size()) {
            errorMsg += " at end of file. Unexpected end of input";
            if (TokenTypeToString.find(type) != TokenTypeToString.end()) {
                errorMsg += ". Expected token: " + TokenTypeToString.at(type);
            }
        }

        SourceLocation loc(
            "",
            m_pos < m_tokens.size() ? m_tokens[m_pos].position.line : 1,
            m_pos < m_tokens.size() ? m_tokens[m_pos].position.column : 1
        );
        LOG_ERROR(
            "Syntax error at line ",
            loc.line,
            ", column ",
            loc.column,
            " - ",
            errorMsg
        );
        SYNTAX_ERROR(errorMsg, loc, "Check the syntax around this location");

        throw std::runtime_error(errorMsg);
    }
}

// (Library)* Contract EOF：允许多个 Library（来自 import 展开），后跟唯一 Contract
std::shared_ptr<ContractNode> Parser::parseContract()
{
    while (match(TOKEN_NEWLINE)) {
    }
    if (isAtEnd()) {
        SourceLocation loc("", 1, 1);
        LOG_ERROR("Syntax error at line 1, column 1 - empty source file");
        SYNTAX_ERROR(
            "empty source file", loc, "Add a contract definition to the file"
        );
        return std::make_shared<ContractNode>("EmptyContract", 1, 1);
    }
    // 先收集 Library 块，Contract 解析完成后再挂上
    std::vector<std::unique_ptr<LibraryNode>> pendingLibraries;
    while (check(TOKEN_LIBRARY)) {
        pendingLibraries.push_back(parseLibrary());
        while (match(TOKEN_NEWLINE)) {
        }
    }
    consume(TOKEN_CONTRACT, "Expected 'Contract'!");
    consume(TOKEN_IDENTIFIER, "Expected contract name!");
    auto token = previous();
    auto contractPtr = std::make_shared<ContractNode>(
        token.value, token.position.line, token.position.column
    );
    LOG_DEBUG("create ", contractPtr->name, " Contract");

    consume(TOKEN_COLON, "Expected ':' after contract definition!");
    while (match(TOKEN_NEWLINE)) {
    }
    consume(TOKEN_INDENT, "Expected 'indent'!");
    bool sawMemberDeclaration = false;
    while (!check(TOKEN_DEDENT) && !isAtEnd()) {
        if (match(TOKEN_NEWLINE)) {
            continue;
        }
        if (check(TOKEN_GLOBAL)) {
            if (sawMemberDeclaration) {
                SourceLocation loc(
                    "", current().position.line, current().position.column
                );
                const std::string message =
                    "global declarations must appear before any Struct or def "
                    "declaration in a Contract";
                SYNTAX_ERROR(
                    message,
                    loc,
                    "Move this global declaration to the beginning of the "
                    "Contract body"
                );
                LOG_ERROR(message);
                throw std::runtime_error(message);
            }
            contractPtr->globalConstants.push_back(parseGlobalConst());
        } else if (check(TOKEN_DEF)) {
            sawMemberDeclaration = true;
            // 构造函数判定
            if ((TOKEN_IDENTIFIER == peekNext().type &&
                 "__init__" == peekNext().value) ||
                TOKEN_INIT == peekNext().type) {
                contractPtr->members.push_back(parseConstructor());
            } else {
                contractPtr->members.push_back(parseFunction());
            }
        } else if (check(TOKEN_STRUCT)) {
            sawMemberDeclaration = true;
            contractPtr->members.push_back(parseStruct());
        } else {
            SourceLocation loc(
                "", current().position.line, current().position.column
            );
            std::ostringstream oss;
            oss << "expected function or struct declaration, got '"
                << current().value << "'";
            LOG_ERROR(
                "Syntax error at line ",
                loc.line,
                ", column ",
                loc.column,
                " - ",
                oss.str()
            );
            SYNTAX_ERROR(
                oss.str(),
                loc,
                "Use 'def' to define a function or 'Struct' to define "
                "a structure"
            );
            throw std::runtime_error(oss.str());
        }
    }

    consume(TOKEN_DEDENT, "Expected dedent after contract body");
    while (match(TOKEN_NEWLINE)) {
    }
    if (!isAtEnd()) {
        SourceLocation loc(
            "", current().position.line, current().position.column
        );
        std::ostringstream oss;
        oss << "unexpected token after contract definition: '"
            << current().value << "'";
        LOG_ERROR(
            "Syntax error at line ",
            loc.line,
            ", column ",
            loc.column,
            " - ",
            oss.str()
        );
        SYNTAX_ERROR(
            oss.str(),
            loc,
            "A source file may contain libraries followed by one contract; "
            "remove trailing tokens"
        );
        throw std::runtime_error(oss.str());
    }

    contractPtr->libraries = std::move(pendingLibraries);
    return contractPtr;
}

std::unique_ptr<GlobalConstNode> Parser::parseGlobalConst()
{
    consume(TOKEN_GLOBAL, "Expected 'global'!");
    const Token globalToken = previous();

    consume(TOKEN_IDENTIFIER, "Expected identifier after 'global'");
    const Token nameToken = previous();
    consume(TOKEN_ASSIGN, "Expected '=' after global constant name");

    std::unique_ptr<LiteralNode> initializer;
    if (match(TOKEN_NUMBER)) {
        const Token literal = previous();
        initializer = std::make_unique<LiteralNode>(
            LiteralNode::Type::Number,
            literal.value,
            literal.position.line,
            literal.position.column
        );
    } else if (match(TOKEN_STRING)) {
        const Token literal = previous();
        initializer = std::make_unique<LiteralNode>(
            LiteralNode::Type::String,
            literal.value,
            literal.position.line,
            literal.position.column
        );
    } else if (match(TOKEN_HEX)) {
        const Token literal = previous();
        initializer = std::make_unique<LiteralNode>(
            LiteralNode::Type::Hex,
            literal.value,
            literal.position.line,
            literal.position.column
        );
    } else if (match(TOKEN_ADDRESS)) {
        const Token literal = previous();
        initializer = std::make_unique<LiteralNode>(
            LiteralNode::Type::Addr,
            literal.value,
            literal.position.line,
            literal.position.column
        );
    } else {
        SourceLocation loc(
            "", current().position.line, current().position.column
        );
        const std::string message =
            "global constant initializer must be a scalar literal";
        SYNTAX_ERROR(
            message,
            loc,
            "Use a number, string, hexadecimal, or address literal"
        );
        LOG_ERROR(message);
        throw std::runtime_error(message);
    }

    consume(TOKEN_NEWLINE, "Expected newline after global declaration");
    return std::make_unique<GlobalConstNode>(
        nameToken.value,
        std::move(initializer),
        globalToken.position.line,
        globalToken.position.column
    );
}

// Library <Name>: INDENT (def | Struct)+ DEDENT
// 禁止 __init__ / init 构造及 main def
std::unique_ptr<LibraryNode> Parser::parseLibrary()
{
    consume(TOKEN_LIBRARY, "Expected 'Library'!");
    consume(TOKEN_IDENTIFIER, "Expected library name!");
    auto nameTok = previous();
    std::string fullName = nameTok.value;
    // 库名允许 dotted（与 import 一致，如 `Library std.p2pkh:`）
    while (check(TOKEN_DOT)) {
        advance();
        consume(TOKEN_IDENTIFIER, "Expected identifier after '.' in library name!");
        fullName += "." + previous().value;
    }
    auto libPtr = std::make_unique<LibraryNode>(
        fullName, nameTok.position.line, nameTok.position.column
    );
    LOG_DEBUG("create ", libPtr->name, " Library");

    consume(TOKEN_COLON, "Expected ':' after library name!");
    while (match(TOKEN_NEWLINE)) {
    }
    consume(TOKEN_INDENT, "Expected 'indent' after library header!");

    while (!(check(TOKEN_DEDENT) || check(TOKEN_EOF))) {
        while (match(TOKEN_NEWLINE)) {
        }
        if (check(TOKEN_DEDENT) || check(TOKEN_EOF))
            break;
        if (check(TOKEN_GLOBAL)) {
            SourceLocation loc(
                "", current().position.line, current().position.column
            );
            const std::string message =
                "global declarations are only allowed at the beginning of a "
                "Contract body, not inside a Library";
            SYNTAX_ERROR(
                message,
                loc,
                "Declare the global in the importing Contract"
            );
            LOG_ERROR(message);
            throw std::runtime_error(message);
        } else if (check(TOKEN_DEF)) {
            // 库内禁止 __init__ 与 main
            if ((TOKEN_IDENTIFIER == peekNext().type &&
                 ("__init__" == peekNext().value ||
                  "main" == peekNext().value)) ||
                TOKEN_INIT == peekNext().type) {
                SourceLocation loc(
                    "", peekNext().position.line, peekNext().position.column
                );
                SYNTAX_ERROR(
                    "library '" + libPtr->name +
                        "' must not define '__init__' or 'main'",
                    loc,
                    "Move constructors and entry points into the user Contract"
                );
                // 跳过该 def 继续收集后续错误
                advance();
                advance();
                continue;
            }
            libPtr->members.push_back(parseFunction());
        } else if (check(TOKEN_STRUCT)) {
            libPtr->members.push_back(parseStruct());
        } else {
            SourceLocation loc(
                "", current().position.line, current().position.column
            );
            std::ostringstream oss;
            oss << "expected 'def' or 'Struct' inside Library '"
                << libPtr->name << "', got '" << current().value << "'";
            SYNTAX_ERROR(
                oss.str(),
                loc,
                "Library blocks may only contain free functions and structs"
            );
            return libPtr;
        }
    }
    match(TOKEN_DEDENT);
    return libPtr;
}

std::unique_ptr<FunctionNode> Parser::parseFunction()
{
    consume(TOKEN_DEF, "Expected 'def'!");

    consume(TOKEN_IDENTIFIER, "Expected function name!");
    auto token = previous();
    auto functionPtr = std::make_unique<FunctionNode>(
        token.value, token.position.line, token.position.column
    );
    LOG_DEBUG("defining ", functionPtr->name, " fun");
    consume(TOKEN_LPAREN, "Expected '(' after function name!");

    functionPtr->parameters = parseParameterList();
    consume(TOKEN_RPAREN, "Expected ')' after contract argument!");

    std::string returnType = "";
    if (match(TOKEN_ARROW)) {
        returnType = parseReturnType();
    }
    functionPtr->returnType = returnType;

    consume(TOKEN_COLON, "Expected ':' after contract definition!");
    functionPtr->block = std::move(parseBlock());
    return functionPtr;
}

std::string Parser::parseTypeName(const std::string& message)
{
    consume(TOKEN_TYPE, message);
    std::string typeName = previous().value;

    if (match(TOKEN_LBRACKET)) {
        if (check(TOKEN_NUMBER)) {
            std::string arraySize = current().value;
            advance();
            consume(
                TOKEN_RBRACKET,
                "Expected ']' after array size in array type"
            );
            typeName += "[" + arraySize + "]";
        } else {
            consume(TOKEN_RBRACKET, "Expected ']' after '[' in array type");
            typeName += "[]";
        }
    }

    return typeName;
}

std::vector<ParameterInfo> Parser::parseParameterList()
{
    std::vector<ParameterInfo> parameters;
    while (!check(TOKEN_RPAREN)) {
        consume(TOKEN_IDENTIFIER, "Expected parameter name");
        std::string paramName = previous().value;

        consume(
            TOKEN_COLON, "Expected ':' after parameter name - type is required"
        );
        std::string paramType = parseTypeName("Expected parameter type after ':'");

        parameters.push_back(ParameterInfo(paramName, paramType));
        LOG_DEBUG("Added parameter: ", paramName, " (", paramType, ")");

        if (!check(TOKEN_RPAREN)) {
            consume(TOKEN_COMMA, "Expected ',' between parameters");
        }
    }
    return parameters;
}

std::vector<CompoundFieldInfo> Parser::parseCompoundFieldList()
{
    consume(TOKEN_LBRACE, "Expected '{' before compound type fields");

    std::vector<CompoundFieldInfo> fields;

    while (!check(TOKEN_RBRACE) && !isAtEnd()) {
        consume(TOKEN_IDENTIFIER, "Expected field name in compound type");
        std::string fieldName = previous().value;

        consume(TOKEN_COLON, "Expected ':' after field name");

        consume(TOKEN_TYPE, "Expected type after ':' in compound type field");
        std::string fieldType = previous().value;

        size_t fieldBytes = 0;
        bool isArray = false;
        size_t arraySize = 0;

        // hex<N>: 提取尾部数字
        if (fieldType.find("hex") == 0 && fieldType.length() > 3) {
            std::string sizeStr = fieldType.substr(3);
            fieldBytes = std::stoul(sizeStr);
        } else if (fieldType == "uint64" && check(TOKEN_LBRACKET)) {
            advance();

            consume(TOKEN_NUMBER, "Expected array size in uint64[N]");
            arraySize = std::stoul(previous().value);
            fieldBytes = arraySize * 8;
            isArray = true;
            fieldType = "uint64[" + std::to_string(arraySize) + "]";

            consume(TOKEN_RBRACKET, "Expected ']' after array size");
        } else {
            SourceLocation loc(
                "", previous().position.line, previous().position.column
            );
            SYNTAX_ERROR(
                "Unsupported compound type field type: " + fieldType,
                loc,
                "Only hex<N> and uint64[N] are supported"
            );
            throw std::runtime_error("Unsupported compound field type");
        }

        fields.emplace_back(
            fieldName, fieldType, fieldBytes, isArray, arraySize
        );

        if (check(TOKEN_COMMA)) {
            advance();
        }
    }

    consume(TOKEN_RBRACE, "Expected '}' after compound type fields");
    return fields;
}

StructFieldType Parser::parseStructFieldType()
{
    // 复合类型：{...}
    if (check(TOKEN_LBRACE)) {
        return StructFieldType("__compound__", parseCompoundFieldList());
    }

    consume(TOKEN_TYPE, "Expect field type");
    std::string baseType = previous().value;

    // 仅为固定大小基础类型设置字节数；结构体数组等留 0 由后续处理
    size_t elementByteSize = 0;
    if (baseType == "uint64") {
        elementByteSize = 8;
    }

    if (match(TOKEN_LBRACKET)) {
        consume(TOKEN_NUMBER, "Expect array size");
        size_t arraySize = std::stoul(previous().value);
        consume(TOKEN_RBRACKET, "Expect ']' after array size");

        return StructFieldType(baseType, arraySize, elementByteSize);
    }

    return StructFieldType(baseType);
}

std::unique_ptr<StructDefNode> Parser::parseStruct()
{
    consume(TOKEN_STRUCT, "Expect 'Struct' before struct definition");

    consume(TOKEN_IDENTIFIER, "Expect struct name");
    auto structNameToken = previous();

    consume(TOKEN_COLON, "Expect ':' after struct name");
    consume(TOKEN_NEWLINE, "Expect newline after ':'");

    consume(TOKEN_INDENT, "Expect indented block for struct fields");

    std::vector<std::pair<std::string, StructFieldType>> fields;
    while (!check(TOKEN_DEDENT) && !isAtEnd()) {
        while (match(TOKEN_NEWLINE)) {
        }

        if (check(TOKEN_DEDENT) || isAtEnd()) {
            break;
        }

        // 强制 "field_name: field_type"
        consume(TOKEN_IDENTIFIER, "Expect field name");
        std::string fieldName = previous().value;

        consume(TOKEN_COLON, "Expect ':' after field name");

        StructFieldType fieldType = parseStructFieldType();

        fields.emplace_back(fieldName, fieldType);

        consume(TOKEN_NEWLINE, "Expect newline after field definition");
    }

    consume(TOKEN_DEDENT, "Expect dedent after struct fields");
    return std::make_unique<StructDefNode>(
        structNameToken.value,
        std::move(fields),
        structNameToken.position.line,
        structNameToken.position.column
    );
}

// NEWLINE INDENT ... DEDENT
std::unique_ptr<BlockNode> Parser::parseBlock()
{
    auto blockPtr = std::make_unique<BlockNode>();
    consume(TOKEN_NEWLINE, "Expected newline before block start!");
    while (match(TOKEN_NEWLINE)) {
    }
    consume(TOKEN_INDENT, "Expected an indent to start a block.");
    while (!check(TOKEN_DEDENT)) {
        while (match(TOKEN_NEWLINE)) {
        }
        if (check(TOKEN_DEDENT))
            break;
        auto stmt = parseStatement();
        if (stmt) {
            blockPtr->statements.push_back(std::move(stmt));
        }
    }
    consume(TOKEN_DEDENT, "Expected a dedent to end block.");
    return blockPtr;
}

std::unique_ptr<StmtNode> Parser::parseStatement()
{
    switch (current().type) {
        case TOKEN_GLOBAL: {
            const Token globalToken = current();
            SourceLocation loc(
                "", globalToken.position.line, globalToken.position.column
            );
            const std::string message =
                "global declarations are only allowed at the beginning of a "
                "Contract body, not inside a function";
            SYNTAX_ERROR(
                message,
                loc,
                "Move this declaration before every Struct and def in the "
                "Contract body"
            );
            LOG_ERROR(message);
            throw std::runtime_error(message);
        }
        case TOKEN_IF:
            advance();
            return parseIf();
        case TOKEN_RETURN: {
            auto returnToken = current();
            advance();
            // 大写 Return：显式返回表达式
            return std::make_unique<ReturnNode>(
                parseExpression(),
                returnToken.position.line,
                returnToken.position.column,
                false
            );
        }
        case TOKEN_RETURN_VALUE: {
            auto returnToken = current();
            advance();
            // 小写 return：表达式仅用于语义/所有权检查，
            // 不产生计算字节码（值已在栈顶）
            return std::make_unique<ReturnNode>(
                parseExpression(),
                returnToken.position.line,
                returnToken.position.column,
                true
            );
        }
        case TOKEN_LBRACE: {
            return parseDestructureAssignment();
        }
        case TOKEN_FOR: {
            Token forToken = current();
            advance();
            return parseForStatement(forToken);
        }
        case TOKEN_IDENTIFIER: {
            size_t startPos = m_pos;  // 用于回溯

            auto identifier = current();
            advance();

            // identifier : type
            if (check(TOKEN_COLON)) {
                advance();

                // identifier : {field1:type1, field2:type2}
                if (check(TOKEN_LBRACE)) {
                    std::vector<CompoundFieldInfo> fields =
                        parseCompoundFieldList();

                    std::unique_ptr<ExprNode> initValue = nullptr;
                    if (match(TOKEN_ASSIGN)) {
                        initValue = parseExpression();
                    }

                    consume(
                        TOKEN_NEWLINE,
                        "Expect newline after compound type declaration"
                    );
                    auto varDeclNode = std::make_unique<VarDeclNode>(
                        identifier.value,
                        "__compound__",
                        identifier.position.line,
                        identifier.position.column,
                        std::move(initValue)
                    );
                    varDeclNode->isCompoundType = true;
                    varDeclNode->compoundFields = std::move(fields);

                    return varDeclNode;
                } else if (check(TOKEN_TYPE)) {
                    std::string typeName = current().value;
                    advance();

                    // 特例：uint64[6] 按变量声明而非数组声明处理
                    if (check(TOKEN_LBRACKET) && typeName == "uint64") {
                        advance();

                        // 仅支持编译期常量数字
                        if (!check(TOKEN_NUMBER)) {
                            SourceLocation loc(
                                "",
                                current().position.line,
                                current().position.column
                            );
                            SYNTAX_ERROR(
                                "Expect array size number after '[' for "
                                "uint64 array-like declaration",
                                loc,
                                "Use syntax like 'amount: uint64[6]'"
                            );
                            throw std::runtime_error(
                                "Invalid uint64[...] declaration"
                            );
                        }

                        Token sizeToken = current();
                        advance();

                        consume(
                            TOKEN_RBRACKET, "Expected ']' after uint64 size"
                        );

                        // 拼出 "uint64[6]" 这样的完整类型名
                        typeName = typeName + "[" + sizeToken.value + "]";

                        std::unique_ptr<ExprNode> initValue = nullptr;
                        if (match(TOKEN_ASSIGN)) {
                            initValue = parseExpression();
                        }

                        consume(
                            TOKEN_NEWLINE,
                            "Expect newline after variable declaration"
                        );

                        return std::make_unique<VarDeclNode>(
                            identifier.value,
                            typeName,
                            identifier.position.line,
                            identifier.position.column,
                            std::move(initValue)
                        );
                    }

                    // identifier : type[size] = [...]（非 uint64[...]）
                    if (check(TOKEN_LBRACKET)) {
                        advance();

                        std::unique_ptr<ExprNode> sizeExpr = nullptr;
                        if (!check(TOKEN_RBRACKET)) {
                            sizeExpr = parseExpression();
                        }

                        consume(
                            TOKEN_RBRACKET, "Expected ']' after array size"
                        );

                        std::unique_ptr<ArrayDefNode> initArray = nullptr;
                        if (match(TOKEN_ASSIGN)) {
                            if (check(TOKEN_LBRACKET)) {
                                initArray = parseArrayLiteral();
                            } else {
                                SourceLocation loc(
                                    "",
                                    current().position.line,
                                    current().position.column
                                );
                                SYNTAX_ERROR(
                                    "Array initialization must use [...] "
                                    "syntax",
                                    loc,
                                    "Use square brackets for array literals"
                                );
                                throw std::runtime_error(
                                    "Invalid array initialization syntax"
                                );
                            }
                        }

                        consume(
                            TOKEN_NEWLINE,
                            "Expect newline after array declaration"
                        );

                        return std::make_unique<ArrayDeclNode>(
                            identifier.value,
                            typeName,
                            identifier.position.line,
                            identifier.position.column,
                            std::move(sizeExpr),
                            std::move(initArray)
                        );
                    } else {
                        std::unique_ptr<ExprNode> initValue = nullptr;
                        if (match(TOKEN_ASSIGN)) {
                            initValue = parseExpression();
                        }

                        consume(
                            TOKEN_NEWLINE,
                            "Expect newline after variable declaration"
                        );

                        return std::make_unique<VarDeclNode>(
                            identifier.value,
                            typeName,
                            identifier.position.line,
                            identifier.position.column,
                            std::move(initValue)
                        );
                    }
                } else {
                    // 冒号后不是类型，回溯
                    m_pos = startPos;
                }
            }

            // a.b.c = v 或 arr[i] = v
            if (check(TOKEN_DOT) || check(TOKEN_LBRACKET)) {
                // 回溯并解析完整链式访问表达式作为左值
                m_pos = startPos;
                auto leftExpr = parseExpression();

                if (check(TOKEN_ASSIGN)) {
                    advance();
                    auto valueExpr = parseExpression();
                    consume(TOKEN_NEWLINE, "Expect newline after assignment");

                    return std::make_unique<AssignNode>(
                        std::move(leftExpr),
                        std::move(valueExpr),
                        identifier.position.line,
                        identifier.position.column
                    );
                } else {
                    consume(TOKEN_NEWLINE, "Expect newline after expression");
                    return std::make_unique<ExprStmtNode>(std::move(leftExpr));
                }
            } else if (check(TOKEN_ASSIGN)) {
                m_pos = startPos;
                return parseAssignment();
            } else {
                m_pos = startPos;
            }

            auto expr = parseExpression();
            consume(TOKEN_NEWLINE, "Expect newline after expression");
            return std::make_unique<ExprStmtNode>(std::move(expr));
        }
        case TOKEN_INDENT: {
            advance();
            auto block = std::make_unique<BlockNode>();
            while (!check(TOKEN_DEDENT) && !isAtEnd()) {
                block->statements.push_back(parseStatement());
            }
            if (isAtEnd()) {
                return block;
            }
            consume(TOKEN_DEDENT, "Expect dedent after block");
            return block;
        }
        case TOKEN_NEWLINE: {
            advance();
            return parseStatement(); // 跳过空行
        }
        default: {
            auto expr = parseExpression();
            consume(TOKEN_NEWLINE, "Expect newline after statement");
            return std::make_unique<ExprStmtNode>(std::move(expr));
        }
    }
}

// if <expr> : <block>
std::unique_ptr<IfNode> Parser::parseIf()
{
    // if 关键字已由调用方 advance()
    auto ifToken = previous();
    auto ifNodePtr = std::make_unique<IfNode>(
        ifToken.position.line, ifToken.position.column
    );
    ifNodePtr->condition = std::move(parseExpression());
    consume(TOKEN_COLON, "Expected ':' after if condition.");
    ifNodePtr->thenBranch = std::move(parseBlock());
    if (match(TOKEN_ELSE)) {
        consume(TOKEN_COLON, "Expected ':' after if condition.");
        ifNodePtr->elseBranch = std::move(parseBlock());
    } else {
        ifNodePtr->elseBranch = std::move(nullptr);
    }

    return ifNodePtr;
}

std::unique_ptr<ForNode> Parser::parseForStatement(const Token& forToken)
{
    consume(TOKEN_IDENTIFIER, "Expected loop target identifier after 'for'");
    Token targetToken = previous();

    if (check(TOKEN_COMMA)) {
        SourceLocation loc(
            "", targetToken.position.line, targetToken.position.column
        );
        SYNTAX_ERROR(
            "Multiple loop targets are not supported yet",
            loc,
            "Use a single identifier on the left side of the for clause"
        );
        throw std::runtime_error("Unsupported destructuring in for loop");
    }

    consume(TOKEN_IN, "Expected 'in' in for statement");

    auto iterableExpr = parseExpression();

    consume(TOKEN_COLON, "Expected ':' after for clause");
    auto bodyBlock = parseBlock();

    auto forNode = std::make_unique<ForNode>(
        forToken.position.line, forToken.position.column
    );
    forNode->target = targetToken.value;
    forNode
        ->targetPos = {targetToken.position.line, targetToken.position.column};
    forNode->iterable = std::move(iterableExpr);
    forNode->body = std::move(bodyBlock);

    return forNode;
}

std::unique_ptr<AssignNode> Parser::parseAssignment()
{
    Token name = current();
    advance();
    consume(TOKEN_ASSIGN, "Expect '=' after variable name");
    auto value = parseExpression();
    consume(TOKEN_NEWLINE, "Expect newline after assignment");
    return std::make_unique<AssignNode>(
        std::make_unique<IdentifierNode>(
            name.value, name.position.line, name.position.column
        ),
        std::move(value)
    );
}

// 简化为左结合二元运算
std::unique_ptr<ExprNode> Parser::parseExpression()
{
    return parseBinaryExpression(0);
}

std::vector<std::unique_ptr<ExprNode>>
Parser::parseArgumentList(const std::string& closingMessage)
{
    std::vector<std::unique_ptr<ExprNode>> args;
    if (!check(TOKEN_RPAREN)) {
        do {
            args.push_back(parseExpression());
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RPAREN, closingMessage);
    return args;
}

// 数字 / 标识符 / 函数调用 / 括号表达式
std::unique_ptr<ExprNode> Parser::parsePrimary()
{
    if (match(TOKEN_NUMBER)) {
        auto token = previous();
        return std::make_unique<LiteralNode>(
            LiteralNode::Type::Number,
            token.value,
            token.position.line,
            token.position.column
        );
    }
    if (match(TOKEN_HEX)) {
        auto token = previous();
        return std::make_unique<LiteralNode>(
            LiteralNode::Type::Hex,
            token.value,
            token.position.line,
            token.position.column
        );
    }
    if (match(TOKEN_ADDRESS)) {
        auto token = previous();
        return std::make_unique<LiteralNode>(
            LiteralNode::Type::Addr,
            token.value,
            token.position.line,
            token.position.column
        );
    }
    if (match(TOKEN_STRING)) {
        auto token = previous();
        return std::make_unique<LiteralNode>(
            LiteralNode::Type::String,
            token.value,
            token.position.line,
            token.position.column
        );
    }
    if (match(TOKEN_IDENTIFIER)) {
        const auto name = previous();
        std::unique_ptr<ExprNode> expr;
        if (match(TOKEN_LPAREN)) {
            auto args = parseArgumentList("Expected ')' after function arguments.");
            auto callExpr = std::make_unique<CallNode>(
                name.value,
                std::move(args),
                name.position.line,
                name.position.column
            );
            callExpr->isRangeCall = (name.value == "Range");
            expr = std::move(callExpr);
        } else {
            expr = std::make_unique<IdentifierNode>(
                name.value, name.position.line, name.position.column
            );
        }

        return parsePostfixExpression(std::move(expr));
    }
    if (match(TOKEN_LPAREN)) {
        auto expr = parseExpression();
        consume(TOKEN_RPAREN, "Expected ')' after expression.");
        return parsePostfixExpression(std::move(expr));
    }

    if (check(TOKEN_LBRACKET)) {
        return parsePostfixExpression(parseArrayLiteral());
    }

    if (check(TOKEN_LBRACE)) {
        return parsePostfixExpression(parseBraceExpression());
    }

    SourceLocation loc(
        "", m_tokens[m_pos].position.line, m_tokens[m_pos].position.column
    );
    std::ostringstream oss;
    oss << "unexpected token '" << current().value << "' in expression";
    LOG_ERROR(
        "Syntax error at line ",
        loc.line,
        ", column ",
        loc.column,
        " - ",
        oss.str()
    );
    SYNTAX_ERROR(oss.str(), loc, "Check the expression syntax");
    return std::make_unique<LiteralNode>(
        LiteralNode::Type::Number,
        "0",
        m_tokens[m_pos].position.line,
        m_tokens[m_pos].position.column
    ); // 返回默认值让编译继续
}

std::unique_ptr<ExprNode>
Parser::parsePostfixExpression(std::unique_ptr<ExprNode> expr)
{
    while (true) {
        if (match(TOKEN_DOT)) {
            consume(TOKEN_IDENTIFIER, "Expect property name after '.'");
            const auto fieldToken = previous();

            if (match(TOKEN_LPAREN)) {
                auto methodArgs =
                    parseArgumentList("Expected ')' after method arguments.");
                auto methodCallNode = std::make_unique<MethodCallNode>(
                    std::move(expr),
                    fieldToken.value,
                    std::move(methodArgs),
                    fieldToken.position.line,
                    fieldToken.position.column
                );
                methodCallNode->object->setParent(methodCallNode.get());
                expr = std::move(methodCallNode);
            } else {
                expr = std::make_unique<FieldAccessNode>(
                    std::move(expr),
                    fieldToken.value,
                    fieldToken.position.line,
                    fieldToken.position.column
                );
            }
        } else if (match(TOKEN_LBRACKET)) {
            const auto basePos = expr ? expr->pos : std::pair<int32_t, int32_t>{0, 0};
            auto indexExpr = parseExpression();
            consume(TOKEN_RBRACKET, "Expected ']' after array index");

            expr = std::make_unique<IndexAccessNode>(
                std::move(expr),
                std::move(indexExpr),
                basePos.first,
                basePos.second
            );
        } else {
            break;
        }
    }

    return expr;
}

std::unique_ptr<ConstructorNode> Parser::parseConstructor()
{
    consume(TOKEN_DEF, "Expected 'def'!");
    consume(TOKEN_IDENTIFIER, "Expected '__init__'!");
    const Token constructorToken = previous();

    if (constructorToken.value != "__init__") {
        SourceLocation loc(
            "",
            constructorToken.position.line,
            constructorToken.position.column
        );
        LOG_ERROR(
            "Semantic error at line ",
            loc.line,
            ", column ",
            loc.column,
            " - constructor must be named '__init__'"
        );
        SEMANTIC_ERROR(
            "constructor must be named '__init__'",
            loc,
            "Rename the constructor to '__init__'"
        );
        // 沿用错误名称继续解析
    }

    consume(TOKEN_LPAREN, "Expected '(' after constructor name!");

    std::vector<ParameterInfo> params = parseParameterList();

    consume(TOKEN_RPAREN, "Expected ')' after parameters!");
    consume(TOKEN_COLON, "Expected ':' after constructor definition!");

    auto body = parseBlock();
    return std::make_unique<ConstructorNode>(
        constructorToken.position.line,
        constructorToken.position.column,
        params,
        std::move(body)
    );
}

std::unique_ptr<ExprNode> Parser::parseBinaryExpression(int minPrecedence)
{
    auto left = parseUnaryExpression();

    while (true) {
        Token opToken = current();
        auto optPrecedence = getOperatorPrecedence(opToken.type);

        if (!optPrecedence || *optPrecedence < minPrecedence) {
            break;
        }

        advance();
        int nextMinPrecedence = *optPrecedence + 1; // 右结合用 *optPrecedence

        auto right = parseBinaryExpression(nextMinPrecedence);
        left = std::make_unique<OpNode>(
            opToken.value,
            std::move(left),
            std::move(right),
            opToken.position.line,
            opToken.position.column
        );
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseUnaryExpression()
{
    // TODO: 支持 ! 一元运算符（|| match(TOKEN_NOT)）
    if (match(TOKEN_MINUS)) {
        Token op = previous();
        auto operand = parseUnaryExpression();
        return std::make_unique<OpNode>(
            op.value,
            nullptr,
            std::move(operand),
            op.position.line,
            op.position.column
        );
    }
    return parsePrimary();
}

std::optional<int> Parser::getOperatorPrecedence(TokenType type)
{
    static const std::unordered_map<TokenType, int> precedences = {
        {TOKEN_EQUAL, 1},
        {TOKEN_NOTEQUAL, 1},
        {TOKEN_LESS, 2},
        {TOKEN_GREATER, 2},
        {TOKEN_LESSEQUAL, 2},
        {TOKEN_GREATEREQUAL, 2},
        {TOKEN_PLUS, 3},
        {TOKEN_MINUS, 3},
        {TOKEN_STAR, 4},
        {TOKEN_SLASH, 4},
    };

    auto it = precedences.find(type);
    return it != precedences.end() ? std::optional(it->second) : std::nullopt;
}

// [e1, e2, e3]
std::unique_ptr<ArrayDefNode> Parser::parseArrayLiteral()
{
    consume(TOKEN_LBRACKET, "Expected '[' at start of array literal");

    std::vector<std::unique_ptr<ExprNode>> elements;

    // 空数组：元素类型由后续语义分析推断
    if (check(TOKEN_RBRACKET)) {
        advance();
        return std::make_unique<ArrayDefNode>("", std::move(elements));
    }

    do {
        elements.push_back(parseExpression());

        if (check(TOKEN_COMMA)) {
            advance();
        } else {
            break;
        }
    } while (!check(TOKEN_RBRACKET) && !isAtEnd());

    consume(TOKEN_RBRACKET, "Expected ']' at end of array literal");

    return std::make_unique<ArrayDefNode>("", std::move(elements));
}

// {e1, e2, ...}
std::unique_ptr<BraceExprNode> Parser::parseBraceExpression()
{
    auto startToken = current();
    consume(TOKEN_LBRACE, "Expected '{'");

    std::vector<std::unique_ptr<ExprNode>> elements;

    if (!check(TOKEN_RBRACE)) {
        do {
            elements.push_back(parseExpression());
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RBRACE, "Expected '}' after brace expression elements");

    return std::make_unique<BraceExprNode>(
        std::move(elements),
        startToken.position.line,
        startToken.position.column
    );
}

// {a, b, c} = expression
std::unique_ptr<DestructureAssignNode> Parser::parseDestructureAssignment()
{
    auto startToken = current();
    consume(TOKEN_LBRACE, "Expected '{'");

    std::vector<std::string> targets;

    if (!check(TOKEN_RBRACE)) {
        do {
            consume(
                TOKEN_IDENTIFIER,
                "Expected identifier in destructure assignment"
            );
            targets.push_back(previous().value);
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RBRACE, "Expected '}' after target list");
    consume(TOKEN_ASSIGN, "Expected '=' in destructure assignment");

    auto value = parseExpression();

    return std::make_unique<DestructureAssignNode>(
        std::move(targets),
        std::move(value),
        startToken.position.line,
        startToken.position.column
    );
}

// 单一类型或 {type1, type2, ...}
std::string Parser::parseReturnType()
{
    if (check(TOKEN_LBRACE)) {
        return parseBraceTypeList();
    }

    return parseTypeName("Expected return type");
}

// {type1, type2, ...}
std::string Parser::parseBraceTypeList()
{
    consume(TOKEN_LBRACE, "Expected '{'");

    std::string typeList = "{";
    bool first = true;

    if (!check(TOKEN_RBRACE)) {
        do {
            if (!first) {
                typeList += ", ";
            }
            first = false;

            typeList += parseTypeName("Expected type in brace type list");
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RBRACE, "Expected '}' after type list");
    typeList += "}";

    return typeList;
}
