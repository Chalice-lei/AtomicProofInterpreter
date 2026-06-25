#include "ast_self_test.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../ast/ast.h"
#include "../crypto/hash_utils.h"
#include "ast_interpreter.h"
#include "runtime_value.h"

namespace apc_interpreter
{
namespace
{

using tbc::BytecodeType;

std::unique_ptr<LiteralNode> numberLiteral(const std::string& value)
{
    return std::make_unique<LiteralNode>(BytecodeType::Number, value);
}

std::unique_ptr<LiteralNode> hexLiteral(const std::string& value)
{
    return std::make_unique<LiteralNode>(BytecodeType::Hex, value);
}

std::unique_ptr<IdentifierNode> identifier(const std::string& name)
{
    return std::make_unique<IdentifierNode>(name);
}

std::unique_ptr<OpNode> op(
    const std::string& name,
    std::unique_ptr<ExprNode> lhs,
    std::unique_ptr<ExprNode> rhs
)
{
    return std::make_unique<OpNode>(
        name,
        std::move(lhs),
        std::move(rhs)
    );
}

std::unique_ptr<CallNode> call(
    const std::string& name,
    std::vector<std::unique_ptr<ExprNode>> args
)
{
    auto node = std::make_unique<CallNode>(name, std::move(args));
    node->isRangeCall = (name == "Range");
    return node;
}

std::unique_ptr<MethodCallNode> methodCall(
    std::unique_ptr<ExprNode> object,
    const std::string& name,
    std::vector<std::unique_ptr<ExprNode>> args = {}
)
{
    return std::make_unique<MethodCallNode>(
        std::move(object),
        name,
        std::move(args)
    );
}

std::unique_ptr<FieldAccessNode> fieldAccess(
    std::unique_ptr<ExprNode> base,
    const std::string& field
)
{
    return std::make_unique<FieldAccessNode>(std::move(base), field);
}

std::unique_ptr<IndexAccessNode> indexAccess(
    std::unique_ptr<ExprNode> base,
    std::unique_ptr<ExprNode> index
)
{
    return std::make_unique<IndexAccessNode>(
        std::move(base),
        std::move(index)
    );
}

std::unique_ptr<BraceExprNode> brace(
    std::vector<std::unique_ptr<ExprNode>> elements
)
{
    return std::make_unique<BraceExprNode>(std::move(elements));
}

std::unique_ptr<ReturnNode> returnExpr(std::unique_ptr<ExprNode> expr)
{
    return std::make_unique<ReturnNode>(std::move(expr));
}

std::unique_ptr<ReturnNode> valueReturnExpr(std::unique_ptr<ExprNode> expr)
{
    return std::make_unique<ReturnNode>(std::move(expr), true);
}

struct ASTSelfTestState
{
    std::ostream& err;
    int failures = 0;

