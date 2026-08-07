#ifndef AST_VISITOR_H
#define AST_VISITOR_H

#include "ast.h"

class ASTVisitor
{
public:
    virtual ~ASTVisitor() = default;
    virtual void visit(ContractNode& node) = 0;
    virtual void visit(GlobalConstNode& /*node*/)
    {}
    virtual void visit(LibraryNode& /*node*/)
    {}
    virtual void visit(FunctionNode& node) = 0;
    virtual void visit(ConstructorNode& node) = 0;
    virtual void visit(StructDefNode& node) = 0;
    virtual void visit(BlockNode& node) = 0;
    virtual void visit(IfNode& node) = 0;
    virtual void visit(ForNode& node) = 0;
    virtual void visit(AssignNode& node) = 0;
    virtual void visit(ExprStmtNode& node) = 0;
    virtual void visit(ReturnNode& node) = 0;
    virtual void visit(VarDeclNode& node) = 0;
    virtual void visit(ArrayDeclNode& node) = 0;
    virtual void visit(ArrayDefNode& node) = 0;
    virtual void visit(LiteralNode& node) = 0;
    virtual void visit(IdentifierNode& node) = 0;
    virtual void visit(CallNode& node) = 0;
    virtual void visit(MethodCallNode& node) = 0;
    virtual void visit(OpNode& node) = 0;
    virtual void visit(FieldAccessNode& node) = 0;
    virtual void visit(IndexAccessNode& node) = 0;
    virtual void visit(BraceExprNode& node) = 0;
    virtual void visit(DestructureAssignNode& node) = 0;
};

#endif // AST_VISITOR_H
