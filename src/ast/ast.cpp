//
// Created by Wayne on 25-2-11.
//

#include "ast.h"

#include <utility>

#include "ast_visitor.h"

void ContractNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void LibraryNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void FunctionNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void StructDefNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void BlockNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void IfNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void ForNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void AssignNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void ExprStmtNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void ReturnNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void LiteralNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void IdentifierNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void CallNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void MethodCallNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void OpNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void FieldAccessNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void IndexAccessNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void ConstructorNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void VarDeclNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void ArrayDeclNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void ArrayDefNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void BraceExprNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}

void DestructureAssignNode::accept(ASTVisitor& visitor)
{
    visitor.visit(*this);
}
