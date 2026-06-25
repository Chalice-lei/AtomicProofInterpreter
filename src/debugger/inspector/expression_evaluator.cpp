#include "expression_evaluator.h"
#include <algorithm>
#include <cctype>

namespace apc_debug {

ExpressionEvaluator::ExpressionEvaluator(
    std::shared_ptr<VariableInspector> varInspector,
    std::shared_ptr<ScopeInspector> scopeInspector
) : m_varInspector(varInspector), m_scopeInspector(scopeInspector) {
}

EvaluationResult ExpressionEvaluator::evaluate(
    const std::string& expression,
    const StackState& stack,
    size_t currentPC
) {
    std::string expr = trim(expression);
    
    if (expr.empty()) {
        return EvaluationResult::error("空表达式");
    }

    auto exprType = getExpressionType(expr);
    
    switch (exprType) {
        case ExprType::Variable:
            return evaluateVariable(expr, stack, currentPC);
            
        case ExprType::Literal:
            return evaluateLiteral(expr);
            
        case ExprType::StackAccess:
            return evaluateStackAccess(expr, stack);
            
        case ExprType::BinaryOp: {
            auto parts = parseBinaryOp(expr);
            if (!parts) {
                return EvaluationResult::error("无法解析二元运算");
            }
            auto leftResult = evaluate(parts->left, stack, currentPC);
            if (!leftResult.success) {
                return leftResult;
            }
            auto rightResult = evaluate(parts->right, stack, currentPC);
            if (!rightResult.success) {
                return rightResult;
            }
            return evaluateBinaryOp(parts->op, leftResult, rightResult);
        }
            
        default:
            return EvaluationResult::error("不支持的表达式类型");
    }
}

EvaluationResult ExpressionEvaluator::evaluateVariable(
    const std::string& varName,
    const StackState& stack,
    size_t currentPC
) {
    auto varValue = m_varInspector->readVariable(varName, stack, currentPC);
    
    if (!varValue) {
        return EvaluationResult::error("变量 '" + varName + "' 未找到");
    }
    
    if (!varValue->isValid) {
        return EvaluationResult::error("变量 '" + varName + "' 无效");
    }
    
    return EvaluationResult(varValue->value, varValue->type);
}

EvaluationResult ExpressionEvaluator::evaluateStackAccess(
    const std::string& expression,
    const StackState& stack
) {
    // 支持 stack[N] 和 stack.top
    std::string expr = trim(expression);

    if (expr == "stack.top") {
        if (stack.empty()) {
            return EvaluationResult::error("栈为空");
        }
        const auto& element = stack.peek(0);
        return EvaluationResult(element.toHexString(true), "bytes");
    }

    size_t bracketPos = expr.find('[');
    if (bracketPos == std::string::npos) {
        return EvaluationResult::error("无效的栈访问语法");
    }
    
    size_t closeBracketPos = expr.find(']', bracketPos);
    if (closeBracketPos == std::string::npos) {
        return EvaluationResult::error("缺少闭合括号");
    }
    
    std::string indexStr = expr.substr(bracketPos + 1, closeBracketPos - bracketPos - 1);
    auto indexOpt = toInt(trim(indexStr));
    if (!indexOpt) {
        return EvaluationResult::error("无效的索引: " + indexStr);
    }
    
    int index = static_cast<int>(*indexOpt);
    if (index < 0 || index >= static_cast<int>(stack.size())) {
        return EvaluationResult::error("索引越界: " + std::to_string(index));
    }
    
    const auto& element = stack.peek(index);
    return EvaluationResult(element.toHexString(true), "bytes");
}

EvaluationResult ExpressionEvaluator::evaluateLiteral(const std::string& literal) {
    std::string lit = trim(literal);

    if (isNumberLiteral(lit)) {
        return EvaluationResult(lit, "int");
    }
    if (isBooleanLiteral(lit)) {
        return EvaluationResult(lit, "bool");
    }
    if (isStringLiteral(lit)) {
        std::string value = lit.substr(1, lit.length() - 2);
        return EvaluationResult(value, "string");
    }

    return EvaluationResult::error("无法识别的字面量: " + lit);
}

EvaluationResult ExpressionEvaluator::evaluateBinaryOp(
    const std::string& op,
    const EvaluationResult& left,
    const EvaluationResult& right
) {
    auto leftInt = toInt(left.value);
    auto rightInt = toInt(right.value);

    if (!leftInt || !rightInt) {
        return EvaluationResult::error("无法将操作数转换为整数");
    }

    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        return performArithmetic(op, *leftInt, *rightInt);
    }
    if (op == "==" || op == "!=" || op == ">" || op == "<" ||
        op == ">=" || op == "<=") {
        return performComparison(op, *leftInt, *rightInt);
    }

