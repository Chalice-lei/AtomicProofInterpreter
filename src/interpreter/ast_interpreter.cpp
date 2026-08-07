#include "ast_interpreter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

#include "../bytecode/byt_data_types.h"
#include "builtin_runtime.h"
#include "function_selection.h"
#include "runtime_argument.h"
#include "runtime_codec.h"
#include "runtime_error.h"
#include "runtime_slot.h"

namespace apc_interpreter
{
namespace
{

using runtime_codec::toLower;

bool isSignatureBuiltinName(const std::string& name)
{
    return name == "CheckSig" || name == "CheckSigVerify" ||
           name == "MultiSig" || name == "MultiSigVerify";
}

std::optional<bool> lookupSignatureResult(
    const RuntimeValue& bvm,
    const std::string& builtinName
)
{
    const RuntimeValue::Struct* fields = nullptr;
    if (bvm.type() == RuntimeType::BuiltinObject) {
        fields = &bvm.builtinObject().fields;
    } else if (bvm.type() == RuntimeType::Struct) {
        fields = &bvm.structFields();
    }

    if (!fields) {
        return std::nullopt;
    }

    std::vector<std::string> keys;
    if (builtinName == "MultiSig" || builtinName == "MultiSigVerify") {
        keys = {"multiSigResult", "signatureValid", "checkSigResult"};
    } else {
        keys = {"checkSigResult", "signatureValid"};
    }

    for (const std::string& key : keys) {
        auto it = fields->find(key);
        if (it != fields->end()) {
            return it->second.truthy();
        }
    }
    return std::nullopt;
}

std::optional<size_t> fixedByteSizeForType(const std::string& typeName)
{
    const std::string lower = toLower(typeName);
    if (lower == "uint64") {
        return 8;
    }
    if (lower.rfind("hex", 0) == 0 && lower.size() > 3) {
        const std::string digits = lower.substr(3);
        if (std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            return static_cast<size_t>(std::stoull(digits));
        }
    }
    return std::nullopt;
}

std::vector<uint8_t> checkedSlice(
    const std::vector<uint8_t>& bytes,
    size_t offset,
    size_t size,
    const std::string& fieldName,
    SourceLocation location
)
{
    if (offset + size > bytes.size()) {
        throw RuntimeError(
            RuntimeErrorKind::TypeMismatch,
            "not enough bytes to unpack field '" + fieldName + "'",
            location
        );
    }
    return std::vector<uint8_t>(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)
    );
}

std::string astNodeName(const ASTNode& node)
{
    if (dynamic_cast<const CallNode*>(&node)) {
        return "CallNode";
    }
    if (dynamic_cast<const MethodCallNode*>(&node)) {
        return "MethodCallNode";
    }
    if (dynamic_cast<const FieldAccessNode*>(&node)) {
        return "FieldAccessNode";
    }
    if (dynamic_cast<const IndexAccessNode*>(&node)) {
        return "IndexAccessNode";
    }
    if (dynamic_cast<const ForNode*>(&node)) {
        return "ForNode";
    }
    if (dynamic_cast<const ArrayDeclNode*>(&node)) {
        return "ArrayDeclNode";
    }
    if (dynamic_cast<const DestructureAssignNode*>(&node)) {
        return "DestructureAssignNode";
    }
    return "ASTNode";
}

} // namespace

ASTInterpretResult ASTInterpreter::run(
    ContractNode& contract,
    const ASTInterpretOptions& options
)
{
    ASTInterpretResult result;

    try {
        m_context = RuntimeContext();
        m_compoundDeclarations.clear();
        m_context.registerContract(contract);
        if (!options.selfFields.empty()) {
            m_context.setSelf(
                RuntimeValue::fromStruct(options.selfFields, "self")
            );
        }
        if (!options.bvmFields.empty()) {
            m_context.setBvm(
                RuntimeValue::fromBuiltinObject("BVM", options.bvmFields)
            );
        }

        std::vector<std::string> functionNames = options.functionNames;
        if (functionNames.empty()) {
            functionNames.push_back(
                chooseFunctionName(contract, options.functionName)
            );
        }

        std::ostringstream functionList;
        for (size_t i = 0; i < functionNames.size(); ++i) {
            const std::string& functionName = functionNames[i];
            if (i != 0) {
                functionList << ",";
            }
            functionList << functionName;

            FunctionNode* function = m_context.findFunction(functionName);
            if (!function) {
                throw RuntimeError(
                    RuntimeErrorKind::UndefinedVariable,
                    "function '" + functionName + "' is not defined",
                    contract.sourceLocation
                );
            }

            const std::vector<RuntimeValue>& callArgs =
                i < options.callArgs.size()
                    ? options.callArgs[i]
                    : (i == 0 ? options.args
                              : std::vector<RuntimeValue>{});
            result.functionNames.push_back(functionName);
            result.returnValuesByFunction.push_back(
                callFunction(*function, callArgs, contract.sourceLocation)
            );
        }

        result.functionName = functionList.str();
        if (!result.returnValuesByFunction.empty()) {
            result.returnValues = result.returnValuesByFunction.back();
        }
        result.success = true;
        return result;
    } catch (const RuntimeError& e) {
        result.errorMessage = e.kindName() + ": " + e.what();
        if (!e.suggestion().empty()) {
            result.errorMessage += " (" + e.suggestion() + ")";
        }
        return result;
    } catch (const std::exception& e) {
        result.errorMessage = e.what();
        return result;
    }
}

void ASTInterpreter::beginSession(
    ContractNode& contract,
    const ASTInterpretOptions& options
)
{
    m_context = RuntimeContext();
    m_compoundDeclarations.clear();
    m_context.registerContract(contract);
    if (!options.selfFields.empty()) {
        m_context.setSelf(RuntimeValue::fromStruct(options.selfFields, "self"));
    }
    if (!options.bvmFields.empty()) {
        m_context.setBvm(
            RuntimeValue::fromBuiltinObject("BVM", options.bvmFields)
        );
    }
    m_currentEnv = m_context.globalEnvironment();
}

void ASTInterpreter::refreshSessionProgram(ContractNode& contract)
{
    m_context.registerContract(contract);
    m_currentEnv = m_context.globalEnvironment();
}

