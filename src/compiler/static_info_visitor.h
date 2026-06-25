#ifndef STATIC_INFO_VISITOR_H
#define STATIC_INFO_VISITOR_H

#include <nlohmann/json.hpp>

#include "../ast/ast_visitor.h"
#include "../bytecode/bytecode_opcodes.h"
#include "../bytecode/scope.h"
#include "../log/logger.h"

// 收集 ABI / 构造函数 / 结构体 / 全函数信息.
class StaticInfoVisitor : public ASTVisitor
{
public:
    StaticInfoVisitor() : m_functionIndex(0)
    {
        m_abiJson = nlohmann::ordered_json::array();
        m_constructorParamsJson = nlohmann::ordered_json::array();
        m_structJson = nlohmann::ordered_json::array();
        m_allFunctionsJson = nlohmann::ordered_json::array();
    }

    nlohmann::ordered_json getConstructorParamsJson() const
    {
        return m_constructorParamsJson;
    };
    nlohmann::ordered_json getAbiJson() const
    {
        return m_abiJson;
    };
    nlohmann::ordered_json getStructJson() const
    {
        return m_structJson;
    };
    nlohmann::ordered_json getAllFunctionsJson() const
    {
        return m_allFunctionsJson;
    };

    void visit(ContractNode& node) override;

    void visit(FunctionNode& node) override;

    void visit(ConstructorNode& node) override;

    void visit(StructDefNode& node) override;

    void visit(BlockNode& /*node*/) override {};
    void visit(IfNode& /*node*/) override {};
    void visit(ForNode& /*node*/) override {};
    void visit(AssignNode& /*node*/) override {};
    void visit(ExprStmtNode& /*node*/) override {};
    void visit(ReturnNode& /*node*/) override {};
    void visit(VarDeclNode& /*node*/) override {};
    void visit(LiteralNode& /*node*/) override {};
    void visit(IdentifierNode& /*node*/) override {};
    void visit(CallNode& /*node*/) override {};
    void visit(MethodCallNode& /*node*/) override {};
    void visit(OpNode& /*node*/) override {};
    void visit(FieldAccessNode& /*node*/) override {};
    void visit(IndexAccessNode& /*node*/) override {};
    void visit(ArrayDeclNode& /*node*/) override {};
    void visit(ArrayDefNode& /*node*/) override {};
    void visit(BraceExprNode& /*node*/) override {};
    void visit(DestructureAssignNode& /*node*/) override {};

private:
    void generateAllFunctionInfo(FunctionNode& node, bool isPrivate);
    void generateFunction(FunctionNode& node);
    void generateConstructor(ConstructorNode& node);
    void generateStruct(StructDefNode& node);

private:
    nlohmann::ordered_json m_constructorParamsJson;
    nlohmann::ordered_json m_abiJson;
    nlohmann::ordered_json m_structJson;
    nlohmann::ordered_json m_allFunctionsJson;
    int m_functionIndex;
};

#endif // STATIC_INFO_VISITOR_H