    void expect(bool condition, const std::string& message)
    {
        if (!condition) {
            ++failures;
            err << "FAIL: " << message << std::endl;
        }
    }
};

ContractNode buildArithmeticContract()
{
    ContractNode contract("ArithmeticSmoke");

    auto body = std::make_unique<BlockNode>();
    body->statements.push_back(std::make_unique<VarDeclNode>(
        "x",
        "int",
        op("+", identifier("input"), numberLiteral("1"))
    ));
    body->statements.push_back(std::make_unique<AssignNode>(
        identifier("x"),
        op("*", identifier("x"), numberLiteral("2"))
    ));

    auto ifNode = std::make_unique<IfNode>();
    ifNode->condition = op(">", identifier("x"), numberLiteral("10"));

    auto thenBlock = std::make_unique<BlockNode>();
    thenBlock->statements.push_back(
        returnExpr(op("-", identifier("x"), numberLiteral("3")))
    );
    ifNode->thenBranch = std::move(thenBlock);

    auto elseBlock = std::make_unique<BlockNode>();
    elseBlock->statements.push_back(returnExpr(identifier("x")));
    ifNode->elseBranch = std::move(elseBlock);

    body->statements.push_back(std::move(ifNode));

    std::vector<ParameterInfo> params{{"input", "int"}};
    contract.members.push_back(
        std::make_unique<FunctionNode>("main", params, std::move(body))
    );
    return contract;
}

ContractNode buildDefaultReturnContract()
{
    ContractNode contract("DefaultSmoke");
    auto body = std::make_unique<BlockNode>();
    body->statements.push_back(std::make_unique<VarDeclNode>("flag", "bool"));
    body->statements.push_back(std::make_unique<IfNode>());
    auto* ifNode = dynamic_cast<IfNode*>(body->statements.back().get());
    ifNode->condition = identifier("flag");

    auto thenBlock = std::make_unique<BlockNode>();
    thenBlock->statements.push_back(returnExpr(numberLiteral("1")));
    ifNode->thenBranch = std::move(thenBlock);

    auto elseBlock = std::make_unique<BlockNode>();
    elseBlock->statements.push_back(returnExpr(numberLiteral("0")));
    ifNode->elseBranch = std::move(elseBlock);

    contract.members.push_back(std::make_unique<FunctionNode>(
        "check_default",
        std::vector<ParameterInfo>{},
        std::move(body)
    ));
    return contract;
}

ContractNode buildFunctionLoopArrayContract()
{
    ContractNode contract("FunctionLoopArraySmoke");

    auto doubleBody = std::make_unique<BlockNode>();
    doubleBody->statements.push_back(
        returnExpr(op("*", identifier("x"), numberLiteral("2")))
    );
    contract.members.push_back(std::make_unique<FunctionNode>(
        "_double",
        std::vector<ParameterInfo>{{"x", "int"}},
        std::move(doubleBody),
        "int"
    ));

    auto mainBody = std::make_unique<BlockNode>();

    std::vector<std::unique_ptr<ExprNode>> pushArgs;
    pushArgs.push_back(numberLiteral("0"));
    mainBody->statements.push_back(std::make_unique<VarDeclNode>(
        "total",
        "int",
        call("Push", std::move(pushArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> arrayElements;
    arrayElements.push_back(numberLiteral("1"));
    arrayElements.push_back(numberLiteral("2"));
    arrayElements.push_back(numberLiteral("3"));
    auto arrayLiteral =
        std::make_unique<ArrayDefNode>("int", std::move(arrayElements));
    mainBody->statements.push_back(std::make_unique<ArrayDeclNode>(
        "values",
        "int",
        nullptr,
        std::move(arrayLiteral)
    ));

    auto forNode = std::make_unique<ForNode>();
    forNode->target = "i";

    std::vector<std::unique_ptr<ExprNode>> rangeArgs;
    rangeArgs.push_back(numberLiteral("0"));
    rangeArgs.push_back(numberLiteral("3"));
    forNode->iterable = call("Range", std::move(rangeArgs));

    auto forBody = std::make_unique<BlockNode>();
    forBody->statements.push_back(std::make_unique<AssignNode>(
        identifier("total"),
        op(
            "+",
            identifier("total"),
            std::make_unique<IndexAccessNode>(
                identifier("values"),
                identifier("i")
            )
        )
    ));
    forNode->body = std::move(forBody);
    mainBody->statements.push_back(std::move(forNode));

    std::vector<std::unique_ptr<ExprNode>> doubleArgs;
    doubleArgs.push_back(methodCall(identifier("total"), "Clone"));
    mainBody->statements.push_back(
        returnExpr(call("_double", std::move(doubleArgs)))
    );

    contract.members.push_back(std::make_unique<FunctionNode>(
        "main",
        std::vector<ParameterInfo>{},
        std::move(mainBody),
        "int"
    ));
    return contract;
}

ContractNode buildMultiReturnDestructureContract()
{
    ContractNode contract("MultiReturnDestructureSmoke");

    auto pairBody = std::make_unique<BlockNode>();
    pairBody->statements.push_back(std::make_unique<VarDeclNode>(
        "x_copy",
        "int",
        methodCall(identifier("x"), "Clone")
    ));
    std::vector<std::unique_ptr<ExprNode>> pairValues;
    pairValues.push_back(identifier("x"));
    pairValues.push_back(op("+", identifier("x_copy"), numberLiteral("1")));
    pairBody->statements.push_back(returnExpr(brace(std::move(pairValues))));
    contract.members.push_back(std::make_unique<FunctionNode>(
        "_pair",
        std::vector<ParameterInfo>{{"x", "int"}},
        std::move(pairBody),
        "{int,int}"
    ));

    auto lowerPairBody = std::make_unique<BlockNode>();
    lowerPairBody->statements.push_back(std::make_unique<VarDeclNode>(
        "x_copy",
        "int",
        methodCall(identifier("x"), "Clone")
    ));
    lowerPairBody->statements.push_back(std::make_unique<VarDeclNode>(
        "a",
        "int",
        identifier("x")
    ));
    lowerPairBody->statements.push_back(std::make_unique<VarDeclNode>(
        "b",
        "int",
        op("+", identifier("x_copy"), numberLiteral("2"))
    ));
    std::vector<std::unique_ptr<ExprNode>> lowerPairValues;
    lowerPairValues.push_back(identifier("a"));
    lowerPairValues.push_back(identifier("b"));
    lowerPairBody->statements.push_back(
        valueReturnExpr(brace(std::move(lowerPairValues)))
    );
    contract.members.push_back(std::make_unique<FunctionNode>(
        "_lower_pair",
        std::vector<ParameterInfo>{{"x", "int"}},
        std::move(lowerPairBody),
        "{int,int}"
    ));

    auto mainBody = std::make_unique<BlockNode>();
    mainBody->statements.push_back(std::make_unique<VarDeclNode>(
        "input_copy",
        "int",
        methodCall(identifier("input"), "Clone")
    ));
    std::vector<std::unique_ptr<ExprNode>> pairArgs;
    pairArgs.push_back(identifier("input"));
    mainBody->statements.push_back(std::make_unique<DestructureAssignNode>(
        std::vector<std::string>{"first", "second"},
        call("_pair", std::move(pairArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> lowerPairArgs;
    lowerPairArgs.push_back(identifier("input_copy"));
    mainBody->statements.push_back(std::make_unique<DestructureAssignNode>(
        std::vector<std::string>{"third", "fourth"},
        call("_lower_pair", std::move(lowerPairArgs))
    ));

    mainBody->statements.push_back(returnExpr(op(
        "+",
        op("+", identifier("first"), identifier("second")),
        op("+", identifier("third"), identifier("fourth"))
    )));

    contract.members.push_back(std::make_unique<FunctionNode>(
        "main",
        std::vector<ParameterInfo>{{"input", "int"}},
        std::move(mainBody),
        "int"
    ));
    return contract;
}

ContractNode buildBuiltinFieldContract()
{
    ContractNode contract("BuiltinFieldSmoke");

    auto body = std::make_unique<BlockNode>();

    std::vector<std::unique_ptr<ExprNode>> hashArgs;
    hashArgs.push_back(methodCall(
        fieldAccess(identifier("self"), "pubKey"),
        "Clone"
    ));
    body->statements.push_back(std::make_unique<VarDeclNode>(
        "pubKeyHash",
        "hex",
        call("Hash160", std::move(hashArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> verifyArgs;
    verifyArgs.push_back(identifier("pubKeyHash"));
    verifyArgs.push_back(fieldAccess(identifier("self"), "pubKeyHash"));
    body->statements.push_back(
        std::make_unique<ExprStmtNode>(call("EqualVerify", std::move(verifyArgs)))
    );

    std::vector<std::unique_ptr<ExprNode>> checkSigArgs;
    checkSigArgs.push_back(identifier("sig"));
    checkSigArgs.push_back(fieldAccess(identifier("self"), "pubKey"));
    body->statements.push_back(
        returnExpr(call("CheckSig", std::move(checkSigArgs)))
    );

    contract.members.push_back(std::make_unique<FunctionNode>(
        "verify",
        std::vector<ParameterInfo>{{"sig", "hex"}},
        std::move(body),
        "bool"
    ));
    return contract;
}

ContractNode buildDataBuiltinContract()
{
    ContractNode contract("DataBuiltinSmoke");
    auto body = std::make_unique<BlockNode>();

    body->statements.push_back(std::make_unique<VarDeclNode>(
        "data",
        "hex",
        hexLiteral("0x01020304")
    ));

    std::vector<std::unique_ptr<ExprNode>> sliceArgs;
    sliceArgs.push_back(numberLiteral("1"));
    sliceArgs.push_back(numberLiteral("2"));
    body->statements.push_back(std::make_unique<VarDeclNode>(
        "part",
        "hex",
        methodCall(identifier("data"), "Slice", std::move(sliceArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> verifyPartArgs;
    verifyPartArgs.push_back(identifier("part"));
    verifyPartArgs.push_back(hexLiteral("0x0203"));
    body->statements.push_back(std::make_unique<ExprStmtNode>(
        call("EqualVerify", std::move(verifyPartArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> catArgs;
    catArgs.push_back(identifier("part"));
    catArgs.push_back(hexLiteral("0x04"));
    body->statements.push_back(std::make_unique<VarDeclNode>(
        "combined",
        "hex",
        call("Cat", std::move(catArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> binToNumArgs;
    binToNumArgs.push_back(hexLiteral("0x2a00"));
    body->statements.push_back(std::make_unique<VarDeclNode>(
        "number",
        "int",
        call("BinToNum", std::move(binToNumArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> numToBinArgs;
    numToBinArgs.push_back(identifier("number"));
    numToBinArgs.push_back(numberLiteral("2"));
    body->statements.push_back(std::make_unique<VarDeclNode>(
        "fixed",
        "hex",
        call("NumToBin", std::move(numToBinArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> verifyFixedArgs;
    verifyFixedArgs.push_back(identifier("fixed"));
    verifyFixedArgs.push_back(hexLiteral("0x2a00"));
    body->statements.push_back(std::make_unique<ExprStmtNode>(
        call("EqualVerify", std::move(verifyFixedArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> sizeArgs;
    sizeArgs.push_back(identifier("combined"));
    body->statements.push_back(returnExpr(call("Size", std::move(sizeArgs))));

    contract.members.push_back(std::make_unique<FunctionNode>(
        "main",
        std::vector<ParameterInfo>{},
        std::move(body),
        "int"
    ));
    return contract;
}

ContractNode buildCompoundUnpackContract()
{
    ContractNode contract("CompoundUnpackSmoke");
    auto body = std::make_unique<BlockNode>();

    auto compoundDecl =
        std::make_unique<VarDeclNode>("utxoData", "__compound__");
    compoundDecl->isCompoundType = true;
    compoundDecl->compoundFields = {
        CompoundFieldInfo("txid", "hex32", 32),
        CompoundFieldInfo("vout", "hex4", 4),
        CompoundFieldInfo("sequence", "hex4", 4),
    };
    body->statements.push_back(std::move(compoundDecl));

    std::vector<std::unique_ptr<ExprNode>> pushArgs;
    pushArgs.push_back(fieldAccess(identifier("BVM"), "unlockingInput"));
    body->statements.push_back(std::make_unique<AssignNode>(
        identifier("utxoData"),
        call("Push", std::move(pushArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> verifyArgs;
    verifyArgs.push_back(fieldAccess(identifier("utxoData"), "sequence"));
    verifyArgs.push_back(hexLiteral("0xffffffff"));
    body->statements.push_back(std::make_unique<ExprStmtNode>(
        call("EqualVerify", std::move(verifyArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> binToNumArgs;
    binToNumArgs.push_back(fieldAccess(identifier("utxoData"), "vout"));
    body->statements.push_back(
        returnExpr(call("BinToNum", std::move(binToNumArgs)))
    );

    contract.members.push_back(std::make_unique<FunctionNode>(
        "main",
        std::vector<ParameterInfo>{},
        std::move(body),
        "int"
    ));
    return contract;
}

ContractNode buildParamStructArrayContract()
{
    ContractNode contract("ParamStructArraySmoke");

    contract.members.push_back(std::make_unique<StructDefNode>(
        "Script",
        std::vector<std::pair<std::string, StructFieldType>>{
            {"Size", StructFieldType("hex4")}
        }
    ));
    contract.members.push_back(std::make_unique<StructDefNode>(
        "Output",
        std::vector<std::pair<std::string, StructFieldType>>{
            {"Value", StructFieldType("hex4")},
            {"LockingScript", StructFieldType("Script")}
        }
    ));
    contract.members.push_back(std::make_unique<StructDefNode>(
        "PreTX",
        std::vector<std::pair<std::string, StructFieldType>>{
            {"Outputs", StructFieldType("Output", 2, 0)}
        }
    ));

    auto body = std::make_unique<BlockNode>();

    auto outputAtOne = std::make_unique<IndexAccessNode>(
        fieldAccess(identifier("pretx"), "Outputs"),
        numberLiteral("1")
    );
    auto sizeAtOne = fieldAccess(
        fieldAccess(std::move(outputAtOne), "LockingScript"),
        "Size"
    );

    std::vector<std::unique_ptr<ExprNode>> verifyArgs;
    verifyArgs.push_back(std::move(sizeAtOne));
    verifyArgs.push_back(hexLiteral("0x03000000"));
    body->statements.push_back(std::make_unique<ExprStmtNode>(
        call("EqualVerify", std::move(verifyArgs))
    ));

    auto outputAtOneForValue = std::make_unique<IndexAccessNode>(
        fieldAccess(identifier("pretx"), "Outputs"),
        numberLiteral("1")
    );
    std::vector<std::unique_ptr<ExprNode>> binToNumArgs;
    binToNumArgs.push_back(fieldAccess(std::move(outputAtOneForValue), "Value"));
    body->statements.push_back(
        returnExpr(call("BinToNum", std::move(binToNumArgs)))
    );

    contract.members.push_back(std::make_unique<FunctionNode>(
        "main",
        std::vector<ParameterInfo>{{"pretx", "PreTX"}},
        std::move(body),
        "int"
    ));
    return contract;
}

ContractNode buildLValueAssignmentContract()
{
    ContractNode contract("LValueAssignmentSmoke");

    contract.members.push_back(std::make_unique<StructDefNode>(
        "Script",
        std::vector<std::pair<std::string, StructFieldType>>{
            {"Size", StructFieldType("hex4")}
        }
    ));
    contract.members.push_back(std::make_unique<StructDefNode>(
        "Output",
        std::vector<std::pair<std::string, StructFieldType>>{
            {"Value", StructFieldType("hex4")},
            {"LockingScript", StructFieldType("Script")}
        }
    ));

    auto body = std::make_unique<BlockNode>();
    body->statements.push_back(std::make_unique<ArrayDeclNode>(
        "outputs",
        "Output",
        numberLiteral("2")
    ));

    body->statements.push_back(std::make_unique<AssignNode>(
        fieldAccess(
            indexAccess(identifier("outputs"), numberLiteral("1")),
            "Value"
        ),
        hexLiteral("0x2a000000")
    ));

    body->statements.push_back(std::make_unique<AssignNode>(
        fieldAccess(
            fieldAccess(
                indexAccess(identifier("outputs"), numberLiteral("1")),
                "LockingScript"
            ),
            "Size"
        ),
        hexLiteral("0x03000000")
    ));

    body->statements.push_back(std::make_unique<ArrayDeclNode>(
        "values",
        "int",
        numberLiteral("2")
    ));

    std::vector<std::unique_ptr<ExprNode>> valueArgs;
    valueArgs.push_back(fieldAccess(
        indexAccess(identifier("outputs"), numberLiteral("1")),
        "Value"
    ));
    body->statements.push_back(std::make_unique<AssignNode>(
        indexAccess(identifier("values"), numberLiteral("0")),
        call("BinToNum", std::move(valueArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> sizeArgs;
    sizeArgs.push_back(fieldAccess(
        fieldAccess(
            indexAccess(identifier("outputs"), numberLiteral("1")),
            "LockingScript"
        ),
        "Size"
    ));
    body->statements.push_back(std::make_unique<AssignNode>(
        indexAccess(identifier("values"), numberLiteral("1")),
        call("BinToNum", std::move(sizeArgs))
    ));

    body->statements.push_back(returnExpr(op(
        "+",
        indexAccess(identifier("values"), numberLiteral("0")),
        indexAccess(identifier("values"), numberLiteral("1"))
    )));

    contract.members.push_back(std::make_unique<FunctionNode>(
        "main",
        std::vector<ParameterInfo>{},
        std::move(body),
        "int"
    ));
    return contract;
}

ContractNode buildSequentialStackTransferContract()
{
    ContractNode contract("SequentialStackTransferSmoke");

    auto setupBody = std::make_unique<BlockNode>();
    setupBody->statements.push_back(std::make_unique<VarDeclNode>(
        "a",
        "int",
        op("+", identifier("x"), numberLiteral("1"))
    ));

    std::vector<std::unique_ptr<ExprNode>> setAltArgs;
    setAltArgs.push_back(identifier("a"));
    setupBody->statements.push_back(std::make_unique<ExprStmtNode>(
        call("SetAlt", std::move(setAltArgs))
    ));
    setupBody->statements.push_back(returnExpr(identifier("a")));

    contract.members.push_back(std::make_unique<FunctionNode>(
        "setup",
        std::vector<ParameterInfo>{{"x", "int"}},
        std::move(setupBody),
        "int"
    ));

    auto verifyBody = std::make_unique<BlockNode>();
    std::vector<std::unique_ptr<ExprNode>> setMainArgs;
    setMainArgs.push_back(identifier("a"));
    verifyBody->statements.push_back(std::make_unique<ExprStmtNode>(
        call("SetMain", std::move(setMainArgs))
    ));
    verifyBody->statements.push_back(
        returnExpr(op("+", identifier("a"), numberLiteral("1")))
    );

    contract.members.push_back(std::make_unique<FunctionNode>(
        "verify",
        std::vector<ParameterInfo>{},
        std::move(verifyBody),
        "int"
    ));

    return contract;
}

ContractNode buildStackOwnershipSuccessContract()
{
    ContractNode contract("StackOwnershipSuccess");
    auto body = std::make_unique<BlockNode>();

    body->statements.push_back(std::make_unique<VarDeclNode>(
        "x",
        "int",
        numberLiteral("7")
    ));

    std::vector<std::unique_ptr<ExprNode>> keepArgs;
    keepArgs.push_back(identifier("x"));
    body->statements.push_back(std::make_unique<VarDeclNode>(
        "y",
        "int",
        call("Keep", std::move(keepArgs))
    ));

    std::vector<std::unique_ptr<ExprNode>> moveArgs;
    moveArgs.push_back(identifier("x"));
    body->statements.push_back(std::make_unique<VarDeclNode>(
        "z",
        "int",
        call("Move", std::move(moveArgs))
    ));

    body->statements.push_back(returnExpr(op(
        "+",
        identifier("y"),
        identifier("z")
    )));

    contract.members.push_back(std::make_unique<FunctionNode>(
        "main",
        std::vector<ParameterInfo>{},
        std::move(body),
        "int"
    ));
    return contract;
}

ContractNode buildMoveViolationContract()
{
    ContractNode contract("MoveViolation");
    auto body = std::make_unique<BlockNode>();

    body->statements.push_back(std::make_unique<VarDeclNode>(
        "x",
        "int",
        numberLiteral("7")
    ));

    std::vector<std::unique_ptr<ExprNode>> moveArgs;
    moveArgs.push_back(identifier("x"));
    body->statements.push_back(std::make_unique<VarDeclNode>(
        "y",
        "int",
        call("Move", std::move(moveArgs))
    ));
    body->statements.push_back(returnExpr(identifier("x")));

    contract.members.push_back(std::make_unique<FunctionNode>(
        "main",
        std::vector<ParameterInfo>{},
        std::move(body),
        "int"
    ));
    return contract;
}

ContractNode buildDeleteViolationContract()
{
    ContractNode contract("DeleteViolation");
    auto body = std::make_unique<BlockNode>();

    body->statements.push_back(std::make_unique<VarDeclNode>(
        "x",
        "int",
        numberLiteral("7")
    ));

    std::vector<std::unique_ptr<ExprNode>> deleteArgs;
    deleteArgs.push_back(identifier("x"));
    body->statements.push_back(std::make_unique<ExprStmtNode>(
        call("Delete", std::move(deleteArgs))
    ));
    body->statements.push_back(returnExpr(identifier("x")));

    contract.members.push_back(std::make_unique<FunctionNode>(
        "main",
        std::vector<ParameterInfo>{},
        std::move(body),
        "int"
    ));
    return contract;
}

void testArithmetic(ASTSelfTestState& state)
{
    ContractNode contract = buildArithmeticContract();
    ASTInterpreter interpreter;
    ASTInterpretOptions options;
    options.functionName = "main";
    options.args.push_back(RuntimeValue::fromInt(5));

    ASTInterpretResult result = interpreter.run(contract, options);
    state.expect(result.success, "arithmetic AST run should succeed");
    state.expect(result.returnValues.size() == 1, "arithmetic return count");
    if (!result.returnValues.empty()) {
        state.expect(
            result.returnValues[0].toScriptNum() == 9,
            "arithmetic return value"
        );
    }
}

void testDefaultAndIf(ASTSelfTestState& state)
{
    ContractNode contract = buildDefaultReturnContract();
    ASTInterpreter interpreter;
    ASTInterpretOptions options;
    options.functionName = "check_default";

    ASTInterpretResult result = interpreter.run(contract, options);
    state.expect(result.success, "default bool AST run should succeed");
    state.expect(result.returnValues.size() == 1, "default bool return count");
    if (!result.returnValues.empty()) {
        state.expect(
            result.returnValues[0].toScriptNum() == 0,
            "default bool false branch"
        );
    }
}

void testFunctionLoopArray(ASTSelfTestState& state)
{
    ContractNode contract = buildFunctionLoopArrayContract();
    ASTInterpreter interpreter;
    ASTInterpretOptions options;
    options.functionName = "main";

    ASTInterpretResult result = interpreter.run(contract, options);
    state.expect(result.success, "function/loop/array AST run should succeed");
    state.expect(result.returnValues.size() == 1, "function/loop/array return count");
    if (!result.returnValues.empty()) {
        state.expect(
            result.returnValues[0].toScriptNum() == 12,
            "function/loop/array return value"
        );
    }
}

void testMultiReturnDestructure(ASTSelfTestState& state)
{
    ContractNode contract = buildMultiReturnDestructureContract();
    ASTInterpreter interpreter;
    ASTInterpretOptions options;
    options.functionName = "main";
    options.args.push_back(RuntimeValue::fromInt(5));

    ASTInterpretResult result = interpreter.run(contract, options);
    state.expect(
        result.success,
        "multi-return destructure AST run should succeed"
    );
    state.expect(
        result.returnValues.size() == 1,
        "multi-return destructure return count"
    );
    if (!result.returnValues.empty()) {
        state.expect(
            result.returnValues[0].toScriptNum() == 23,
            "multi-return destructure return value"
        );
    }
}

void testBuiltinFieldAccess(ASTSelfTestState& state)
{
    ContractNode contract = buildBuiltinFieldContract();
    ASTInterpreter interpreter;
    ASTInterpretOptions options;
    options.functionName = "verify";
    options.args.push_back(RuntimeValue::fromHexString("0x01", "hex"));

    RuntimeValue pubKey = RuntimeValue::fromHexString("0x02", "hex");
    options.selfFields["pubKey"] = pubKey;
    options.selfFields["pubKeyHash"] = RuntimeValue::fromBytes(
        apc_crypto::hash160Digest(pubKey.toScriptBytes()),
        "hash160"
    );
    options.bvmFields["checkSigResult"] = RuntimeValue::fromBool(true);

    ASTInterpretResult result = interpreter.run(contract, options);
    state.expect(result.success, "builtin/self field AST run should succeed");
    state.expect(result.returnValues.size() == 1, "builtin/self field return count");
    if (!result.returnValues.empty()) {
        state.expect(
            result.returnValues[0].truthy(),
            "builtin/self field CheckSig result"
        );
    }

    ASTInterpretOptions falseOptions = options;
    falseOptions.bvmFields["checkSigResult"] = RuntimeValue::fromBool(false);
    ASTInterpretResult falseResult = interpreter.run(contract, falseOptions);
    state.expect(
        falseResult.success,
        "builtin/self field false CheckSig AST run should succeed"
    );
    if (!falseResult.returnValues.empty()) {
        state.expect(
            !falseResult.returnValues[0].truthy(),
            "builtin/self field CheckSig should respect false BVM result"
        );
    }
}

void testDataBuiltins(ASTSelfTestState& state)
{
    ContractNode contract = buildDataBuiltinContract();
    ASTInterpreter interpreter;
    ASTInterpretOptions options;
    options.functionName = "main";

    ASTInterpretResult result = interpreter.run(contract, options);
    state.expect(result.success, "data builtin AST run should succeed");
    state.expect(result.returnValues.size() == 1, "data builtin return count");
    if (!result.returnValues.empty()) {
        state.expect(
            result.returnValues[0].toScriptNum() == 3,
            "data builtin return value"
        );
    }
}

void testCompoundUnpack(ASTSelfTestState& state)
{
    ContractNode contract = buildCompoundUnpackContract();
    ASTInterpreter interpreter;
    ASTInterpretOptions options;
    options.functionName = "main";
    options.bvmFields["unlockingInput"] = RuntimeValue::fromHexString(
        "0x000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f"
        "04000000"
        "ffffffff",
        "hex"
    );

    ASTInterpretResult result = interpreter.run(contract, options);
    state.expect(result.success, "compound unpack AST run should succeed");
    state.expect(result.returnValues.size() == 1, "compound unpack return count");
    if (!result.returnValues.empty()) {
        state.expect(
            result.returnValues[0].toScriptNum() == 4,
            "compound unpack vout value"
        );
    }
}

void testParamStructArrayAccess(ASTSelfTestState& state)
{
    ContractNode contract = buildParamStructArrayContract();
    ASTInterpreter interpreter;
    ASTInterpretOptions options;
    options.functionName = "main";

    RuntimeValue::Array outputs;
    outputs.push_back(RuntimeValue::fromStruct(
        {
            {"Value", RuntimeValue::fromHexString("0x00000000", "hex4")},
            {"LockingScript",
             RuntimeValue::fromStruct(
                 {{"Size", RuntimeValue::fromHexString("0x00000000", "hex4")}},
                 "Script"
             )},
        },
        "Output"
    ));
    outputs.push_back(RuntimeValue::fromStruct(
        {
            {"Value", RuntimeValue::fromHexString("0x2a000000", "hex4")},
            {"LockingScript",
             RuntimeValue::fromStruct(
                 {{"Size", RuntimeValue::fromHexString("0x03000000", "hex4")}},
                 "Script"
             )},
        },
        "Output"
    ));

    options.args.push_back(RuntimeValue::fromStruct(
        {{"Outputs", RuntimeValue::fromArray(std::move(outputs), "Output[]")}},
        "PreTX"
    ));

    ASTInterpretResult result = interpreter.run(contract, options);
    state.expect(result.success, "param struct array AST run should succeed");
    state.expect(result.returnValues.size() == 1, "param struct array return count");
    if (!result.returnValues.empty()) {
        state.expect(
            result.returnValues[0].toScriptNum() == 42,
            "param struct array return value"
        );
    }
}

void testLValueAssignment(ASTSelfTestState& state)
{
    ContractNode contract = buildLValueAssignmentContract();
    ASTInterpreter interpreter;
    ASTInterpretOptions options;
    options.functionName = "main";

    ASTInterpretResult result = interpreter.run(contract, options);
    state.expect(result.success, "lvalue assignment AST run should succeed");
    state.expect(result.returnValues.size() == 1, "lvalue assignment return count");
    if (!result.returnValues.empty()) {
        state.expect(
            result.returnValues[0].toScriptNum() == 45,
            "lvalue assignment return value"
        );
    }
}

void testSequentialStackTransfer(ASTSelfTestState& state)
{
    ContractNode contract = buildSequentialStackTransferContract();
    ASTInterpreter interpreter;
    ASTInterpretOptions options;
    options.functionNames = {"setup", "verify"};
    options.callArgs.push_back({RuntimeValue::fromInt(40)});
    options.callArgs.push_back({});

    ASTInterpretResult result = interpreter.run(contract, options);
    state.expect(result.success, "sequential stack-transfer AST run should succeed");
    state.expect(
        result.returnValuesByFunction.size() == 2,
        "sequential stack-transfer return groups"
    );
    if (result.returnValuesByFunction.size() == 2 &&
        !result.returnValuesByFunction[0].empty() &&
        !result.returnValuesByFunction[1].empty()) {
        state.expect(
            result.returnValuesByFunction[0][0].toScriptNum() == 41,
            "sequential setup return value"
        );
        state.expect(
            result.returnValuesByFunction[1][0].toScriptNum() == 42,
            "sequential verify return value"
        );
    }
    state.expect(result.returnValues.size() == 1, "sequential final return count");
    if (!result.returnValues.empty()) {
        state.expect(
            result.returnValues[0].toScriptNum() == 42,
            "sequential final return value"
        );
    }
}

void testStackOwnershipBuiltins(ASTSelfTestState& state)
{
    {
        ContractNode contract = buildStackOwnershipSuccessContract();
        ASTInterpreter interpreter;
        ASTInterpretOptions options;
        options.functionName = "main";

        ASTInterpretResult result = interpreter.run(contract, options);
        state.expect(result.success, "stack ownership success run should succeed");
        state.expect(result.returnValues.size() == 1, "stack ownership return count");
        if (!result.returnValues.empty()) {
            state.expect(
                result.returnValues[0].toScriptNum() == 14,
                "stack ownership return value"
            );
        }
    }

    {
        ContractNode contract = buildMoveViolationContract();
        ASTInterpreter interpreter;
        ASTInterpretOptions options;
        options.functionName = "main";

        ASTInterpretResult result = interpreter.run(contract, options);
        state.expect(!result.success, "Move should consume the source variable");
        state.expect(
            result.errorMessage.find("consumed") != std::string::npos,
            "Move violation error should mention consumed"
        );
    }

    {
        ContractNode contract = buildDeleteViolationContract();
        ASTInterpreter interpreter;
        ASTInterpretOptions options;
        options.functionName = "main";

        ASTInterpretResult result = interpreter.run(contract, options);
        state.expect(!result.success, "Delete should delete the source variable");
        state.expect(
            result.errorMessage.find("deleted") != std::string::npos,
            "Delete violation error should mention deleted"
        );
    }
}

} // namespace

bool runASTSelfTest(std::ostream& out, std::ostream& err)
{
    ASTSelfTestState state{err};
    testArithmetic(state);
    testDefaultAndIf(state);
    testFunctionLoopArray(state);
    testMultiReturnDestructure(state);
    testBuiltinFieldAccess(state);
    testDataBuiltins(state);
    testCompoundUnpack(state);
    testParamStructArrayAccess(state);
    testLValueAssignment(state);
    testSequentialStackTransfer(state);
    testStackOwnershipBuiltins(state);

    if (state.failures == 0) {
        out << "AST interpreter self-test passed" << std::endl;
        return true;
    }

    err << "AST interpreter self-test failed with " << state.failures
        << " failure(s)" << std::endl;
    return false;
}

} // namespace apc_interpreter