ASTReplExecutionResult ASTInterpreter::executeReplBlock(BlockNode& block)
{
    ASTReplExecutionResult result;

    try {
        m_currentEnv = m_context.globalEnvironment();
        ensureCurrentEnvironment();

        for (size_t i = 0; i < block.statements.size(); ++i) {
            StmtNode& stmt = *block.statements[i];
            const bool isLast = i + 1 == block.statements.size();

            if (isLast) {
                if (auto* exprStmt = dynamic_cast<ExprStmtNode*>(&stmt)) {
                    if (exprStmt->expr) {
                        RuntimeValue value = evalExpr(*exprStmt->expr);
                        if (!value.isVoid()) {
                            result.hasOutput = true;
                            result.outputValues.push_back(std::move(value));
                        }
                    }
                    result.success = true;
                    return result;
                }
            }

            ControlFlow flow = execStmt(stmt);
            if (flow.kind == FlowKind::Return) {
                result.hasOutput = !flow.values.empty();
                result.outputValues = std::move(flow.values);
                result.success = true;
                return result;
            }
        }

        result.success = true;
        return result;
    } catch (const RuntimeError& e) {
        result.errorMessage = e.kindName() + ": " + e.what();
        if (!e.suggestion().empty()) {
            result.errorMessage += " (" + e.suggestion() + ")";
        }
        m_currentEnv = m_context.globalEnvironment();
        return result;
    } catch (const std::exception& e) {
        result.errorMessage = e.what();
        m_currentEnv = m_context.globalEnvironment();
        return result;
    }
}

std::vector<std::string> ASTInterpreter::globalNames() const
{
    auto environment = m_context.globalEnvironment();
    return environment ? environment->localNames() : std::vector<std::string>{};
}

RuntimeValue ASTInterpreter::evalExpr(ExprNode& expr)
{
    if (auto* node = dynamic_cast<LiteralNode*>(&expr)) {
        return evalLiteral(*node);
    }
    if (auto* node = dynamic_cast<IdentifierNode*>(&expr)) {
        return evalIdentifier(*node);
    }
    if (auto* node = dynamic_cast<OpNode*>(&expr)) {
        return evalOp(*node);
    }
    if (auto* node = dynamic_cast<ArrayDefNode*>(&expr)) {
        return evalArray(*node);
    }
    if (auto* node = dynamic_cast<BraceExprNode*>(&expr)) {
        return evalBrace(*node);
    }
    if (auto* node = dynamic_cast<CallNode*>(&expr)) {
        return evalCall(*node);
    }
    if (auto* node = dynamic_cast<MethodCallNode*>(&expr)) {
        return evalMethodCall(*node);
    }
    if (auto* node = dynamic_cast<FieldAccessNode*>(&expr)) {
        return evalFieldAccess(*node);
    }
    if (auto* node = dynamic_cast<IndexAccessNode*>(&expr)) {
        return evalIndexAccess(*node);
    }

    throw RuntimeError(
        RuntimeErrorKind::Generic,
        "unsupported expression node: " + astNodeName(expr),
        locationOf(expr)
    );
}

RuntimeValue ASTInterpreter::evalLiteral(LiteralNode& node)
{
    switch (node.type) {
        case tbc::BytecodeType::Number:
            return RuntimeValue::fromInt(std::stoll(node.value), "number");
        case tbc::BytecodeType::Boolean: {
            const std::string lower = toLower(node.value);
            return RuntimeValue::fromBool(
                lower == "true" || lower == "1" || lower == "yes"
            );
        }
        case tbc::BytecodeType::String:
            return RuntimeValue::fromString(node.value);
        case tbc::BytecodeType::Hex:
            return RuntimeValue::fromHexString(node.value, "hex");
        case tbc::BytecodeType::Addr:
            return RuntimeValue::fromAddress(node.value);
        default:
            return RuntimeValue::fromHexString(
                node.value,
                tbc::TypeMapper::toString(node.type)
            );
    }
}

RuntimeValue ASTInterpreter::evalIdentifier(IdentifierNode& node)
{
    ensureCurrentEnvironment();

    const std::string lower = toLower(node.name);
    if (lower == "true") {
        return RuntimeValue::fromBool(true);
    }
    if (lower == "false") {
        return RuntimeValue::fromBool(false);
    }
    if (node.name == "self") {
        return m_context.self();
    }
    if (node.name == "BVM" || node.name == "bvm") {
        return m_context.bvm();
    }

    return m_currentEnv->resolve(node.name).value;
}

RuntimeValue ASTInterpreter::evalOp(OpNode& node)
{
    if (!node.lhs) {
        RuntimeValue rhs = evalExpr(*node.rhs);
        if (node.op == "-") {
            return RuntimeValue::fromInt(-rhs.toScriptNum(), rhs.declaredType());
        }
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "unsupported unary operator '" + node.op + "'",
            locationOf(node)
        );
    }

    RuntimeValue lhs = evalExpr(*node.lhs);
    RuntimeValue rhs = evalExpr(*node.rhs);

    if (node.op == "+") {
        if (lhs.type() == RuntimeType::String || rhs.type() == RuntimeType::String) {
            return RuntimeValue::fromString(
                lhs.toDisplayString() + rhs.toDisplayString()
            );
        }
        return RuntimeValue::fromInt(lhs.toScriptNum() + rhs.toScriptNum());
    }
    if (node.op == "-") {
        return RuntimeValue::fromInt(lhs.toScriptNum() - rhs.toScriptNum());
    }
    if (node.op == "*") {
        return RuntimeValue::fromInt(lhs.toScriptNum() * rhs.toScriptNum());
    }
    if (node.op == "/") {
        const int64_t divisor = rhs.toScriptNum();
        if (divisor == 0) {
            throw RuntimeError(
                RuntimeErrorKind::Generic,
                "division by zero",
                locationOf(node)
            );
        }
        return RuntimeValue::fromInt(lhs.toScriptNum() / divisor);
    }
    if (node.op == "==" || node.op == "=") {
        return RuntimeValue::fromBool(lhs == rhs);
    }
    if (node.op == "!=") {
        return RuntimeValue::fromBool(lhs != rhs);
    }
    if (node.op == "<") {
        return RuntimeValue::fromBool(lhs.toScriptNum() < rhs.toScriptNum());
    }
    if (node.op == ">") {
        return RuntimeValue::fromBool(lhs.toScriptNum() > rhs.toScriptNum());
    }
    if (node.op == "<=") {
        return RuntimeValue::fromBool(lhs.toScriptNum() <= rhs.toScriptNum());
    }
    if (node.op == ">=") {
        return RuntimeValue::fromBool(lhs.toScriptNum() >= rhs.toScriptNum());
    }
    if (node.op == "&&" || node.op == "and") {
        return RuntimeValue::fromBool(lhs.truthy() && rhs.truthy());
    }
    if (node.op == "||" || node.op == "or") {
        return RuntimeValue::fromBool(lhs.truthy() || rhs.truthy());
    }

    throw RuntimeError(
        RuntimeErrorKind::Generic,
        "unsupported binary operator '" + node.op + "'",
        locationOf(node)
    );
}

