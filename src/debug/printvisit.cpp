#include "printvisit.h"

void PrintVisitor::visit(ContractNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 1, "ContractNode:", node.name);
    indentLevel++;
    for (auto& member : node.members) {
        member->accept(*this);
    }
    indentLevel--;
}

void PrintVisitor::visit(FunctionNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 0, "FunctionNode:", node.name, "(");
    for (size_t i = 0; i < node.parameters.size(); ++i) {
        Logger::GetInstance().Log(
            LogLevel::DEBUG,
            "",
            0,
            1,
            0,
            node.parameters[i].name,
            ":",
            node.parameters[i].type
        );
        // node.parameters[i].second;
        if (i + 1 < node.parameters.size()) {
            Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, ",", " ");
        }
    }
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, ")");

    if (node.block) {
        indentLevel++;
        node.block->accept(*this);
        indentLevel--;
    }
}

void PrintVisitor::visit(StructDefNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 1, "StructDefNode: ", node.name);
    indentLevel++;
    for (auto& field : node.fields) {
        printIndent();
        Logger::GetInstance().Log(
            LogLevel::DEBUG,
            "",
            0,
            1,
            1,
            "Field: ",
            field.first,
            " : ",
            field.second.getTypeString()
        );
    }
    indentLevel--;
}

void PrintVisitor::visit(BlockNode& node)
{
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "BlockNode {");
    indentLevel++;
    for (auto& stmt : node.statements) {
        stmt->accept(*this);
    }
    indentLevel--;
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "}");
}

void PrintVisitor::visit(IfNode& node)
{
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "IfNode:");
    indentLevel++;
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "Condition:");
    indentLevel++;
    node.condition->accept(*this);
    indentLevel--;

    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "ThenBranch:");
    indentLevel++;
    node.thenBranch->accept(*this);
    indentLevel--;

    if (node.elseBranch) {
        printIndent();
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "ElseBranch:");
        indentLevel++;
        node.elseBranch->accept(*this);
        indentLevel--;
    }
    indentLevel--;
}

void PrintVisitor::visit(ForNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 1, "ForNode: target=", node.target);

    indentLevel++;
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "Iterable:");
    indentLevel++;
    if (node.iterable) {
        node.iterable->accept(*this);
    }
    indentLevel--;

    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "Body:");
    indentLevel++;
    if (node.body) {
        node.body->accept(*this);
    }
    indentLevel -= 2;
}

void PrintVisitor::visit(AssignNode& node)
{
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, "AssignNode: ");
    node.name->accept(*this);
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, " = ");
    indentLevel++;
    node.value->accept(*this);
    indentLevel--;
}

void PrintVisitor::visit(ExprStmtNode& node)
{
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "ExprStmtNode:");
    indentLevel++;
    node.expr->accept(*this);
    indentLevel--;
}

void PrintVisitor::visit(ReturnNode& node)
{
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "ReturnNode:");
    if (node.expr) {
        indentLevel++;
        node.expr->accept(*this);
        indentLevel--;
    } else {
        printIndent();
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "(null)");
    }
}

void PrintVisitor::visit(LiteralNode& node)
{
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, "LiteralNode: ");
    switch (node.type) {
        case LiteralNode::Type::Number:
            Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, "Number");
            break;
        case LiteralNode::Type::String:
            Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, "String");
            break;
        case LiteralNode::Type::Boolean:
            Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, "Boolean");
            break;
        case LiteralNode::Type::Hex:
            Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, "Hex");
            break;
        case LiteralNode::Type::FixedArray:
        case LiteralNode::Type::PubKey:
        case LiteralNode::Type::Sig:
        case LiteralNode::Type::Ripemd160:
        case LiteralNode::Type::PubKeyHash:
        case LiteralNode::Type::Sha1:
        case LiteralNode::Type::Sha256:
        case LiteralNode::Type::SigHashType:
        case LiteralNode::Type::SigHashPreimage:
        case LiteralNode::Type::OpCodeType:
        case LiteralNode::Type::Addr:
        case LiteralNode::Type::PrivKey:
        default:
            Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, "Other");
            break;
    }
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, " = ", node.value);
}

void PrintVisitor::visit(IdentifierNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 1, "IdentifierNode: ", node.name);
}

void PrintVisitor::visit(CallNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 1, "CallNode: ", node.funcName, "(");
    indentLevel++;
    for (auto& arg : node.args) {
        arg->accept(*this);
    }
    indentLevel--;
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, ")");
}

