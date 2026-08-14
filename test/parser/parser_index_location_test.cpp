#include "ast/ast.h"
#include "error/error_manager.h"
#include "lexer/lexer.h"
#include "parser/parser.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Node>
Node& requireNode(ASTNode* node, const std::string& label)
{
    auto* typed = dynamic_cast<Node*>(node);
    require(typed != nullptr, label + ": unexpected AST node type");
    return *typed;
}

void requirePosition(
    const ASTNode& node,
    int32_t expectedLine,
    int32_t expectedColumn,
    const std::string& label
)
{
    require(
        node.pos.first == expectedLine && node.pos.second == expectedColumn,
        label + ": expected position " + std::to_string(expectedLine) + ":" +
            std::to_string(expectedColumn) + ", got " +
            std::to_string(node.pos.first) + ":" +
            std::to_string(node.pos.second)
    );
}

void testIndexLocationsUseLeftBracket()
{
    const std::string source =
        "Contract IndexLocations:\n"
        "    def main():\n"
        "        values[index]\n"
        "        root.items[first][second]\n"
        "        (root).items[first]\n";

    auto& errors = ErrorManager::getInstance();
    errors.clear();

    Lexer lexer(source, "parser_index_location_test.ct");
    const auto tokens = lexer.tokenize();
    require(!errors.hasErrors(), "lexer reported an unexpected error");

    Parser parser(tokens);
    const auto contract = parser.parseContract();
    require(contract != nullptr, "parser returned a null contract");
    require(!errors.hasErrors(), "parser reported an unexpected error");
    require(contract->members.size() == 1, "expected one contract member");

    auto& function =
        requireNode<FunctionNode>(contract->members.front().get(), "main");
    require(function.block != nullptr, "main has no block");
    require(
        function.block->statements.size() == 3,
        "expected three expression statements"
    );

    auto expressionAt = [&](size_t index) -> ExprNode* {
        auto& statement = requireNode<ExprStmtNode>(
            function.block->statements.at(index).get(),
            "statement " + std::to_string(index)
        );
        require(statement.expr != nullptr, "expression statement is empty");
        return statement.expr.get();
    };

    auto& direct =
        requireNode<IndexAccessNode>(expressionAt(0), "values[index]");
    requirePosition(direct, 3, 15, "values[index]");

    auto& outer = requireNode<IndexAccessNode>(
        expressionAt(1), "root.items[first][second] outer index"
    );
    auto& inner = requireNode<IndexAccessNode>(
        outer.base.get(), "root.items[first][second] inner index"
    );
    requirePosition(inner, 4, 19, "root.items[first]");
    requirePosition(outer, 4, 26, "root.items[first][second]");

    auto& grouped = requireNode<IndexAccessNode>(
        expressionAt(2), "(root).items[first]"
    );
    requirePosition(grouped, 5, 21, "(root).items[first]");

    errors.clear();
}

} // namespace

int main()
{
    try {
        testIndexLocationsUseLeftBracket();
        std::cout << "parser_index_location_test: all checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "parser_index_location_test: " << error.what() << '\n';
        return 1;
    }
}