RuntimeValue ASTInterpreter::evalArray(ArrayDefNode& node)
{
    RuntimeValue::Array values;
    values.reserve(node.elements.size());
    for (auto& element : node.elements) {
        values.push_back(evalExpr(*element));
    }
    return RuntimeValue::fromArray(std::move(values), node.elementType);
}

RuntimeValue ASTInterpreter::evalBrace(BraceExprNode& node)
{
    RuntimeValue::Array values;
    values.reserve(node.elements.size());
    for (auto& element : node.elements) {
        values.push_back(evalExpr(*element));
    }
    return RuntimeValue::fromArray(std::move(values), "brace");
}

RuntimeValue ASTInterpreter::evalCall(CallNode& node)
{
    if (node.funcName == "Range" || node.isRangeCall) {
        RuntimeValue::Array values;
        for (int64_t item : evalRange(node)) {
            values.push_back(RuntimeValue::fromInt(item, "int"));
        }
        return RuntimeValue::fromArray(std::move(values), "int[]");
    }

    if (node.funcName == "Delete" || node.funcName == "Keep" ||
        node.funcName == "Move" || node.funcName == "SetAlt" ||
        node.funcName == "SetMain") {
        return evalStackTransferCall(node);
    }

    std::vector<RuntimeValue> args;
    args.reserve(node.args.size());
    for (auto& arg : node.args) {
        args.push_back(evalExpr(*arg));
    }

    if (isSignatureBuiltinName(node.funcName)) {
        if (args.size() != 2) {
            throw RuntimeError(
                RuntimeErrorKind::BuiltinError,
                node.funcName + "(...) expects 2 argument(s), got " +
                    std::to_string(args.size()),
                locationOf(node)
            );
        }

        std::optional<bool> configuredResult =
            lookupSignatureResult(m_context.bvm(), node.funcName);
        if (!configuredResult.has_value()) {
            throw RuntimeError(
                RuntimeErrorKind::BuiltinError,
                node.funcName +
                    "(...) requires BVM.checkSigResult or "
                    "BVM.multiSigResult in AST interpreter mode",
                locationOf(node),
                "Pass --bvm checkSigResult=true/false or provide "
                "multiSigResult for multisig checks"
            );
        }

        const bool ok = configuredResult.value();
        if (node.funcName == "CheckSigVerify" ||
            node.funcName == "MultiSigVerify") {
            if (!ok) {
                throw RuntimeError(
                    RuntimeErrorKind::BuiltinError,
                    node.funcName + "(...) failed",
                    locationOf(node)
                );
            }
            return RuntimeValue::voidValue();
        }
        return RuntimeValue::fromBool(ok);
    }

    if (BuiltinRuntime::isBuiltinFunction(node.funcName)) {
        return BuiltinRuntime::callFunction(
            node.funcName,
            args,
            locationOf(node)
        );
    }

    FunctionNode* function = m_context.findFunction(node.funcName);
    if (!function) {
        throw RuntimeError(
            RuntimeErrorKind::UndefinedVariable,
            "function '" + node.funcName + "' is not defined",
            locationOf(node)
        );
    }

    return firstOrAggregateReturn(
        callFunction(*function, args, locationOf(node)),
        node.funcName
    );
}

RuntimeValue ASTInterpreter::evalStackTransferCall(CallNode& node)
{
    if (node.args.size() != 1) {
        throw RuntimeError(
            RuntimeErrorKind::BuiltinError,
            node.funcName + "(...) expects 1 argument(s), got " +
                std::to_string(node.args.size()),
            locationOf(node)
        );
    }

    auto* identifier =
        dynamic_cast<IdentifierNode*>(node.args.front().get());
    if (!identifier) {
        return BuiltinRuntime::callFunction(
            node.funcName,
            {evalExpr(*node.args.front())},
            locationOf(node)
        );
    }

    ensureCurrentEnvironment();
    auto globalEnv = m_context.globalEnvironment();
    const std::string& name = identifier->name;

    if (node.funcName == "Delete") {
        m_currentEnv->markDeleted(name);
        return RuntimeValue::voidValue();
    }

    if (node.funcName == "Keep") {
        RuntimeSlot& slot = m_currentEnv->resolve(name);
        m_currentEnv->markKeep(name);
        return slot.value;
    }

    if (node.funcName == "Move") {
        RuntimeSlot& slot = m_currentEnv->resolve(name);
        RuntimeValue value = slot.value;
        m_currentEnv->markConsumed(name);
        return value;
    }

    if (node.funcName == "SetMain") {
        if (auto* globalSlot = globalEnv->tryResolve(name)) {
            const RuntimeSlot& usableGlobalSlot = globalEnv->resolve(name);
            RuntimeValue value = usableGlobalSlot.value;
            if (m_currentEnv->containsLocal(name)) {
                RuntimeSlot& localSlot = m_currentEnv->assign(name, value);
                localSlot.storage = StorageClass::MainStack;
            } else {
                m_currentEnv->define(
                    name,
                    RuntimeSlot(
                        value,
                        usableGlobalSlot.declaredType.empty()
                            ? value.declaredType()
                            : usableGlobalSlot.declaredType,
                        StorageClass::MainStack
                    )
                );
            }
            globalSlot->storage = StorageClass::MainStack;
            return value;
        }

        RuntimeSlot& slot = m_currentEnv->resolve(name);
        slot.storage = StorageClass::MainStack;
        return slot.value;
    }

    RuntimeSlot& slot = m_currentEnv->resolve(name);
    RuntimeValue value = slot.value;
    if (node.funcName == "SetAlt") {
        slot.storage = StorageClass::AltStack;
        if (globalEnv->containsLocal(name)) {
            RuntimeSlot& globalSlot = globalEnv->assign(name, value);
            globalSlot.storage = StorageClass::AltStack;
        } else {
            globalEnv->define(
                name,
                RuntimeSlot(
                    value,
                    slot.declaredType.empty()
                        ? value.declaredType()
                        : slot.declaredType,
                    StorageClass::AltStack
                )
            );
        }
    }
    return value;
}

