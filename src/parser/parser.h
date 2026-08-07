//
// Created by Wayne on 25-2-11.
//

#ifndef PARSER_H
#define PARSER_H

#include <optional>
#include <string>
#include <vector>

#include "../ast/ast.h"
#include "../include/token_type.h"

class Parser
{
public:
    explicit Parser(const std::vector<Token>& tokens)
        : m_pos(0), m_tokens(tokens)
    {}
    // 解析 (Library)* Contract EOF；库块挂在 ContractNode::libraries 由后续 Pass 合并
    std::shared_ptr<ContractNode> parseContract();
    std::unique_ptr<LibraryNode> parseLibrary();

private:
    Token previous();
    Token current();
    Token peekNext();
    Token advance();
    bool isAtEnd();
    bool match(TokenType type);
    bool check(TokenType type, int add = 0);
    void consume(TokenType type, const std::string& message);

    std::unique_ptr<FunctionNode> parseFunction();
    std::unique_ptr<GlobalConstNode> parseGlobalConst();
    std::unique_ptr<StructDefNode> parseStruct();
    std::unique_ptr<BlockNode> parseBlock();
    std::unique_ptr<StmtNode> parseStatement();
    std::unique_ptr<IfNode> parseIf();
    std::unique_ptr<ForNode> parseForStatement(const Token& forToken);
    std::unique_ptr<AssignNode> parseAssignment();
    std::unique_ptr<ExprNode> parseExpression();
    std::unique_ptr<ExprNode> parsePrimary();
    std::unique_ptr<ExprNode> parsePostfixExpression(std::unique_ptr<ExprNode> expr);
    std::unique_ptr<ConstructorNode> parseConstructor();
    std::unique_ptr<ExprNode> parseUnaryExpression();
    std::unique_ptr<ExprNode> parseBinaryExpression(int minPrecedence);
    std::vector<std::unique_ptr<ExprNode>>
    parseArgumentList(const std::string& closingMessage);

    std::unique_ptr<ArrayDefNode> parseArrayLiteral();

    std::unique_ptr<BraceExprNode> parseBraceExpression();
    std::unique_ptr<DestructureAssignNode> parseDestructureAssignment();

    StructFieldType parseStructFieldType();
    std::vector<CompoundFieldInfo> parseCompoundFieldList();

    std::string parseReturnType();
    std::string parseBraceTypeList();
    std::string parseTypeName(const std::string& message);
    std::vector<ParameterInfo> parseParameterList();

    std::optional<int> getOperatorPrecedence(TokenType type);
private:
    size_t m_pos;
    const std::vector<Token>& m_tokens;
};

#endif // PARSER_H
