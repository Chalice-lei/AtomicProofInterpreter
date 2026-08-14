#include "ast/ast.h"
#include "error/error_manager.h"
#include "lexer/lexer.h"
#include "parser/parser.h"

#include <iostream>
#include <sstream>
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

void requireCompoundFields(
    const std::vector<CompoundFieldInfo>& fields,
    const std::string& label
)
{
    require(fields.size() == 2, label + ": expected two fields");

    const auto& values = fields.at(0);
    require(values.name == "values", label + ": unexpected first field name");
    require(
        values.type == "uint64[3]",
        label + ": uint64 array type was not normalized"
    );
    require(values.byteSize == 24, label + ": unexpected uint64 byte size");
    require(values.isArray, label + ": uint64 field is not marked as array");
    require(values.arraySize == 3, label + ": unexpected uint64 array size");

    const auto& digest = fields.at(1);
    require(digest.name == "digest", label + ": unexpected second field name");
    require(digest.type == "hex20", label + ": unexpected hex field type");
    require(digest.byteSize == 20, label + ": unexpected hex byte size");
    require(!digest.isArray, label + ": hex field is marked as array");
    require(digest.arraySize == 0, label + ": unexpected hex array size");
}

void testStructAndLocalCompoundTypesShareParsingRules()
{
    const std::string source =
        "Contract CompoundParsing:\n"
        "    Struct Envelope:\n"
        "        payload: {values:uint64[003], digest:hex20}\n"
        "    def main():\n"
        "        local: {values:uint64[003], digest:hex20}\n";

    auto& errors = ErrorManager::getInstance();
    errors.clear();

    Lexer lexer(source, "parser_compound_type_test.ct");
    const auto tokens = lexer.tokenize();
    require(!errors.hasErrors(), "lexer reported an unexpected error");

    Parser parser(tokens);
    const auto contract = parser.parseContract();
    require(contract != nullptr, "parser returned a null contract");
    require(!errors.hasErrors(), "parser reported an unexpected error");
    require(contract->members.size() == 2, "expected two contract members");

    auto& structDef =
        requireNode<StructDefNode>(contract->members.at(0).get(), "Envelope");
    require(structDef.fields.size() == 1, "Envelope should have one field");
    require(
        structDef.fields.front().first == "payload",
        "unexpected Struct field name"
    );
    const auto& structType = structDef.fields.front().second;
    require(structType.isCompoundType, "Struct field is not compound");
    requireCompoundFields(structType.compoundFields, "Struct compound");

    auto& function =
        requireNode<FunctionNode>(contract->members.at(1).get(), "main");
    require(function.block != nullptr, "main has no block");
    require(
        function.block->statements.size() == 1,
        "main should contain one declaration"
    );
    auto& local = requireNode<VarDeclNode>(
        function.block->statements.front().get(), "local"
    );
    require(local.type == "__compound__", "unexpected local variable type");
    require(local.isCompoundType, "local variable is not compound");
    requireCompoundFields(local.compoundFields, "local compound");

    errors.clear();
}

struct ParseFailure
{
    bool threw = false;
    int errorCount = 0;
    std::string output;
};

class StderrCapture
{
public:
    StderrCapture()
        : previous_(std::cerr.rdbuf(stream_.rdbuf()))
    {}

    ~StderrCapture()
    {
        std::cerr.rdbuf(previous_);
    }

    std::string str() const
    {
        return stream_.str();
    }

private:
    std::ostringstream stream_;
    std::streambuf* previous_;
};

ParseFailure parseInvalid(const std::string& source, const std::string& filename)
{
    auto& errors = ErrorManager::getInstance();
    errors.clear();
    errors.setColorOutput(false);
    errors.setShowContext(false);

    ParseFailure result;
    {
        StderrCapture captured;
        try {
            Lexer lexer(source, filename);
            Parser parser(lexer.tokenize());
            (void)parser.parseContract();
        } catch (const std::runtime_error&) {
            result.threw = true;
        }
        result.output = captured.str();
    }

    result.errorCount = errors.getErrorCount();
    errors.clear();
    return result;
}

void requireUnsupportedTypeFailure(
    const ParseFailure& failure,
    const std::string& expectedLocation,
    const std::string& label
)
{
    require(failure.threw, label + ": parser did not throw");
    require(failure.errorCount == 1, label + ": expected exactly one error");
    require(
        failure.output.find(expectedLocation) != std::string::npos,
        label + ": error points to the wrong token"
    );
    require(
        failure.output.find("Unsupported compound type field type: bool") !=
            std::string::npos,
        label + ": unexpected error message"
    );
    require(
        failure.output.find("Only hex<N> and uint64[N] are supported") !=
            std::string::npos,
        label + ": expected compound type guidance"
    );
}

void testStructAndLocalCompoundTypesShareErrors()
{
    const auto structFailure = parseInvalid(
        "Contract InvalidStruct:\n"
        "    Struct Envelope:\n"
        "        payload: {field:bool}\n",
        "parser_compound_struct_invalid.ct"
    );
    requireUnsupportedTypeFailure(
        structFailure,
        "parser_compound_struct_invalid.ct:3:25",
        "Struct compound"
    );

    const auto localFailure = parseInvalid(
        "Contract InvalidLocal:\n"
        "    def main():\n"
        "        local: {field:bool}\n",
        "parser_compound_local_invalid.ct"
    );
    requireUnsupportedTypeFailure(
        localFailure,
        "parser_compound_local_invalid.ct:3:23",
        "local compound"
    );
}

} // namespace

int main()
{
    try {
        testStructAndLocalCompoundTypesShareParsingRules();
        testStructAndLocalCompoundTypesShareErrors();
        std::cout << "parser_compound_type_test: all checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "parser_compound_type_test: " << error.what() << '\n';
        return 1;
    }
}