RuntimeValue ASTInterpreter::evalMethodCall(MethodCallNode& node)
{
    if (!node.object) {
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "method call has no receiver",
            locationOf(node)
        );
    }

    RuntimeValue object = evalExpr(*node.object);

    if (node.methodName == "Clone") {
        if (!node.args.empty()) {
            throw RuntimeError(
                RuntimeErrorKind::BuiltinError,
                ".Clone() expects no arguments",
                locationOf(node)
            );
        }
        return object;
    }

    if (node.methodName == "Push") {
        if (!node.args.empty()) {
            throw RuntimeError(
                RuntimeErrorKind::BuiltinError,
                ".Push() expects no arguments",
                locationOf(node)
            );
        }
        return object;
    }

    if (BuiltinRuntime::isBuiltinFunction(node.methodName)) {
        std::vector<RuntimeValue> args;
        args.reserve(node.args.size() + 1);
        args.push_back(std::move(object));
        for (auto& arg : node.args) {
            args.push_back(evalExpr(*arg));
        }
        return BuiltinRuntime::callFunction(
            node.methodName,
            args,
            locationOf(node)
        );
    }

    throw RuntimeError(
        RuntimeErrorKind::BuiltinError,
        "unsupported method call '." + node.methodName + "()'",
        locationOf(node)
    );
}

RuntimeValue ASTInterpreter::evalFieldAccess(FieldAccessNode& node)
{
    if (!node.base) {
        throw RuntimeError(
            RuntimeErrorKind::InvalidFieldAccess,
            "field access has no base expression",
            locationOf(node)
        );
    }

    RuntimeValue base = evalExpr(*node.base);
    if (base.type() == RuntimeType::Struct) {
        const auto& fields = base.structFields();
        auto it = fields.find(node.field);
        if (it != fields.end()) {
            return it->second;
        }
        throw RuntimeError(
            RuntimeErrorKind::InvalidFieldAccess,
            "struct value has no field '" + node.field + "'",
            locationOf(node),
            "Pass deployment fields with --self " + node.field + "=<value>"
        );
    }

    if (base.type() == RuntimeType::BuiltinObject) {
        const auto& object = base.builtinObject();
        auto it = object.fields.find(node.field);
        if (it != object.fields.end()) {
            return it->second;
        }
        throw RuntimeError(
            RuntimeErrorKind::InvalidFieldAccess,
            "builtin object '" + object.name + "' has no field '" +
                node.field + "'",
            locationOf(node),
            "Pass runtime fields with --bvm " + node.field + "=<value>"
        );
    }

    if ((node.field == "Size" || node.field == "size" ||
         node.field == "Length" || node.field == "length")) {
        if (base.type() == RuntimeType::Array) {
            return RuntimeValue::fromInt(
                static_cast<int64_t>(base.array().size()),
                "int"
            );
        }
        if (base.type() == RuntimeType::Bytes) {
            return RuntimeValue::fromInt(
                static_cast<int64_t>(base.bytes().size()),
                "int"
            );
        }
        if (base.type() == RuntimeType::String ||
            base.type() == RuntimeType::Address) {
            return RuntimeValue::fromInt(
                static_cast<int64_t>(base.stringValue().size()),
                "int"
            );
        }
    }

    throw RuntimeError(
        RuntimeErrorKind::InvalidFieldAccess,
        "value of type '" + RuntimeValue::typeName(base.type()) +
            "' has no field '" + node.field + "'",
        locationOf(node)
    );
}

RuntimeValue ASTInterpreter::evalIndexAccess(IndexAccessNode& node)
{
    if (!node.base || !node.index) {
        throw RuntimeError(
            RuntimeErrorKind::InvalidIndexAccess,
            "index access requires both base and index expressions",
            locationOf(node)
        );
    }

    RuntimeValue base = evalExpr(*node.base);
    const int64_t rawIndex = evalExpr(*node.index).toScriptNum();
    if (rawIndex < 0) {
        throw RuntimeError(
            RuntimeErrorKind::InvalidIndexAccess,
            "array index cannot be negative",
            locationOf(node)
        );
    }
    const size_t index = static_cast<size_t>(rawIndex);

    if (base.type() == RuntimeType::Array) {
        const auto& values = base.array();
        if (index >= values.size()) {
            throw RuntimeError(
                RuntimeErrorKind::InvalidIndexAccess,
                "array index " + std::to_string(index) +
                    " is out of bounds for length " +
                    std::to_string(values.size()),
                locationOf(node)
            );
        }
        return values[index];
    }

    if (base.type() == RuntimeType::Bytes) {
        const auto& bytes = base.bytes();
        if (index >= bytes.size()) {
            throw RuntimeError(
                RuntimeErrorKind::InvalidIndexAccess,
                "bytes index " + std::to_string(index) +
                    " is out of bounds for length " +
                    std::to_string(bytes.size()),
                locationOf(node)
            );
        }
        return RuntimeValue::fromInt(bytes[index], "uint8");
    }

    if (base.type() == RuntimeType::String ||
        base.type() == RuntimeType::Address) {
        const auto& text = base.stringValue();
        if (index >= text.size()) {
            throw RuntimeError(
                RuntimeErrorKind::InvalidIndexAccess,
                "string index " + std::to_string(index) +
                    " is out of bounds for length " +
                    std::to_string(text.size()),
                locationOf(node)
            );
        }
        return RuntimeValue::fromString(std::string(1, text[index]));
    }

    throw RuntimeError(
        RuntimeErrorKind::InvalidIndexAccess,
        "value of type '" + RuntimeValue::typeName(base.type()) +
            "' is not indexable",
        locationOf(node)
    );
}

