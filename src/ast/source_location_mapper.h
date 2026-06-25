#ifndef SOURCE_LOCATION_MAPPER_H
#define SOURCE_LOCATION_MAPPER_H

#include "ast.h"
#include "ast_visitor.h"
#include "../source/source_map.h"

namespace ast_source
{

inline SourceLocation mapLocation(const ASTNode& node, const SourceMap& sourceMap)
{
    const SourceLineMapping* mapping = sourceMap.lookup(node.pos.first);
    if (mapping && mapping->isValid()) {
        return SourceLocation(mapping->filename, mapping->line, node.pos.second);
    }

    return SourceLocation("", node.pos.first, node.pos.second);
}

class SourceMapApplyingVisitor final : public ASTVisitor
{
public:
    explicit SourceMapApplyingVisitor(const SourceMap& sourceMap)
        : m_sourceMap(sourceMap)
    {}

    void apply(ASTNode* node)
    {
        if (!node) {
            return;
        }

        node->setSourceLocation(mapLocation(*node, m_sourceMap));
        node->accept(*this);
    }

    void visit(ContractNode& node) override
    {
        for (auto& library : node.libraries) {
            apply(library.get());
        }
        applyAll(node.members);
    }

    void visit(LibraryNode& node) override
    {
        applyAll(node.members);
    }

    void visit(FunctionNode& node) override
    {
        apply(node.block.get());
    }

    void visit(ConstructorNode& node) override
    {
        apply(node.block.get());
    }

    void visit(StructDefNode& /*node*/) override
    {}

    void visit(BlockNode& node) override
    {
        applyAll(node.statements);
    }

    void visit(IfNode& node) override
    {
        apply(node.condition.get());
        apply(node.thenBranch.get());
        apply(node.elseBranch.get());
    }

    void visit(ForNode& node) override
    {
        apply(node.iterable.get());
        apply(node.body.get());
    }

    void visit(AssignNode& node) override
    {
        apply(node.name.get());
        apply(node.value.get());
    }

    void visit(ExprStmtNode& node) override
    {
        apply(node.expr.get());
    }

    void visit(ReturnNode& node) override
    {
        apply(node.expr.get());
    }

    void visit(VarDeclNode& node) override
    {
        apply(node.initValue.get());
    }

    void visit(ArrayDeclNode& node) override
    {
        apply(node.sizeExpr.get());
        apply(node.initArray.get());
    }

    void visit(ArrayDefNode& node) override
    {
        applyAll(node.elements);
    }

    void visit(LiteralNode& /*node*/) override
    {}

    void visit(IdentifierNode& /*node*/) override
    {}

    void visit(CallNode& node) override
    {
        applyAll(node.args);
    }

    void visit(MethodCallNode& node) override
    {
        apply(node.object.get());
        applyAll(node.args);
    }

    void visit(OpNode& node) override
    {
        apply(node.lhs.get());
        apply(node.rhs.get());
    }

    void visit(FieldAccessNode& node) override
    {
        apply(node.base.get());
    }

    void visit(IndexAccessNode& node) override
    {
        apply(node.base.get());
        apply(node.index.get());
    }

    void visit(BraceExprNode& node) override
    {
        applyAll(node.elements);
    }

    void visit(DestructureAssignNode& node) override
    {
        apply(node.value.get());
    }

private:
    template <typename T>
    void applyAll(const std::vector<std::unique_ptr<T>>& nodes)
    {
        for (const auto& child : nodes) {
            apply(child.get());
        }
    }

    const SourceMap& m_sourceMap;
};

inline void applySourceMap(ASTNode* node, const SourceMap& sourceMap)
{
    SourceMapApplyingVisitor visitor(sourceMap);
    visitor.apply(node);
}

} // namespace ast_source

#endif // SOURCE_LOCATION_MAPPER_H