void PrintVisitor::visit(MethodCallNode& node)
{
    printIndent();
    Logger::GetInstance().Log(
        LogLevel::DEBUG, "", 0, 1, 1, "MethodCallNode: ", node.methodName, "("
    );

    indentLevel++;

    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "Object:");
    indentLevel++;
    node.object->accept(*this);
    indentLevel--;

    if (!node.args.empty()) {
        printIndent();
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "Arguments:");
        indentLevel++;
        for (auto& arg : node.args) {
            arg->accept(*this);
        }
        indentLevel--;
    }

    indentLevel--;
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, ")");
}

void PrintVisitor::visit(OpNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 1, "OpNode: ", node.op);
    indentLevel++;
    if (node.lhs) {
        printIndent();
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "LHS:");
        indentLevel++;
        node.lhs->accept(*this);
        indentLevel--;
    }
    if (node.rhs) {
        printIndent();
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "RHS:");
        indentLevel++;
        node.rhs->accept(*this);
        indentLevel--;
    }
    indentLevel--;
}

void PrintVisitor::visit(FieldAccessNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 1, "FieldAccessNode: ", node.field);
    indentLevel++;
    node.base->accept(*this);
    indentLevel--;
}

void PrintVisitor::visit(IndexAccessNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 1, "IndexAccessNode: ");
    indentLevel++;
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "Base: ");
    indentLevel++;
    node.base->accept(*this);
    indentLevel--;
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "Index: ");
    indentLevel++;
    node.index->accept(*this);
    indentLevel -= 2;
}

void PrintVisitor::visit(ConstructorNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 0, "ConstructorNode:", node.name, "(");
    for (size_t i = 0; i < node.parameters.size(); ++i) {
        Logger::GetInstance().Log(
            LogLevel::DEBUG,
            "",
            0,
            1,
            0,
            node.parameters[i].name,
            ":",
            node.parameters[i].type
        );
        // node.parameters[i].second;
        if (i + 1 < node.parameters.size()) {
            Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, ",", " ");
        }
    }
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, ")");

    if (node.block) {
        indentLevel++;
        node.block->accept(*this);
        indentLevel--;
    }
}

void PrintVisitor::printIndent()
{
    for (int i = 0; i < indentLevel; ++i) {
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, " ");
    }
}

void PrintVisitor::visit(VarDeclNode& node)
{
    printIndent();
    Logger::GetInstance().Log(
        LogLevel::DEBUG, "", 0, 1, 0, "VarDeclNode: ", node.name, ":", node.type
    );
    if (node.initValue) {
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, " = ");
        indentLevel++;
        node.initValue->accept(*this);
        indentLevel--;
    } else {
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "");
    }
}

void PrintVisitor::visit(ArrayDeclNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 0, "ArrayDeclNode: ", node.name);
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 1, " : ", node.elementType, "[]");

    if (node.sizeExpr) {
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, " [size=");
        indentLevel++;
        node.sizeExpr->accept(*this);
        indentLevel--;
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "]");
    }

    if (node.initArray) {
        Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, " = ");
        indentLevel++;
        node.initArray->accept(*this);
        indentLevel--;
    }
}

void PrintVisitor::visit(ArrayDefNode& node)
{
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, "ArrayDefNode: [");

    for (size_t i = 0; i < node.elements.size(); ++i) {
        if (i > 0) {
            Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, ", ");
        }
        indentLevel++;
        node.elements[i]->accept(*this);
        indentLevel--;
    }

    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "]");
}

void PrintVisitor::visit(BraceExprNode& node)
{
    printIndent();
    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 0, "BraceExprNode: {");

    for (size_t i = 0; i < node.elements.size(); ++i) {
        if (i > 0) {
            Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, ", ");
        }
        indentLevel++;
        node.elements[i]->accept(*this);
        indentLevel--;
    }

    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "}");
}

void PrintVisitor::visit(DestructureAssignNode& node)
{
    printIndent();
    Logger::GetInstance()
        .Log(LogLevel::DEBUG, "", 0, 1, 0, "DestructureAssignNode: {");

    for (size_t i = 0; i < node.targets.size(); ++i) {
        if (i > 0) {
            Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, ", ");
        }
        Logger::GetInstance()
            .Log(LogLevel::DEBUG, "", 0, 1, 0, node.targets[i]);
    }

    Logger::GetInstance().Log(LogLevel::DEBUG, "", 0, 1, 1, "} = ");

    indentLevel++;
    node.value->accept(*this);
    indentLevel--;
}