RuntimeValue&
ASTInterpreter::resolveAssignable(ExprNode& target, SourceLocation location)
{
    ensureCurrentEnvironment();

    if (auto* identifier = dynamic_cast<IdentifierNode*>(&target)) {
        const std::string lower = toLower(identifier->name);
        if (lower == "true" || lower == "false" ||
            identifier->name == "self" || identifier->name == "BVM" ||
            identifier->name == "bvm") {
            throw RuntimeError(
                RuntimeErrorKind::Generic,
                "runtime object or literal '" + identifier->name +
                    "' is not assignable",
                location
            );
        }
        return m_currentEnv->resolve(identifier->name).value;
    }

    if (auto* field = dynamic_cast<FieldAccessNode*>(&target)) {
        if (!field->base) {
            throw RuntimeError(
                RuntimeErrorKind::InvalidFieldAccess,
                "field assignment has no base expression",
                location
            );
        }

        RuntimeValue& base = resolveAssignable(*field->base, location);
        if (base.type() != RuntimeType::Struct) {
            throw RuntimeError(
                RuntimeErrorKind::InvalidFieldAccess,
                "value of type '" + RuntimeValue::typeName(base.type()) +
                    "' has no assignable field '" + field->field + "'",
                location
            );
        }

        auto& fields = base.structFields();
        auto it = fields.find(field->field);
        if (it != fields.end()) {
            return it->second;
        }

        if (StructDefNode* structDef = m_context.findStruct(base.declaredType())) {
            for (const auto& [fieldName, fieldType] : structDef->fields) {
                if (fieldName == field->field) {
                    auto inserted = fields.emplace(
                        fieldName,
                        defaultValueForStructField(fieldType)
                    );
                    return inserted.first->second;
                }
            }
        }

        throw RuntimeError(
            RuntimeErrorKind::InvalidFieldAccess,
            "struct value has no assignable field '" + field->field + "'",
            location
        );
    }

    if (auto* index = dynamic_cast<IndexAccessNode*>(&target)) {
        if (!index->base || !index->index) {
            throw RuntimeError(
                RuntimeErrorKind::InvalidIndexAccess,
                "index assignment requires both base and index expressions",
                location
            );
        }

        RuntimeValue& base = resolveAssignable(*index->base, location);
        const int64_t rawIndex = evalExpr(*index->index).toScriptNum();
        if (rawIndex < 0) {
            throw RuntimeError(
                RuntimeErrorKind::InvalidIndexAccess,
                "array index cannot be negative",
                location
            );
        }
        if (base.type() != RuntimeType::Array) {
            throw RuntimeError(
                RuntimeErrorKind::InvalidIndexAccess,
                "value of type '" + RuntimeValue::typeName(base.type()) +
                    "' is not assignable by index",
                location
            );
        }

        auto& values = base.array();
        const size_t offset = static_cast<size_t>(rawIndex);
        if (offset >= values.size()) {
            throw RuntimeError(
                RuntimeErrorKind::InvalidIndexAccess,
                "array index " + std::to_string(offset) +
                    " is out of bounds for length " +
                    std::to_string(values.size()),
                location
            );
        }
        return values[offset];
    }

    throw RuntimeError(
        RuntimeErrorKind::Generic,
        "expression '" + astNodeName(target) + "' is not assignable",
        location
    );
}

void ASTInterpreter::assignIdentifier(
    IdentifierNode& target,
    RuntimeValue value,
    SourceLocation location
)
{
    const std::string lower = toLower(target.name);
    if (lower == "true" || lower == "false" ||
        target.name == "self" || target.name == "BVM" || target.name == "bvm") {
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "runtime object or literal '" + target.name + "' is not assignable",
            location
        );
    }

    const std::vector<CompoundFieldInfo>* compoundFields = nullptr;
    if (m_currentEnv->contains(target.name)) {
        RuntimeSlot& slot = m_currentEnv->resolve(target.name);
        auto compoundIt = m_compoundDeclarations.find(target.name);
        if (compoundIt != m_compoundDeclarations.end()) {
            compoundFields = &compoundIt->second;
        }
        value = coerceDeclaredValue(
            std::move(value),
            slot.declaredType,
            compoundFields,
            location
        );
        m_currentEnv->assign(target.name, std::move(value));
    } else {
        const std::string declaredType = value.declaredType();
        m_currentEnv->define(
            target.name,
            RuntimeSlot(
                std::move(value),
                declaredType,
                StorageClass::MainStack
            )
        );
    }
}

ASTInterpreter::ControlFlow ASTInterpreter::execStmt(StmtNode& stmt)
{
    if (auto* node = dynamic_cast<BlockNode*>(&stmt)) {
        return execBlock(*node, true);
    }
    if (auto* node = dynamic_cast<IfNode*>(&stmt)) {
        return execIf(*node);
    }
    if (auto* node = dynamic_cast<ForNode*>(&stmt)) {
        return execFor(*node);
    }
    if (auto* node = dynamic_cast<AssignNode*>(&stmt)) {
        return execAssign(*node);
    }
    if (auto* node = dynamic_cast<VarDeclNode*>(&stmt)) {
        return execVarDecl(*node);
    }
    if (auto* node = dynamic_cast<ArrayDeclNode*>(&stmt)) {
        return execArrayDecl(*node);
    }
    if (auto* node = dynamic_cast<ExprStmtNode*>(&stmt)) {
        return execExprStmt(*node);
    }
    if (auto* node = dynamic_cast<ReturnNode*>(&stmt)) {
        return execReturn(*node);
    }
    if (auto* node = dynamic_cast<DestructureAssignNode*>(&stmt)) {
        return execDestructureAssign(*node);
    }

    throw RuntimeError(
        RuntimeErrorKind::Generic,
        "unsupported statement node: " + astNodeName(stmt),
        locationOf(stmt)
    );
}

ASTInterpreter::ControlFlow
ASTInterpreter::execBlock(BlockNode& block, bool createScope)
{
    ensureCurrentEnvironment();

    std::shared_ptr<Environment> previousEnv = m_currentEnv;
    if (createScope) {
        m_currentEnv = m_currentEnv->createChild("block");
    }

    for (auto& stmt : block.statements) {
        ControlFlow flow = execStmt(*stmt);
        if (flow.kind != FlowKind::Normal) {
            m_currentEnv = previousEnv;
            return flow;
        }
    }

    m_currentEnv = previousEnv;
    return {};
}

ASTInterpreter::ControlFlow ASTInterpreter::execIf(IfNode& node)
{
    if (!node.condition) {
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "if statement has no condition",
            locationOf(node)
        );
    }

    if (evalExpr(*node.condition).truthy()) {
        if (node.thenBranch) {
            return execStmt(*node.thenBranch);
        }
        return {};
    }

    if (node.elseBranch) {
        return execStmt(*node.elseBranch);
    }
    return {};
}