    return EvaluationResult::error("不支持的运算符: " + op);
}

ExpressionEvaluator::ExprType ExpressionEvaluator::getExpressionType(
    const std::string& expr
) {
    std::string e = trim(expr);

    if (isLiteral(e)) {
        return ExprType::Literal;
    }
    if (isStackAccess(e)) {
        return ExprType::StackAccess;
    }
    if (parseBinaryOp(e)) {
        return ExprType::BinaryOp;
    }
    return ExprType::Variable;
}

std::optional<ExpressionEvaluator::BinaryOpParts> ExpressionEvaluator::parseBinaryOp(
    const std::string& expr
) {
    static const std::vector<std::string> operators = {
        "==", "!=", ">=", "<=", ">", "<",
        "+", "-", "*", "/", "%"
    };

    for (const auto& op : operators) {
        size_t pos = expr.find(op);
        if (pos != std::string::npos && pos > 0 && pos < expr.length() - op.length()) {
            BinaryOpParts parts;
            parts.left = trim(expr.substr(0, pos));
            parts.op = op;
            parts.right = trim(expr.substr(pos + op.length()));

            if (!parts.left.empty() && !parts.right.empty()) {
                return parts;
            }
        }
    }

    return std::nullopt;
}

std::string ExpressionEvaluator::trim(const std::string& str) {
    size_t start = 0;
    size_t end = str.length();
    
    while (start < end && std::isspace(static_cast<unsigned char>(str[start]))) {
        ++start;
    }
    
    while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        --end;
    }
    
    return str.substr(start, end - start);
}

bool ExpressionEvaluator::isLiteral(const std::string& expr) {
    return isNumberLiteral(expr) || isStringLiteral(expr) || isBooleanLiteral(expr);
}

bool ExpressionEvaluator::isNumberLiteral(const std::string& expr) {
    if (expr.empty()) {
        return false;
    }
    
    size_t start = 0;
    if (expr[0] == '-' || expr[0] == '+') {
        start = 1;
    }
    
    if (start >= expr.length()) {
        return false;
    }
    
    return std::all_of(expr.begin() + start, expr.end(), ::isdigit);
}

bool ExpressionEvaluator::isStringLiteral(const std::string& expr) {
    return expr.length() >= 2 && 
           expr.front() == '"' && 
           expr.back() == '"';
}

bool ExpressionEvaluator::isBooleanLiteral(const std::string& expr) {
    return expr == "true" || expr == "false";
}

bool ExpressionEvaluator::isStackAccess(const std::string& expr) {
    return expr.find("stack[") == 0 || expr == "stack.top";
}

EvaluationResult ExpressionEvaluator::performArithmetic(
    const std::string& op,
    int64_t left,
    int64_t right
) {
    int64_t result;
    
    if (op == "+") {
        result = left + right;
    } else if (op == "-") {
        result = left - right;
    } else if (op == "*") {
        result = left * right;
    } else if (op == "/") {
        if (right == 0) {
            return EvaluationResult::error("除以零");
        }
        result = left / right;
    } else if (op == "%") {
        if (right == 0) {
            return EvaluationResult::error("除以零");
        }
        result = left % right;
    } else {
        return EvaluationResult::error("未知的算术运算符: " + op);
    }
    
    return EvaluationResult(std::to_string(result), "int");
}

EvaluationResult ExpressionEvaluator::performComparison(
    const std::string& op,
    int64_t left,
    int64_t right
) {
    bool result;
    
    if (op == "==") {
        result = (left == right);
    } else if (op == "!=") {
        result = (left != right);
    } else if (op == ">") {
        result = (left > right);
    } else if (op == "<") {
        result = (left < right);
    } else if (op == ">=") {
        result = (left >= right);
    } else if (op == "<=") {
        result = (left <= right);
    } else {
        return EvaluationResult::error("未知的比较运算符: " + op);
    }
    
    return EvaluationResult(result ? "true" : "false", "bool");
}

std::optional<int64_t> ExpressionEvaluator::toInt(const std::string& str) {
    try {
        if (str.size() >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            return std::stoll(str, nullptr, 16);
        }
        return std::stoll(str);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace apc_debug



