#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "ast/ast.h"
#include "bytecode/bytecode_generator.h"
#include "compiler/ast_to_bytecode_visitor.h"

namespace
{
[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "if lowering state regression failed: " << message << '\n';
    std::exit(1);
}
} // namespace

int main()
{
    tbc::BytecodeGenerator generator;
    ASTToBytecodeVisitor visitor(generator);

    auto divisionByZero = std::make_unique<OpNode>(
        "/",
        std::make_unique<LiteralNode>(
            LiteralNode::Type::Number, "1", 4, 12
        ),
        std::make_unique<LiteralNode>(
            LiteralNode::Type::Number, "0", 4, 16
        ),
        4,
        14
    );
    auto comparison = std::make_unique<OpNode>(
        "!=",
        std::move(divisionByZero),
        std::make_unique<LiteralNode>(
            LiteralNode::Type::Number, "1", 4, 21
        ),
        4,
        18
    );
    OpNode* comparisonPtr = comparison.get();

    IfNode branch(4, 5);
    branch.condition = std::move(comparison);
    branch.thenBranch = std::make_unique<BlockNode>(4, 24);

    bool threw = false;
    try {
        branch.accept(visitor);
    } catch (const std::exception&) {
        threw = true;
    }

    if (!threw) {
        fail("invalid condition did not abort lowering");
    }
    if (comparisonPtr->op != "!=") {
        fail("temporary OP_NOTIF rewrite leaked into the shared AST");
    }

    return 0;
}