ASTInterpreter::ControlFlow ASTInterpreter::execFor(ForNode& node)
{
    ensureCurrentEnvironment();

    if (!node.iterable) {
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "for loop has no iterable expression",
            locationOf(node)
        );
    }
    if (!node.body) {
        return {};
    }

    std::vector<RuntimeValue> iterationValues;
    bool isStaticRangeLoop = false;
    if (auto* rangeCall = dynamic_cast<CallNode*>(node.iterable.get());
        rangeCall && (rangeCall->funcName == "Range" ||
                      rangeCall->isRangeCall)) {
        isStaticRangeLoop = true;
        for (int64_t value : evalRange(*rangeCall)) {
            iterationValues.push_back(RuntimeValue::fromInt(value, "int"));
        }
    } else {
        RuntimeValue iterable = evalExpr(*node.iterable);
        if (iterable.type() != RuntimeType::Array) {
            throw RuntimeError(
                RuntimeErrorKind::TypeMismatch,
                "for loop iterable must be Range(...) or an array",
                locationOf(*node.iterable)
            );
        }
        iterationValues = iterable.array();
    }

    std::shared_ptr<Environment> previousEnv = m_currentEnv;
    std::shared_ptr<Environment> loopEnv = m_currentEnv->createChild("for");
    m_currentEnv = loopEnv;

    try {
        for (RuntimeValue value : iterationValues) {
            if (loopEnv->contains(node.target)) {
                loopEnv->assign(node.target, std::move(value));
            } else {
                // A non-empty static loop introduces its induction target in
                // the enclosing environment. Iteration bodies still execute
                // in child scopes, so their locals do not leak across rounds.
                // Empty loops never reach this definition and introduce no
                // target, matching bytecode lowering.
                auto targetEnvironment =
                    isStaticRangeLoop ? previousEnv : loopEnv;
                targetEnvironment->define(
                    node.target,
                    RuntimeSlot(
                        std::move(value),
                        node.getInferredType(),
                        StorageClass::MainStack
                    )
                );
            }

            ControlFlow flow = execBlock(*node.body, true);
            if (flow.kind != FlowKind::Normal) {
                m_currentEnv = previousEnv;
                return flow;
            }

            m_currentEnv = loopEnv;
        }
    } catch (...) {
        m_currentEnv = previousEnv;
        throw;
    }

    m_currentEnv = previousEnv;
    return {};
}

ASTInterpreter::ControlFlow ASTInterpreter::execAssign(AssignNode& node)
{
    ensureCurrentEnvironment();

    RuntimeValue value =
        node.value ? evalExpr(*node.value) : RuntimeValue::voidValue();

    if (auto* identifier = dynamic_cast<IdentifierNode*>(node.name.get())) {
        assignIdentifier(*identifier, std::move(value), locationOf(node));
        return {};
    }

    if (!node.name) {
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "assignment has no target expression",
            locationOf(node)
        );
    }

    RuntimeValue& target = resolveAssignable(*node.name, locationOf(node));
    if (!target.declaredType().empty()) {
        value = coerceDeclaredValue(
            std::move(value),
            target.declaredType(),
            nullptr,
            locationOf(node)
        );
        value.setDeclaredType(target.declaredType());
    }
    target = std::move(value);
    return {};
}

ASTInterpreter::ControlFlow ASTInterpreter::execVarDecl(VarDeclNode& node)
{
    ensureCurrentEnvironment();

    const std::vector<CompoundFieldInfo>* compoundFields = nullptr;
    if (node.isCompoundType) {
        m_compoundDeclarations[node.name] = node.compoundFields;
        compoundFields = &m_compoundDeclarations[node.name];
    }

    RuntimeValue value =
        node.initValue ? evalExpr(*node.initValue)
                       : (compoundFields ? defaultCompoundValue(*compoundFields)
                                         : defaultValueForType(node.type));
    if (node.type.empty()) {
        node.type = value.declaredType();
    }
    value = coerceDeclaredValue(
        std::move(value),
        node.type,
        compoundFields,
        locationOf(node)
    );
    value.setDeclaredType(node.type);
    m_currentEnv->define(
        node.name,
        RuntimeSlot(std::move(value), node.type, StorageClass::MainStack)
    );
    return {};
}

ASTInterpreter::ControlFlow ASTInterpreter::execArrayDecl(ArrayDeclNode& node)
{
    ensureCurrentEnvironment();

    RuntimeValue value;
    if (node.initArray) {
        value = evalArray(*node.initArray);
    } else {
        int64_t size = 0;
        if (node.sizeExpr) {
            size = evalExpr(*node.sizeExpr).toScriptNum();
            if (size < 0) {
                throw RuntimeError(
                    RuntimeErrorKind::InvalidIndexAccess,
                    "array size cannot be negative",
                    locationOf(*node.sizeExpr)
                );
            }
        }

        RuntimeValue::Array values;
        values.reserve(static_cast<size_t>(size));
        for (int64_t i = 0; i < size; ++i) {
            values.push_back(defaultValueForType(node.elementType));
        }
        value = RuntimeValue::fromArray(std::move(values), node.elementType);
    }

    const std::string declaredType =
        node.elementType.empty() ? "array" : node.elementType + "[]";
    value.setDeclaredType(declaredType);
    m_currentEnv->define(
        node.name,
        RuntimeSlot(std::move(value), declaredType, StorageClass::MainStack)
    );
    return {};
}

ASTInterpreter::ControlFlow ASTInterpreter::execExprStmt(ExprStmtNode& node)
{
    if (node.expr) {
        (void)evalExpr(*node.expr);
    }
    return {};
}

ASTInterpreter::ControlFlow ASTInterpreter::execReturn(ReturnNode& node)
{
    ControlFlow flow;
    flow.kind = FlowKind::Return;
    if (!node.expr) {
        return flow;
    }

    if (node.isValueReturn) {
        if (auto* identifier = dynamic_cast<IdentifierNode*>(node.expr.get())) {
            flow.values.push_back(evalIdentifier(*identifier));
            return flow;
        }

        if (auto* brace = dynamic_cast<BraceExprNode*>(node.expr.get())) {
            flow.values.reserve(brace->elements.size());
            for (auto& element : brace->elements) {
                auto* identifier =
                    dynamic_cast<IdentifierNode*>(element.get());
                if (!identifier) {
                    throw RuntimeError(
                        RuntimeErrorKind::Generic,
                        "lowercase return brace expression only accepts "
                        "existing variable names",
                        locationOf(node),
                        "Use uppercase Return(...) to compute return values"
                    );
                }
                flow.values.push_back(evalIdentifier(*identifier));
            }
            return flow;
        }

        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "lowercase return only accepts an existing variable or a brace "
            "list of variables",
            locationOf(node),
            "Use uppercase Return(...) to compute a value"
        );
    }

    if (auto* brace = dynamic_cast<BraceExprNode*>(node.expr.get())) {
        flow.values.reserve(brace->elements.size());
        for (auto& element : brace->elements) {
            flow.values.push_back(evalExpr(*element));
        }
    } else {
        flow.values.push_back(evalExpr(*node.expr));
    }
    return flow;
}

