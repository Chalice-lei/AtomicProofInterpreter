#ifndef PRINT_VISIT_H
#define PRINT_VISIT_H

#include <iostream>
#include <sstream>
#include <string>

#include "../ast/ast_visitor.h"
#include "../log/logger.h"

class PrintVisitor : public ASTVisitor
{
public:
    PrintVisitor() : indentLevel(0)
    {}

    void visit(ContractNode& node) override;

    void visit(GlobalConstNode& node) override;

    void visit(FunctionNode& node) override;

    void visit(StructDefNode& node) override;

    void visit(BlockNode& node) override;

    void visit(IfNode& node) override;

    void visit(ForNode& node) override;

    void visit(AssignNode& node) override;

    void visit(ExprStmtNode& node) override;

    void visit(ReturnNode& node) override;

    void visit(VarDeclNode& node) override;

    void visit(LiteralNode& node) override;

    void visit(IdentifierNode& node) override;

    void visit(CallNode& node) override;

    void visit(MethodCallNode& node) override;

    void visit(OpNode& node) override;

    void visit(FieldAccessNode& node) override;

    void visit(IndexAccessNode& node) override;

    void visit(ArrayDeclNode& node) override;

    void visit(ArrayDefNode& node) override;

    void visit(BraceExprNode& node) override;

    void visit(DestructureAssignNode& node) override;

    void visit(ConstructorNode& node) override;

private:
    int indentLevel;

    void printIndent();
};

[[maybe_unused]] static void debugPrintAst(
    const std::shared_ptr<ContractNode> root
)
{
    PrintVisitor printer;
    root->accept(printer);
}

#endif // PRINT_VISIT_H