ASTInterpreter::ControlFlow
ASTInterpreter::execDestructureAssign(DestructureAssignNode& node)
{
    ensureCurrentEnvironment();

    if (!node.value) {
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "destructure assignment has no value expression",
            locationOf(node)
        );
    }

    RuntimeValue value = evalExpr(*node.value);
    std::vector<RuntimeValue> values;
    if (value.type() == RuntimeType::Array) {
        values = value.array();
    } else if (node.targets.size() == 1) {
        values.push_back(std::move(value));
    } else {
        throw RuntimeError(
            RuntimeErrorKind::TypeMismatch,
            "destructure assignment expects an array or multi-value return",
            locationOf(node)
        );
    }

    if (values.size() != node.targets.size()) {
        throw RuntimeError(
            RuntimeErrorKind::TypeMismatch,
            "destructure assignment target count " +
                std::to_string(node.targets.size()) +
                " does not match value count " +
                std::to_string(values.size()),
            locationOf(node)
        );
    }

    for (size_t i = 0; i < node.targets.size(); ++i) {
        if (node.targets[i].empty()) {
            throw RuntimeError(
                RuntimeErrorKind::UndefinedVariable,
                "destructure assignment target cannot be empty",
                locationOf(node)
            );
        }
        IdentifierNode target(node.targets[i]);
        assignIdentifier(target, std::move(values[i]), locationOf(node));
    }

    return {};
}

std::vector<RuntimeValue> ASTInterpreter::callFunction(
    FunctionNode& function,
    const std::vector<RuntimeValue>& args,
    SourceLocation callLocation
)
{
    if (!function.block) {
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "function '" + function.name + "' has no body",
            function.sourceLocation
        );
    }
    if (args.size() > function.parameters.size()) {
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "too many arguments for function '" + function.name + "'",
            callLocation
        );
    }

    std::shared_ptr<Environment> previousEnv = m_currentEnv;
    std::shared_ptr<Environment> parentEnv =
        previousEnv ? previousEnv : m_context.globalEnvironment();
    std::shared_ptr<Environment> functionEnv =
        parentEnv->createChild(function.name);

    bool framePushed = false;
    m_currentEnv = functionEnv;

    try {
        for (size_t i = 0; i < function.parameters.size(); ++i) {
            const auto& param = function.parameters[i];
            RuntimeValue value =
                i < args.size() ? args[i] : defaultValueForType(param.type);
            value = coerceDeclaredValue(
                std::move(value),
                param.type,
                nullptr,
                callLocation
            );
            if (!param.type.empty()) {
                value.setDeclaredType(param.type);
            }
            const std::string declaredType =
                param.type.empty() ? value.declaredType() : param.type;
            functionEnv->define(
                param.name,
                RuntimeSlot(
                    std::move(value),
                    declaredType,
                    StorageClass::MainStack
                )
            );
        }

        RuntimeCallFrame frame;
        frame.functionName = function.name;
        frame.environment = functionEnv;
        frame.callLocation = callLocation;
        m_context.pushFrame(std::move(frame));
        framePushed = true;

        ControlFlow flow = execBlock(*function.block, false);
        if (framePushed) {
            m_context.popFrame();
            framePushed = false;
        }
        m_currentEnv = previousEnv;

        if (flow.kind == FlowKind::Return) {
            return std::move(flow.values);
        }
        return {};
    } catch (...) {
        if (framePushed) {
            m_context.popFrame();
        }
        m_currentEnv = previousEnv;
        throw;
    }
}

std::vector<int64_t> ASTInterpreter::evalRange(CallNode& node)
{
    if (node.args.empty() || node.args.size() > 3) {
        throw RuntimeError(
            RuntimeErrorKind::BuiltinError,
            "Range(...) expects 1 to 3 arguments",
            locationOf(node)
        );
    }

    std::vector<int64_t> bounds;
    bounds.reserve(node.args.size());
    for (auto& arg : node.args) {
        bounds.push_back(evalExpr(*arg).toScriptNum());
    }

    int64_t start = 0;
    int64_t stop = 0;
    int64_t step = 1;
    if (bounds.size() == 1) {
        stop = bounds[0];
    } else if (bounds.size() == 2) {
        start = bounds[0];
        stop = bounds[1];
    } else {
        start = bounds[0];
        stop = bounds[1];
        step = bounds[2];
    }

    if (step == 0) {
        throw RuntimeError(
            RuntimeErrorKind::BuiltinError,
            "Range(...) step cannot be zero",
            locationOf(node)
        );
    }

    std::vector<int64_t> values;
    constexpr size_t kMaxRangeItems = 1000000;
    if (step > 0) {
        for (int64_t value = start; value < stop; value += step) {
            values.push_back(value);
            if (values.size() > kMaxRangeItems) {
                throw RuntimeError(
                    RuntimeErrorKind::BuiltinError,
                    "Range(...) produced too many values",
                    locationOf(node)
                );
            }
        }
    } else {
        for (int64_t value = start; value > stop; value += step) {
            values.push_back(value);
            if (values.size() > kMaxRangeItems) {
                throw RuntimeError(
                    RuntimeErrorKind::BuiltinError,
                    "Range(...) produced too many values",
                    locationOf(node)
                );
            }
        }
    }

    return values;
}

RuntimeValue ASTInterpreter::firstOrAggregateReturn(
    std::vector<RuntimeValue> values,
    const std::string& functionName
) const
{
    if (values.empty()) {
        return RuntimeValue::voidValue();
    }
    if (values.size() == 1) {
        return std::move(values.front());
    }
    return RuntimeValue::fromArray(
        std::move(values),
        functionName.empty() ? "return[]" : functionName + "_return[]"
    );
}

RuntimeValue ASTInterpreter::coerceDeclaredValue(
    RuntimeValue value,
    const std::string& declaredType,
    const std::vector<CompoundFieldInfo>* compoundFields,
    SourceLocation location
)
{
    if (compoundFields) {
        if (value.type() == RuntimeType::Struct) {
            value.setDeclaredType(declaredType.empty() ? "__compound__" : declaredType);
            return value;
        }
        return unpackCompoundValue(value, *compoundFields, location);
    }

    if (!declaredType.empty()) {
        if (StructDefNode* structDef = m_context.findStruct(declaredType)) {
            if (value.isVoid()) {
                return defaultStructValue(*structDef);
            }
            if (value.type() == RuntimeType::Struct) {
                if (value.structFields().empty()) {
                    return defaultStructValue(*structDef);
                }
                value.setDeclaredType(declaredType);
                return value;
            }
            return unpackStructValue(value, *structDef, location);
        }
    }

    return value;
}

RuntimeValue ASTInterpreter::unpackCompoundValue(
    const RuntimeValue& value,
    const std::vector<CompoundFieldInfo>& fields,
    SourceLocation location
) const
{
    const std::vector<uint8_t> bytes = value.toScriptBytes();
    RuntimeValue::Struct result;

    size_t offset = 0;
    for (const auto& field : fields) {
        if (field.byteSize == 0) {
            throw RuntimeError(
                RuntimeErrorKind::TypeMismatch,
                "compound field '" + field.name + "' has no fixed byte size",
                location
            );
        }

        auto fieldBytes = checkedSlice(
            bytes,
            offset,
            field.byteSize,
            field.name,
            location
        );
        offset += field.byteSize;
        result[field.name] =
            RuntimeValue::fromBytes(std::move(fieldBytes), field.type);
    }

    return RuntimeValue::fromStruct(std::move(result), "__compound__");
}

RuntimeValue ASTInterpreter::defaultCompoundValue(
    const std::vector<CompoundFieldInfo>& fields
) const
{
    return defaultRuntimeCompoundValue(fields);
}

RuntimeValue ASTInterpreter::unpackStructValue(
    const RuntimeValue& value,
    StructDefNode& structDef,
    SourceLocation location
) const
{
    const std::vector<uint8_t> bytes = value.toScriptBytes();
    RuntimeValue::Struct result;

    size_t offset = 0;
    for (const auto& [fieldName, fieldType] : structDef.fields) {
        if (fieldType.isCompoundType) {
            const size_t size = fieldType.getTotalByteSize();
            auto fieldBytes =
                checkedSlice(bytes, offset, size, fieldName, location);
            offset += size;
            result[fieldName] = unpackCompoundValue(
                RuntimeValue::fromBytes(std::move(fieldBytes), "__compound__"),
                fieldType.compoundFields,
                location
            );
            continue;
        }

        if (fieldType.isArray) {
            const size_t size = fieldType.getTotalByteSize();
            if (size == 0) {
                throw RuntimeError(
                    RuntimeErrorKind::TypeMismatch,
                    "struct field '" + fieldName +
                        "' has unsupported variable-width array type",
                    location
                );
            }
            auto fieldBytes =
                checkedSlice(bytes, offset, size, fieldName, location);
            offset += size;
            result[fieldName] =
                RuntimeValue::fromBytes(std::move(fieldBytes), fieldType.getTypeString());
            continue;
        }

        if (StructDefNode* nested = m_context.findStruct(fieldType.baseType)) {
            throw RuntimeError(
                RuntimeErrorKind::TypeMismatch,
                "nested struct field '" + fieldName +
                    "' cannot be unpacked from bytes without fixed-size fields",
                location
            );
            (void)nested;
        }

        auto width = fixedByteSizeForType(fieldType.baseType);
        if (!width.has_value()) {
            throw RuntimeError(
                RuntimeErrorKind::TypeMismatch,
                "struct field '" + fieldName +
                    "' has unsupported variable-width type '" +
                    fieldType.baseType + "'",
                location
            );
        }

        auto fieldBytes =
            checkedSlice(bytes, offset, *width, fieldName, location);
        offset += *width;
        result[fieldName] =
            RuntimeValue::fromBytes(std::move(fieldBytes), fieldType.baseType);
    }

    return RuntimeValue::fromStruct(std::move(result), structDef.name);
}

RuntimeValue ASTInterpreter::defaultStructValue(StructDefNode& structDef) const
{
    if (ContractNode* contract = m_context.contract()) {
        return defaultRuntimeValueForType(structDef.name, *contract);
    }

    RuntimeValue::Struct result;
    for (const auto& [fieldName, fieldType] : structDef.fields) {
        result[fieldName] = defaultValueForStructField(fieldType);
    }
    return RuntimeValue::fromStruct(std::move(result), structDef.name);
}

RuntimeValue ASTInterpreter::defaultValueForStructField(
    const StructFieldType& fieldType
) const
{
    if (ContractNode* contract = m_context.contract()) {
        return defaultRuntimeValueForFieldType(fieldType, *contract);
    }

    if (fieldType.isCompoundType) {
        return defaultCompoundValue(fieldType.compoundFields);
    }

    if (fieldType.isArray) {
        RuntimeValue::Array values;
        values.reserve(fieldType.arraySize);
        for (size_t i = 0; i < fieldType.arraySize; ++i) {
            values.push_back(defaultValueForType(fieldType.baseType));
        }
        return RuntimeValue::fromArray(
            std::move(values),
            fieldType.baseType + "[]"
        );
    }

    return defaultValueForType(fieldType.baseType);
}

std::string ASTInterpreter::chooseFunctionName(
    ContractNode& contract,
    const std::string& requestedName
) const
{
    const std::string selected =
        function_selection::chooseASTFunctionName(contract, requestedName);
    if (!selected.empty()) {
        return selected;
    }

    throw RuntimeError(
        RuntimeErrorKind::UndefinedVariable,
        "no public function found for AST interpretation",
        contract.sourceLocation
    );
}

RuntimeValue
ASTInterpreter::defaultValueForType(const std::string& typeName) const
{
    if (ContractNode* contract = m_context.contract()) {
        return defaultRuntimeValueForType(typeName, *contract);
    }

    if (StructDefNode* structDef = m_context.findStruct(typeName)) {
        return defaultStructValue(*structDef);
    }
    if (isRuntimeBoolType(typeName)) {
        return RuntimeValue::fromBool(false);
    }
    if (isRuntimeHexType(typeName)) {
        return RuntimeValue::fromHexString("0x", typeName);
    }
    if (normalizeRuntimeName(typeName) == "string") {
        return RuntimeValue::fromString("");
    }
    if (normalizeRuntimeName(typeName) == "address") {
        return RuntimeValue::fromAddress("");
    }
    if (isRuntimeIntegerType(typeName) || typeName.empty()) {
        return RuntimeValue::fromInt(0, typeName.empty() ? "int" : typeName);
    }
    return RuntimeValue::fromStruct({}, typeName);
}

void ASTInterpreter::ensureCurrentEnvironment() const
{
    if (!m_currentEnv) {
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "AST interpreter has no active environment"
        );
    }
}

SourceLocation ASTInterpreter::locationOf(const ASTNode& node) const
{
    if (node.hasSourceLocation()) {
        return node.sourceLocation;
    }
    return SourceLocation("", node.pos.first, node.pos.second);
}

} // namespace apc_interpreter
