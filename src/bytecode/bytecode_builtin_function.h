#ifndef BYTCODE_BUILTIN_FUNCTION_H
#define BYTCODE_BUILTIN_FUNCTION_H

#include <algorithm>
#include <climits>
#include <functional>
#include <iomanip>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../util/op_stack.h" // StackElement
#include "byt_data_types.h"
#include "byt_defs.h"
#include "bytecode_base_function.h"
#include "bytecode_helper_fun.h"
#include "bytecode_opcodes.h"
#include "scope.h"

SPACE_TBC_START

// 内置函数基类 (类型由操作码上下文决定)
class BuiltinFunction : public BytecodeFunction
{
public:
    virtual ~BuiltinFunction() = default;

    virtual std::string getOpcodeHex(
        const StackElement& objElement,
        const std::vector<StackElement>& stackArgs,
        std::shared_ptr<Scope> = nullptr
    ) const = 0;

    static std::vector<StackElement>
    extractArgsFromStack(std::shared_ptr<Scope> scopePtr, size_t argCount)
    {
        std::vector<StackElement> args;
        args.reserve(argCount);

        for (size_t i = 0; i < argCount; ++i) {
            if (scopePtr->empty()) {
                LOG_ERROR("Insufficient scopePtr elements for builtin function"
                );
                throw std::runtime_error(
                    "Insufficient scopePtr elements for builtin function"
                );
            }
            args.push_back(scopePtr->top());
            scopePtr->pop();
        }

        // 反转: 第一个参数位于 args[0]
        std::reverse(args.begin(), args.end());
        return args;
    }
};

/// @brief 可变参数内置函数基类 (用于 Delete、keep 等)
template <typename FuncType>
class VariableArgBuiltinFunction : public BuiltinFunction
{
private:
    size_t expectedArgCount;
    FuncType opcodeFunc;
    size_t returnCount;
    std::vector<tbc::OpType> returnTypes;
    std::vector<tbc::OpType> inputTypes;

public:
    VariableArgBuiltinFunction(
        size_t argCount,
        FuncType funcImpl,
        size_t retCount = 0,
        const std::vector<tbc::OpType>& retTypes = {},
        const std::vector<tbc::OpType>& inTypes = {}
    )
        : expectedArgCount(argCount), opcodeFunc(funcImpl),
          returnCount(retCount), returnTypes(retTypes), inputTypes(inTypes)
    {}

    std::string getOpcodeHex(
        const StackElement& objElement,
        const std::vector<StackElement>& stackArgs,
        std::shared_ptr<Scope> scopePtr = nullptr
    ) const override
    {
        return opcodeFunc(objElement, stackArgs, scopePtr);
    }

    size_t getExpectedArgCount() const override
    {
        return expectedArgCount;
    }

    size_t getReturnCount() const override
    {
        return returnCount;
    }

    std::vector<tbc::OpType> getReturnTypes() const override
    {
        return returnTypes;
    }

    std::vector<tbc::OpType> getInputTypes() const override
    {
        return inputTypes;
    }
};

/// @brief 定参函数模板
template <size_t argCount, typename FuncType, size_t returnCount = 0>
class BuiltinFunctionTemplate : public BuiltinFunction
{
public:
private:
    FuncType opcodeFunc;
    std::vector<tbc::OpType> returnTypes;
    std::vector<tbc::OpType> inputTypes;

public:
    BuiltinFunctionTemplate(
        FuncType func,
        const std::vector<tbc::OpType>& retTypes = {},
        const std::vector<tbc::OpType>& inTypes = {}
    )
        : opcodeFunc(func), returnTypes(retTypes), inputTypes(inTypes)
    {}

    std::string getOpcodeHex(
        const StackElement& objElement,
        const std::vector<StackElement>& stackArgs,
        std::shared_ptr<Scope> scopePtr = nullptr
    ) const override
    {
        return opcodeFunc(objElement, stackArgs, scopePtr);
    }

    size_t getExpectedArgCount() const override
    {
        return argCount;
    }

    size_t getReturnCount() const override
    {
        return returnCount;
    }

    std::vector<tbc::OpType> getReturnTypes() const override
    {
        return returnTypes;
    }

    std::vector<tbc::OpType> getInputTypes() const override
    {
        return inputTypes;
    }
};

/// @brief 内置函数创建器 (Bitcoin 风格的上下文类型系统)
static const std::unordered_map<
    std::string,
    std::shared_ptr<BuiltinFunction> (*)(size_t)>
    buildinCreators = {
        {"SetAlt",
         [](size_t size) -> std::shared_ptr<BuiltinFunction> {
             if (1 != size) {
                 return nullptr;
             }
             auto func = [size](
                             const StackElement& /*objElement*/,
                             const std::vector<StackElement>& stackArgs,
                             std::shared_ptr<Scope> stackPtr
                         ) -> std::string {
                 if (stackArgs.empty()) {
                     LOG_ERROR("SetAlt function execution failed: no arguments "
                               "provided, expected 1 argument");
                     throw std::runtime_error(
                         "SetAlt function execution failed: no arguments "
                         "provided, expected 1 argument"
                     );
                 }
                 const SymbolTable& symbolTable = stackPtr->getCurrentSymtab();
                 auto arg = stackArgs[0];
                 auto stackPosOpt = symbolTable.getPos(arg);
                 if (!stackPosOpt.has_value()) {
                     LOG_ERROR(
                         "SetAlt function execution failed: cannot find "
                         "variable '" +
                         stackArgs[0].getName() + "' in symbol table"
                     );
                     throw std::runtime_error(
                         "SetAlt function execution "
                         "failed: cannot find variable '" +
                         stackArgs[0].getName() + "' in symbol table"
                     );
                 }

                 std::ostringstream oss;
                 auto position = stackPosOpt.value();

                 // 将指定变量移至副栈：先把它转到主栈顶，再 OP_TOALTSTACK
                 if (position == 0) {
                     oss << opcodeToHex(tbc::BytOpcode::OP_TOALTSTACK);
                 } else if (position == 1) {
                     oss << opcodeToHex(tbc::BytOpcode::OP_SWAP);
                     oss << opcodeToHex(tbc::BytOpcode::OP_TOALTSTACK);
                 } else if (position > 1) {
                     // pos==2: OP_ROT (1B); pos>=3: N OP_ROLL (2B)
                     oss << rollToTopHex(position);
                     oss << opcodeToHex(tbc::BytOpcode::OP_TOALTSTACK);
                 } else {
                     LOG_ERROR(
                         "SetAlt function execution failed: variable '" +
                         stackArgs[0].getName() +
                         "' has invalid stack position (position=" +
                         std::to_string(position) + ")"
                     );
                     throw std::runtime_error("SetAlt function execution "
                                              "failed: invalid stack position");
                 }

                 return oss.str();
             };
             return std::make_shared<
                 BuiltinFunctionTemplate<1, decltype(func), 0>>(
                 func,
                 std::vector<tbc::OpType>{},
                 std::vector<tbc::OpType>{} // 不限制输入类型
             );
         }},
        {"SetMain",
         [](size_t size) -> std::shared_ptr<BuiltinFunction> {
             if (1 != size) {
                 return nullptr;
             }
             auto func = [size](
                             const StackElement& /*objElement*/,
                             const std::vector<StackElement>& stackArgs,
                             std::shared_ptr<Scope> stackPtr
                         ) -> std::string {
                 if (stackArgs.empty()) {
                     LOG_ERROR("SetMain function execution failed: no "
                               "arguments provided, expected 1 argument");
                     throw std::runtime_error(
                         "SetMain function execution failed: no arguments "
                         "provided, expected 1 argument"
                     );
                 }
                 const SymbolTable& symbolTable = stackPtr->getCurrentSymtab();
                 auto arg = stackArgs[0];
                 auto stackPosOpt = symbolTable.getPos(arg, true);
                 if (!stackPosOpt.has_value()) {
                     LOG_ERROR(
                         "SetMain function execution failed: cannot find "
                         "variable '" +
                         stackArgs[0].getName() +
                         "' in alternate stack symbol table"
                     );
                     throw std::runtime_error(
                         "SetMain function execution failed: cannot find "
                         "variable '" +
                         stackArgs[0].getName() +
                         "' in alternate stack symbol table"
                     );
                 }
                 std::ostringstream oss;
                 auto position = stackPosOpt.value();

                 // 把目标变量从副栈移回主栈：先连带其上方元素一起弹回主栈，
                 // 再 SWAP+TOALTSTACK 把上方元素按原顺序送回副栈，
                 // 最终主栈顶为目标变量，副栈其余元素保持不变
                 for (int i = 0; i <= position; i++) {
                     oss << opcodeToHex(tbc::BytOpcode::OP_FROMALTSTACK);
                 }
                 for (int i = 0; i < position; i++) {
                     oss << opcodeToHex(tbc::BytOpcode::OP_SWAP);
                     oss << opcodeToHex(tbc::BytOpcode::OP_TOALTSTACK);
                 }

                 return oss.str();
             };
             return std::make_shared<
                 BuiltinFunctionTemplate<1, decltype(func), 0>>(
                 func,
                 std::vector<tbc::OpType>{},
                 std::vector<tbc::OpType>{} // 不限制输入类型
             );
         }},
        {"Clone",
         [](size_t size) -> std::shared_ptr<BuiltinFunction> {
             if (0 != size) {
                 return nullptr;
             }
             // 根据对象在栈中的位置选择 clone 操作码
             auto func = [size](
                             const StackElement& objElement,
                             const std::vector<StackElement>& stackArgs,
                             std::shared_ptr<Scope> stackPtr
                         ) -> std::string {
                 if (size != stackArgs.size()) {
                     LOG_ERROR(
                         "Clone() function argument count mismatch: "
                         "expected 0 arguments, got " +
                         std::to_string(stackArgs.size()) + " arguments"
                     );
                     throw std::invalid_argument(
                         "Clone() function argument count mismatch: expected 0 "
                         "arguments, got " +
                         std::to_string(stackArgs.size()) + " arguments"
                     );
                 }

                 if (!stackPtr) {
                     LOG_ERROR(
                         "Clone() function execution failed: stack pointer is "
                         "null, cannot perform stack operations"
                     );
                     throw std::runtime_error(
                         "Clone() function execution failed: stack pointer is "
                         "null, cannot perform stack operations"
                     );
                 }

                 // 把 objElement 解析为栈位置索引
                 auto pos = stackPtr->getPos(objElement.getName());
                 if (!pos.has_value()) {
                     LOG_ERROR(
                         "Clone() function execution failed: cannot find "
                         "object '" +
                         objElement.getName() + "' in stack"
                     );
                     throw std::runtime_error(
                         "Clone() function execution "
                         "failed: cannot find object '" +
                         objElement.getName() + "' in stack"
                     );
                 }

                 auto position = pos.value();
                 std::string result;

                 if (position == 0) {
                     result = opcodeToHex(tbc::BytOpcode::OP_DUP);
                 } else if (position == 1) {
                     result = opcodeToHex(tbc::BytOpcode::OP_OVER);
                 } else if (position > 1) {
                     // OP_PICK 要求栈顶为索引
                     result += numberToScriptHex(position);
                     result += opcodeToHex(tbc::BytOpcode::OP_PICK);
                 } else {
                     LOG_ERROR(
                         "Clone() function execution failed: object '" +
                         objElement.getName() +
                         "' has invalid stack position (position=" +
                         std::to_string(position) + ")"
                     );
                     throw std::runtime_error(
                         "Clone() function execution failed: object '" +
                         objElement.getName() +
                         "' has invalid stack position (position=" +
                         std::to_string(position) + ")"
                     );
                 }

                 return result;
             };
             return std::make_shared<
                 BuiltinFunctionTemplate<0, decltype(func), 1>>(
                 func,
                 // 返回类型与原对象相同, 这里假设为 BYTES
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{}
             );
         }},
        {"Range",
         [](size_t size) -> std::shared_ptr<BuiltinFunction> {
             if (size < 1 || size > 3) {
                 return nullptr;
             }

             auto func = [size](
                             const StackElement& /*objElement*/,
                             const std::vector<StackElement>& stackArgs,
                             std::shared_ptr<Scope> scopePtr
                         ) -> std::string {
                 if (!scopePtr) {
                     LOG_ERROR("Range() function execution failed: scope "
                               "pointer is null");
                     throw std::runtime_error("Range() function execution "
                                              "failed: scope pointer is null");
                 }

                 if (stackArgs.size() != size) {
                     LOG_ERROR(
                         "Range() function argument count mismatch: expected " +
                         std::to_string(size) + " arguments, got " +
                         std::to_string(stackArgs.size())
                     );
                     throw std::invalid_argument(
                         "Range() function argument count mismatch"
                     );
                 }

                 auto parseNumericArg = [](const StackElement& element,
                                           const std::string& label
                                        ) -> int64_t {
                     std::string raw = element.getData();
                     if (raw.empty()) {
                         raw = element.getName();
                     }
                     if (raw.empty()) {
                         throw std::runtime_error(
                             "Range() '" + label +
                             "' argument is empty; only integer literals are "
                             "supported"
                         );
                     }

                     try {
                         return scriptHexToNumber(raw);
                     } catch (const std::exception&) {
                         throw std::runtime_error(
                             "Range() '" + label +
                             "' argument must be an integer literal"
                         );
                     }
                 };

                 int64_t start = 0;
                 int64_t stop = 0;
                 int64_t step = 1;

                 switch (size) {
                     case 1:
                         stop = parseNumericArg(stackArgs[0], "stop");
                         break;
                     case 2:
                         start = parseNumericArg(stackArgs[0], "start");
                         stop = parseNumericArg(stackArgs[1], "stop");
                         break;
                     case 3:
                         start = parseNumericArg(stackArgs[0], "start");
                         stop = parseNumericArg(stackArgs[1], "stop");
                         step = parseNumericArg(stackArgs[2], "step");
                         break;
                 }

                 if (step == 0) {
                     LOG_ERROR("Range() step argument must not be zero");
                     throw std::runtime_error(
                         "Range() step argument must not be zero"
                     );
                 }

                 static size_t rangeCounter = 0;
                 const std::string label = "__range_iter_" +
                                           std::to_string(rangeCounter++);

                 std::ostringstream metadataStream;
                 metadataStream << "range:" << start << ":" << stop << ":"
                                << step;
                 const std::string metadata = metadataStream.str();

                 std::vector<uint8_t> encoded(metadata.begin(), metadata.end());
                 std::string pushHex = encodePushData(encoded);
                 StackElement rangeElement(label, "range_iter", metadata);
                 scopePtr->push(rangeElement);

                 return "0x" + pushHex;
             };

             return std::make_shared<VariableArgBuiltinFunction<decltype(func
             )>>(size,
                 func,
                 1,
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>(size, tbc::OpType::INTEGER));
         }},
        {"Slice",
         [](size_t size) -> std::shared_ptr<BuiltinFunction> {
             if (2 != size) {
                 return nullptr;
             }
             auto func = [size](
                             const StackElement& objElement,
                             const std::vector<StackElement>& stackArgs,
                             std::shared_ptr<Scope> stackPtr
                         ) -> std::string {
                 if (size != stackArgs.size()) {
                     LOG_ERROR(
                         "Slice() function argument count mismatch: expected 2 "
                         "arguments, got " +
                         std::to_string(stackArgs.size()) + " arguments"
                     );
                     throw std::invalid_argument(
                         "Slice() function argument count mismatch: expected 2 "
                         "arguments, got " +
                         std::to_string(stackArgs.size()) + " arguments"
                     );
                 }

                 if (!stackPtr) {
                     LOG_ERROR(
                         "Slice() function execution failed: stack pointer is "
                         "null, cannot perform stack operations"
                     );
                     throw std::runtime_error(
                         "Slice() function execution failed: stack pointer is "
                         "null, cannot perform stack operations"
                     );
                 }

                 std::string result;

                 try {
                     auto startArg = stackArgs[0].getName();
                     auto endArg = stackArgs[1].getName();

                     auto objPos = stackPtr->getPos(objElement.getName());
                     if (!objPos.has_value()) {
                         LOG_ERROR(
                             "Slice() function execution failed: cannot find "
                             "object '" +
                             objElement.getName() + "' in stack"
                         );
                         throw std::runtime_error(
                             "Slice() function execution failed: cannot find "
                             "object '" +
                             objElement.getName() + "' in stack"
                         );
                     }

                     // 把对象移到栈顶 (消耗所有权)
                     auto position = objPos.value();
                     if (position < 0) {
                         LOG_ERROR(
                             "Slice() function execution failed: object '" +
                             objElement.getName() +
                             "' has invalid stack position (position=" +
                             std::to_string(position) + ")"
                         );
                         throw std::runtime_error(
                             "Slice() function execution failed: object '" +
                             objElement.getName() +
                             "' has invalid stack position (position=" +
                             std::to_string(position) + ")"
                         );
                     } else if (position == 0) {
                         // 已在栈顶
                     } else if (position == 1) {
                         result += opcodeToHex(tbc::BytOpcode::OP_SWAP);
                    } else {
                        // pos==2: OP_ROT (1B); pos>=3: N OP_ROLL (2B)
                        result += rollToTopHex(position);
                    }

                     std::string startHex = stackArgs[0].getData();
                     std::string endHex = stackArgs[1].getData();

                     int64_t startValue = 0;
                     int64_t endValue = 0;

                     // Bitcoin Script 数值解析规则
                     try {
                         startValue = scriptHexToNumber(startHex);
                     } catch (...) {
                         startValue = 0;
                     }

                     try {
                         endValue = scriptHexToNumber(endHex);
                     } catch (...) {
                         endValue = 0;
                     }

                     // -1 表示边界, 0 表示从开始
                     bool startIsFromBeginning =
                         (startValue == -1 || startValue == 0);
                     bool endIsToEnd = (endValue == -1);

                     if (startIsFromBeginning && !endIsToEnd) {
                         // slice(-1, n) = data[0:n]: 在 n 处 SPLIT 丢右
                         result += hexData(endHex);
                         result += opcodeToHex(tbc::BytOpcode::OP_SPLIT);
                         result += opcodeToHex(tbc::BytOpcode::OP_DROP);

                     } else if (!startIsFromBeginning && endIsToEnd) {
                         // slice(n, -1) = data[n:]: 在 n 处 SPLIT 丢左
                         result += hexData(startHex);
                         result += opcodeToHex(tbc::BytOpcode::OP_SPLIT);
                         result += opcodeToHex(tbc::BytOpcode::OP_NIP);

                     } else if (!startIsFromBeginning && !endIsToEnd) {
                         // slice(start, length): a[start:start+length]
                         // 在 start 处 SPLIT 丢左, 再在 length 处 SPLIT 丢右
                         result += hexData(startHex);
                         result += opcodeToHex(tbc::BytOpcode::OP_SPLIT);
                         result += opcodeToHex(tbc::BytOpcode::OP_NIP);

                         result += hexData(endHex);
                         result += opcodeToHex(tbc::BytOpcode::OP_SPLIT);
                         result += opcodeToHex(tbc::BytOpcode::OP_DROP);

                     } else {
                         // Slice(-1, -1) / Slice(0, -1) 不支持
                         LOG_ERROR("Slice() function: unsupported mode - both "
                                   "parameters cannot be boundary values");
                         throw std::invalid_argument(
                             "Slice() function: unsupported mode - both "
                             "parameters cannot be boundary values"
                         );
                     }

                 } catch (const std::invalid_argument& e) {
                     throw;
                 } catch (const std::runtime_error& e) {
                     throw;
                 } catch (const std::exception& e) {
                     LOG_ERROR(
                         "Slice() function execution "
                         "encountered unknown error: " +
                         std::string(e.what())
                     );
                     throw std::runtime_error(
                         "Slice() function execution "
                         "encountered unknown error: " +
                         std::string(e.what())
                     );
                 }

                 return result;
             };
             return std::make_shared<
                 BuiltinFunctionTemplate<2, decltype(func), 1>>(
                 func,
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{
                     tbc::OpType::INTEGER, tbc::OpType::INTEGER
                 }
                 // 模式: slice(start,length) / slice(-1,end) / slice(start,-1)
             );
         }},
        {"Delete",
         [](size_t size) -> std::shared_ptr<BuiltinFunction> {
             if (size == 0) {
                 return nullptr; // 至少需要 1 个参数
             }

             auto func = [size](
                             const StackElement& /*objElement*/,
                             const std::vector<StackElement>& stackArgs,
                             std::shared_ptr<Scope> stackPtr
                         ) -> std::string {
                 if (size != stackArgs.size()) {
                     LOG_ERROR(
                         "Delete() function argument count mismatch: "
                         "expected " +
                         std::to_string(size) + " arguments, got " +
                         std::to_string(stackArgs.size()) + " arguments"
                     );
                     throw std::invalid_argument(
                         "Delete() function argument count mismatch"
                     );
                 }

                 if (!stackPtr) {
                     LOG_ERROR("Delete() function execution failed: stack "
                               "pointer is null");
                     throw std::runtime_error("Delete() function execution "
                                              "failed: stack pointer is null");
                 }

                 // 去掉所有 0x 前缀
                 auto cleanHexString = [](const std::string& hexStr
                                       ) -> std::string {
                     std::string result = hexStr;
                     size_t pos = 0;
                     while ((pos = result.find("0x", pos)) != std::string::npos
                     ) {
                         result.erase(pos, 2);
                     }
                     return result;
                 };

                 // 删除副栈变量: 同时生成字节码并同步内部栈状态
                 auto processAltStackDeletion =
                     [&](const std::string& varName,
                         int64_t altPosition,
                         std::shared_ptr<Scope> scopePtr) -> std::string {
                     std::string deleteResult;

                     auto& currentSymtab = scopePtr->getCurrentSymtab();
                     auto& altStackPtr = currentSymtab.m_altStackPtr;

                     // 副栈栈顶: FROMALTSTACK + DROP
                     if (altPosition == 0) {
                         deleteResult += opcodeToHex(
                             tbc::BytOpcode::OP_FROMALTSTACK
                         );
                         deleteResult += opcodeToHex(tbc::BytOpcode::OP_DROP);

                         if (!altStackPtr->empty()) {
                             auto element = altStackPtr->top();
                             altStackPtr->pop();
                             scopePtr->push(element);
                             scopePtr->pop();
                             LOG_DEBUG(
                                 "Actually moved and deleted variable '" +
                                 varName + "' from alt stack top"
                             );
                         }

                         currentSymtab.removeSymbol(varName);
                         LOG_DEBUG(
                             "Removed variable '" + varName +
                             "' from symbol table"
                         );

                         LOG_DEBUG(
                             "Generated bytecode and executed deletion for "
                             "variable '" +
                             varName + "' from alt stack top"
                         );
                         return deleteResult;
                     }

                     // 不在栈顶: 先把上方元素搬到主栈临存
                     std::vector<StackElement> tempElements;

                     for (int64_t i = 0; i < altPosition; i++) {
                         deleteResult += opcodeToHex(
                             tbc::BytOpcode::OP_FROMALTSTACK
                         );

                         if (!altStackPtr->empty()) {
                             auto element = altStackPtr->top();
                             altStackPtr->pop();
                             scopePtr->push(element);
                             tempElements.push_back(element);
                             LOG_DEBUG(
                                 "Actually moved temp element from alt stack "
                                 "to main stack"
                             );
                         }
                     }

                     // 目标变量: 副栈 -> 主栈 -> 丢弃
                     deleteResult += opcodeToHex(tbc::BytOpcode::OP_FROMALTSTACK
                     );
                     deleteResult += opcodeToHex(tbc::BytOpcode::OP_DROP);

                     if (!altStackPtr->empty()) {
                         auto targetElement = altStackPtr->top();
                         altStackPtr->pop();
                         scopePtr->push(targetElement);
                         scopePtr->pop();
                         LOG_DEBUG(
                             "Actually moved and deleted target variable '" +
                             varName + "' from alt stack"
                         );
                     }

                     // 临存元素逆序放回副栈
                     for (int64_t i = tempElements.size() - 1; i >= 0; i--) {
                         deleteResult += opcodeToHex(
                             tbc::BytOpcode::OP_TOALTSTACK
                         );

                         if (!scopePtr->empty()) {
                             auto element = scopePtr->top();
                             scopePtr->pop();
                             altStackPtr->push(element);
                             LOG_DEBUG(
                                 "Actually moved temp element back to alt stack"
                             );
                         }
                     }

                     currentSymtab.removeSymbol(varName);
                     LOG_DEBUG(
                         "Removed variable '" + varName + "' from symbol table"
                     );

                     LOG_DEBUG(
                         "Generated bytecode and executed deletion for "
                         "variable '" +
                         varName + "' from alt stack position " +
                         std::to_string(altPosition) + ", moved " +
                         std::to_string(tempElements.size()) + " temp elements"
                     );

                     return cleanHexString(deleteResult);
                 };

                 // 估算变量在栈上占用的槽数
                 auto getVariableStackSize =
                     [&stackPtr](const std::string& varName) -> int64_t {
                     auto pos = stackPtr->getPos(varName);
                     if (!pos.has_value()) {
                         return 1;
                     }

                     try {
                         const auto& element = stackPtr->at(
                             static_cast<uint64_t>(pos.value())
                         );
                         std::string varType = element.getType();

                         if (varType.empty()) {
                             return 1;
                         }

                         // 结构体类型
                         if (varType.find("Struct") != std::string::npos ||
                             varType.find("struct") != std::string::npos) {
                             std::regex fieldCountRegex(
                                 R"((?:Struct|struct).*?(?:fieldCount|fields?)[:=](\d+))"
                             );
                             std::smatch match;

                             if (std::regex_search(
                                     varType, match, fieldCountRegex
                                 ) &&
                                 match.size() > 1) {
                                 try {
                                     int64_t fieldCount = std::stoll(
                                         match[1].str()
                                     );
                                     return std::max(
                                         fieldCount, static_cast<int64_t>(1)
                                     );
                                 } catch (const std::exception&) {
                                     // 解析失败, 走启发式
                                 }
                             }

                             // 启发式: 据类型串中冒号/逗号数估算
                             size_t colonCount = std::count(
                                 varType.begin(), varType.end(), ':'
                             );
                             size_t commaCount = std::count(
                                 varType.begin(), varType.end(), ','
                             );

                             if (colonCount > 2 || commaCount > 1) {
                                 return std::min(
                                     static_cast<int64_t>(
                                         colonCount + commaCount
                                     ),
                                     static_cast<int64_t>(8)
                                 );
                             }

                             return 2; // 默认复杂结构占 2 槽
                         }

                         if (varType.find("Array") != std::string::npos ||
                             varType.find("array") != std::string::npos ||
                             varType.find("[]") != std::string::npos) {
                             std::regex arraySizeRegex(
                                 R"((?:Array|array).*?(?:size|length)[:=](\d+))"
                             );
                             std::smatch match;

                             if (std::regex_search(
                                     varType, match, arraySizeRegex
                                 ) &&
                                 match.size() > 1) {
                                 try {
                                     int64_t arraySize = std::stoll(
                                         match[1].str()
                                     );
                                     return std::max(
                                         arraySize, static_cast<int64_t>(1)
                                     );
                                 } catch (const std::exception&) {
                                 }
                             }

                             return 2; // 默认数组占 2 槽
                         }

                         return 1; // 简单类型

                     } catch (const std::exception& e) {
                         LOG_WARNING(
                             "Failed to get type info for variable '" +
                             varName + "': " + e.what() +
                             ", assuming 1 stack position"
                         );
                         return 1;
                     }
                 };

                 std::string result;

                 if (size == 1) {
                     // Delete(x): 支持结构体递归删除
                     const std::string& varName = stackArgs[0].getName();

                     // 收集要删除的变量名（含展开的结构体字段）
                     std::vector<std::string> varsToDelete;

                     // 查找以 varName. 开头的所有字段
                     bool foundAnyField = false;
                     auto allSymbols =
                         stackPtr->getCurrentSymtab().getCurrentScopeSymbols();

                     for (const auto& symbol : allSymbols) {
                         std::string symbolName = symbol.getSymbolName();
                         if (symbolName.find(varName + ".") == 0) {
                             varsToDelete.push_back(symbolName);
                             foundAnyField = true;
                         }
                     }

                     // 没有字段则按普通变量处理
                     if (!foundAnyField) {
                         varsToDelete.push_back(varName);
                     }

                     LOG_DEBUG(
                         "Delete single parameter '" + varName +
                         "' expanded to " +
                         std::to_string(varsToDelete.size()) + " variables: " +
                         [&varsToDelete]() {
                             std::string list;
                             for (size_t i = 0; i < varsToDelete.size(); ++i) {
                                 if (i > 0)
                                     list += ", ";
                                 list += varsToDelete[i];
                             }
                             return list;
                         }()
                     );

                     // 多字段结构体: 走优化的多变量删除
                     if (varsToDelete.size() > 1) {
                         LOG_DEBUG(
                             "Processing struct deletion with " +
                             std::to_string(varsToDelete.size()) + " fields"
                         );

                         // {name, position, stackSize}
                         std::vector<std::tuple<std::string, int64_t, int64_t>>
                             structToDelete;

                         for (const auto& fieldName : varsToDelete) {
                             // 主栈优先
                             auto pos = stackPtr->getPos(fieldName, false);
                             if (pos.has_value()) {
                                 int64_t stackSize = getVariableStackSize(
                                     fieldName
                                 );
                                 structToDelete.push_back(
                                     {fieldName, pos.value(), stackSize}
                                 );
                                 continue;
                             }

                             // 主栈未找到再查副栈
                             auto altPos = stackPtr->getPos(fieldName, true);
                             if (altPos.has_value()) {
                                 LOG_INFO(
                                     "Delete() function: struct field '" +
                                     fieldName +
                                     "' found in alt stack at position " +
                                     std::to_string(altPos.value())
                                 );
                                 result += processAltStackDeletion(
                                     fieldName, altPos.value(), stackPtr
                                 );
                                 continue;
                             }

                             // 都没找到则跳过
                             LOG_DEBUG(
                                 "Delete() function: struct field '" +
                                 fieldName + "' not found in main or alt stack"
                             );
                         }

                         // 主栈字段: 走优化策略
                         if (!structToDelete.empty()) {
                             // 按位置降序: 避免删除时位置漂移
                             std::sort(
                                 structToDelete.begin(),
                                 structToDelete.end(),
                                 [](const auto& a, const auto& b) {
                                     return std::get<1>(a) > std::get<1>(b);
                                 }
                             );

                             // 与多参数 Delete 同一套优化策略
                             std::set<int64_t> deletePositions;
                             for (const auto& item : structToDelete) {
                                 deletePositions.insert(std::get<1>(item));
                             }

                             while (!structToDelete.empty()) {
                                 bool optimizationApplied = false;

                                 // 策略1: OP_2DROP
                                 if (deletePositions.count(0) &&
                                     deletePositions.count(1)) {
                                     result += opcodeToHex(
                                         tbc::BytOpcode::OP_2DROP
                                     );
                                     if (!stackPtr->empty())
                                         stackPtr->pop();
                                     if (!stackPtr->empty())
                                         stackPtr->pop();

                                     // 从删除列表移除位置 0/1 的字段
                                     auto it0 = std::find_if(
                                         structToDelete.begin(),
                                         structToDelete.end(),
                                         [](const auto& item) {
                                             return std::get<1>(item) == 0;
                                         }
                                     );
                                     auto it1 = std::find_if(
                                         structToDelete.begin(),
                                         structToDelete.end(),
                                         [](const auto& item) {
                                             return std::get<1>(item) == 1;
                                         }
                                     );

                                     if (it0 != structToDelete.end()) {
                                         auto& currentSymtab =
                                             stackPtr->getCurrentSymtab();
                                         currentSymtab.removeSymbol(
                                             std::get<0>(*it0)
                                         );
                                         structToDelete.erase(it0);
                                     }
                                     if (it1 != structToDelete.end()) {
                                         auto& currentSymtab =
                                             stackPtr->getCurrentSymtab();
                                         currentSymtab.removeSymbol(
                                             std::get<0>(*it1)
                                         );
                                         structToDelete.erase(it1);
                                     }

                                     deletePositions.erase(0);
                                     deletePositions.erase(1);

                                     // 位置整体下移 2
                                     std::set<int64_t> newPositions;
                                     for (auto pos : deletePositions) {
                                         if (pos >= 2)
                                             newPositions.insert(pos - 2);
                                     }
                                     deletePositions = newPositions;

                                     for (auto& item : structToDelete) {
                                         int64_t& itemPos = std::get<1>(item);
                                         if (itemPos >= 2)
                                             itemPos -= 2;
                                     }

                                     LOG_DEBUG(
                                         "Applied OP_2DROP optimization for "
                                         "struct fields at positions 0 and 1"
                                     );
                                     optimizationApplied = true;
                                 }

                                 // 策略2: OP_NIP
                                 else if (deletePositions.count(1) &&
                                          !deletePositions.count(0)) {
                                     result += opcodeToHex(
                                         tbc::BytOpcode::OP_NIP
                                     );

                                     if (stackPtr->size() >= 2) {
                                         auto top = stackPtr->top();
                                         stackPtr->pop();
                                         if (!stackPtr->empty())
                                             stackPtr->pop();
                                         stackPtr->getCurrentSymtab()
                                             .m_stackPtr->push(top);
                                     }

                                     auto it = std::find_if(
                                         structToDelete.begin(),
                                         structToDelete.end(),
                                         [](const auto& item) {
                                             return std::get<1>(item) == 1;
                                         }
                                     );
                                     if (it != structToDelete.end()) {
                                         auto& currentSymtab =
                                             stackPtr->getCurrentSymtab();
                                         currentSymtab.removeSymbol(
                                             std::get<0>(*it)
                                         );
                                         structToDelete.erase(it);
                                     }

                                     deletePositions.erase(1);

                                     std::set<int64_t> newPositions;
                                     for (auto pos : deletePositions) {
                                         if (pos > 1)
                                             newPositions.insert(pos - 1);
                                         else
                                             newPositions.insert(pos);
                                     }
                                     deletePositions = newPositions;

                                     for (auto& item : structToDelete) {
                                         int64_t& itemPos = std::get<1>(item);
                                         if (itemPos > 1)
                                             itemPos -= 1;
                                     }

                                     LOG_DEBUG(
                                         "Applied OP_NIP optimization for "
                                         "struct field at position 1"
                                     );
                                     optimizationApplied = true;
                                 }

                                 // 回退到传统删除
                                 if (!optimizationApplied &&
                                     !structToDelete.empty()) {
                                     auto current = structToDelete[0];
                                     std::string fieldName = std::get<0>(current
                                     );
                                     int64_t position = std::get<1>(current);

                                     if (position == 1) {
                                         result += opcodeToHex(
                                             tbc::BytOpcode::OP_NIP
                                         );
                                         if (stackPtr->size() >= 2) {
                                             auto top = stackPtr->top();
                                             stackPtr->pop();
                                             if (!stackPtr->empty())
                                                 stackPtr->pop();
                                             stackPtr->getCurrentSymtab()
                                                 .m_stackPtr->push(top);
                                         }
                                     } else {
                                         if (position == 0) {
                                             result += opcodeToHex(
                                                 tbc::BytOpcode::OP_DROP
                                             );
                                             if (!stackPtr->empty())
                                                 stackPtr->pop();
                                        } else {
                                            // pos==1 → OP_NIP，pos==2 → OP_ROT OP_DROP
                                            result += rollDropHex(position);
                                            stackPtr->roll(position);
                                            if (!stackPtr->empty())
                                                stackPtr->pop();
                                         }
                                     }

                                     auto& currentSymtab =
                                         stackPtr->getCurrentSymtab();
                                     currentSymtab.removeSymbol(fieldName);
                                     structToDelete.erase(structToDelete.begin()
                                     );
                                     deletePositions.erase(position);

                                     std::set<int64_t> newPositions;
                                     for (auto pos : deletePositions) {
                                         if (pos > position)
                                             newPositions.insert(pos - 1);
                                         else
                                             newPositions.insert(pos);
                                     }
                                     deletePositions = newPositions;

                                     for (auto& item : structToDelete) {
                                         int64_t& itemPos = std::get<1>(item);
                                         if (itemPos > position)
                                             itemPos -= 1;
                                     }

                                     LOG_DEBUG(
                                         "Applied single delete for struct "
                                         "field '" +
                                         fieldName + "' at position " +
                                         std::to_string(position)
                                     );
                                 }
                             }
                         }

                         return cleanHexString(result);
                     }

                     // 单变量（非结构体）走简单删除
                     else if (varsToDelete.size() == 1) {
                         const std::string& actualVarName = varsToDelete[0];

                         // 主栈优先
                         auto pos = stackPtr->getPos(actualVarName, false);
                         if (pos.has_value()) {
                             auto position = pos.value();
                             int64_t stackSize = getVariableStackSize(varName);

                             if (stackSize == 1) {
                                 // 简单类型: 同时生成字节码和同步内部栈
                                 if (position == 0) {
                                     result += opcodeToHex(
                                         tbc::BytOpcode::OP_DROP
                                     );

                                     if (!stackPtr->empty()) {
                                         stackPtr->pop();
                                         LOG_DEBUG(
                                             "Actually removed variable '" +
                                             varName + "' from main stack top"
                                         );
                                     }
                                 } else if (position == 1) {
                                     result += opcodeToHex(
                                         tbc::BytOpcode::OP_NIP
                                     );

                                     stackPtr->roll(position);
                                     if (!stackPtr->empty()) {
                                         stackPtr->pop();
                                         LOG_DEBUG(
                                             "Actually removed variable '" +
                                             varName +
                                             "' from main stack position 1 "
                                         );
                                     }
                                } else if (position > 1) {
                                    // pos==2: OP_ROT OP_DROP; pos>=3: N OP_ROLL OP_DROP
                                    result += rollDropHex(position);

                                    stackPtr->roll(position);
                                    if (!stackPtr->empty()) {
                                        stackPtr->pop();
                                        LOG_DEBUG(
                                            "Actually rolled and removed "
                                            "variable "
                                            "'" +
                                            varName +
                                             "' from main stack position " +
                                             std::to_string(position)
                                         );
                                     }
                                 }
                             } else {
                                 // 复杂结构: 删除多个连续栈元素
                                 for (int64_t i = 0; i < stackSize; i++) {
                                     if (position == 0) {
                                         result += opcodeToHex(
                                             tbc::BytOpcode::OP_DROP
                                         );

                                         if (!stackPtr->empty()) {
                                             stackPtr->pop();
                                             LOG_DEBUG(
                                                 "Actually removed element " +
                                                 std::to_string(i) +
                                                 " of variable '" + varName +
                                                 "' from main stack top"
                                             );
                                         }
                                     } else {
                                         // 每删一个, 后续元素位置前移
                                         int64_t currentPos = position - i;
                                        if (currentPos > 0) {
                                            // pos==1: OP_SWAP; pos==2: OP_ROT
                                            result += rollToTopHex(currentPos);

                                            stackPtr->roll(currentPos);
                                             LOG_DEBUG(
                                                 "Actually rolled element " +
                                                 std::to_string(i) +
                                                 " of variable '" + varName +
                                                 "' from position " +
                                                 std::to_string(currentPos) +
                                                 " to stack top"
                                             );
                                         }
                                         result += opcodeToHex(
                                             tbc::BytOpcode::OP_DROP
                                         );

                                         if (!stackPtr->empty()) {
                                             stackPtr->pop();
                                             LOG_DEBUG(
                                                 "Actually removed element " +
                                                 std::to_string(i) +
                                                 " of variable '" + varName +
                                                 "' from main stack"
                                             );
                                         }
                                     }
                                 }
                             }

                             auto& currentSymtab = stackPtr->getCurrentSymtab();
                             currentSymtab.removeSymbol(varName);
                             LOG_DEBUG(
                                 "Removed variable '" + varName +
                                 "' from symbol table"
                             );
                             return result;
                         }

                         // 主栈未找到再查副栈
                         auto altPos = stackPtr->getPos(varName, true);
                         if (altPos.has_value()) {
                             LOG_INFO(
                                 "Delete() function: variable '" + varName +
                                 "' found in alt stack at position " +
                                 std::to_string(altPos.value())
                             );
                             result += processAltStackDeletion(
                                 varName, altPos.value(), stackPtr
                             );
                             return cleanHexString(result);
                         }

                         LOG_INFO(
                             "Delete() function: variable '" + actualVarName +
                             "' not found in main or alt stack, skipping"
                         );
                         return "";
                     }
                 } else {
                     // 多参数 Delete(x,y,a,b,c...): 走代价感知的局部优化策略

                     // {name, position, stackSize}
                     std::vector<std::tuple<std::string, int64_t, int64_t>>
                         toDelete;

                     [[maybe_unused]] auto calculateOpcodeCost =
                         [](const std::string& operation) -> int {
                         if (operation == "OP_DROP")
                             return 1;
                         if (operation == "OP_2DROP")
                             return 1;
                         if (operation == "OP_NIP")
                             return 1;
                         if (operation.find("ROLL") != std::string::npos) {
                             // ROLL 还要推入位置参数, 估算字节码长度
                             return operation.length() / 2;
                         }
                         return operation.length() / 2;
                     };

                     [[maybe_unused]] auto isInDeleteList =
                         [&toDelete](int64_t position) -> bool {
                         for (const auto& item : toDelete) {
                             if (std::get<1>(item) == position) {
                                 return true;
                             }
                         }
                         return false;
                     };

                     // 收集每个变量的位置和占用大小, 支持主栈和副栈
                     for (const auto& arg : stackArgs) {
                         const std::string& varName = arg.getName();

                         // 含展开的结构体字段
                         std::vector<std::string> varsToDelete;

                         // 查找以 varName. 开头的字段
                         bool foundAnyField = false;
                         auto allSymbols = stackPtr->getCurrentSymtab()
                                               .getCurrentScopeSymbols();

                         for (const auto& symbol : allSymbols) {
                             std::string symbolName = symbol.getSymbolName();
                             if (symbolName.find(varName + ".") == 0) {
                                 varsToDelete.push_back(symbolName);
                                 foundAnyField = true;
                             }
                         }

                         if (!foundAnyField) {
                             varsToDelete.push_back(varName);
                         }

                         for (const auto& actualVarName : varsToDelete) {
                             auto pos = stackPtr->getPos(actualVarName, false);
                             if (pos.has_value()) {
                                 int64_t stackSize = getVariableStackSize(
                                     actualVarName
                                 );
                                 toDelete.push_back(
                                     {actualVarName, pos.value(), stackSize}
                                 );
                                 continue;
                             }

                             auto altPos =
                                 stackPtr->getPos(actualVarName, true);
                             if (altPos.has_value()) {
                                 LOG_INFO(
                                     "Delete() function: variable '" +
                                     actualVarName +
                                     "' found in alt stack at position " +
                                     std::to_string(altPos.value())
                                 );

                                 result += processAltStackDeletion(
                                     actualVarName, altPos.value(), stackPtr
                                 );
                                 continue;
                             }

                             LOG_DEBUG(
                                 "Delete() function: variable '" +
                                 actualVarName +
                                 "' not found in main or alt stack"
                             );
                         }
                     }

                     if (toDelete.empty()) {
                         LOG_INFO("Delete() function: no variables found in "
                                  "stack, no operation performed");
                         return "";
                     }

                     // 待删除位置的集合, 便于快速查找
                     std::set<int64_t> deletePositions;
                     for (const auto& item : toDelete) {
                         deletePositions.insert(std::get<1>(item));
                     }

                     while (!toDelete.empty()) {
                         bool optimizationApplied = false;

                         // 策略1: 栈顶和次栈顶都要删 -> OP_2DROP
                         if (deletePositions.count(0) &&
                             deletePositions.count(1)) {
                             result += opcodeToHex(tbc::BytOpcode::OP_2DROP);

                             if (!stackPtr->empty())
                                 stackPtr->pop();
                             if (!stackPtr->empty())
                                 stackPtr->pop();

                             auto it0 = std::find_if(
                                 toDelete.begin(),
                                 toDelete.end(),
                                 [](const auto& item) {
                                     return std::get<1>(item) == 0;
                                 }
                             );
                             auto it1 = std::find_if(
                                 toDelete.begin(),
                                 toDelete.end(),
                                 [](const auto& item) {
                                     return std::get<1>(item) == 1;
                                 }
                             );

                             if (it0 != toDelete.end()) {
                                 auto& currentSymtab =
                                     stackPtr->getCurrentSymtab();
                                 currentSymtab.removeSymbol(std::get<0>(*it0));
                                 toDelete.erase(it0);
                             }
                             if (it1 != toDelete.end()) {
                                 auto& currentSymtab =
                                     stackPtr->getCurrentSymtab();
                                 currentSymtab.removeSymbol(std::get<0>(*it1));
                                 toDelete.erase(it1);
                             }

                             deletePositions.erase(0);
                             deletePositions.erase(1);

                             // 删了 2 个元素, 更新剩余位置
                             std::set<int64_t> newPositions;
                             for (auto pos : deletePositions) {
                                 if (pos >= 2)
                                     newPositions.insert(pos - 2);
                             }
                             deletePositions = newPositions;

                             for (auto& item : toDelete) {
                                 int64_t& itemPos = std::get<1>(item);
                                 if (itemPos >= 2)
                                     itemPos -= 2;
                             }

                             LOG_DEBUG(
                                 "Applied OP_2DROP optimization for positions "
                                 "0 and 1"
                             );
                             optimizationApplied = true;
                         }

                         // 策略2: 仅次栈顶需要删 -> OP_NIP
                         else if (deletePositions.count(1) &&
                                  !deletePositions.count(0)) {
                             result += opcodeToHex(tbc::BytOpcode::OP_NIP);

                             if (stackPtr->size() >= 2) {
                                 auto top = stackPtr->top();
                                 stackPtr->pop();
                                 if (!stackPtr->empty()) {
                                     stackPtr->pop();
                                 }
                                 stackPtr->getCurrentSymtab().m_stackPtr->push(
                                     top
                                 );
                             }

                             auto it = std::find_if(
                                 toDelete.begin(),
                                 toDelete.end(),
                                 [](const auto& item) {
                                     return std::get<1>(item) == 1;
                                 }
                             );
                             if (it != toDelete.end()) {
                                 auto& currentSymtab =
                                     stackPtr->getCurrentSymtab();
                                 currentSymtab.removeSymbol(std::get<0>(*it));
                                 toDelete.erase(it);
                             }

                             deletePositions.erase(1);

                             // 删了位置 1, 位置 2+ 整体下移
                             std::set<int64_t> newPositions;
                             for (auto pos : deletePositions) {
                                 if (pos > 1)
                                     newPositions.insert(pos - 1);
                                 else
                                     newPositions.insert(pos);
                             }
                             deletePositions = newPositions;

                             for (auto& item : toDelete) {
                                 int64_t& itemPos = std::get<1>(item);
                                 if (itemPos > 1)
                                     itemPos -= 1;
                             }

                             LOG_DEBUG(
                                 "Applied OP_NIP optimization for position 1"
                             );
                             optimizationApplied = true;
                         }

                         // 策略3: 栈顶要删, 找一个移动代价最低的搭档配 OP_2DROP
                         else if (deletePositions.count(0) &&
                                  deletePositions.size() > 1) {
                             int64_t bestPartner = -1;
                             int bestCost = INT_MAX;

                             for (auto pos : deletePositions) {
                                 if (pos != 0) {
                                     int moveCost =
                                         (pos <= 16)    ? 1
                                         : (pos <= 255) ? 2
                                         : (pos <= 65535)
                                             ? 3
                                             : 5;
                                     // ROLL + 2DROP
                                     int totalCost = moveCost + 1 + 1;

                                     if (totalCost < bestCost) {
                                         bestCost = totalCost;
                                         bestPartner = pos;
                                     }
                                 }
                             }

                            if (bestPartner != -1) {
                                // 把搭档移到次栈顶, 再 OP_2DROP
                                result += rollToTopHex(bestPartner - 1);
                                result += opcodeToHex(tbc::BytOpcode::OP_2DROP
                                );

                                 stackPtr->roll(bestPartner - 1);
                                 if (!stackPtr->empty())
                                     stackPtr->pop();
                                 if (!stackPtr->empty())
                                     stackPtr->pop();

                                 auto it0 = std::find_if(
                                     toDelete.begin(),
                                     toDelete.end(),
                                     [](const auto& item) {
                                         return std::get<1>(item) == 0;
                                     }
                                 );
                                 auto itPartner = std::find_if(
                                     toDelete.begin(),
                                     toDelete.end(),
                                     [bestPartner](const auto& item) {
                                         return std::get<1>(item) ==
                                                bestPartner;
                                     }
                                 );

                                 if (it0 != toDelete.end()) {
                                     auto& currentSymtab =
                                         stackPtr->getCurrentSymtab();
                                     currentSymtab.removeSymbol(std::get<0>(*it0
                                     ));
                                     toDelete.erase(it0);
                                 }
                                 if (itPartner != toDelete.end()) {
                                     auto& currentSymtab =
                                         stackPtr->getCurrentSymtab();
                                     currentSymtab.removeSymbol(
                                         std::get<0>(*itPartner)
                                     );
                                     toDelete.erase(itPartner);
                                 }

                                 deletePositions.erase(0);
                                 deletePositions.erase(bestPartner);

                                 std::set<int64_t> newPositions;
                                 for (auto pos : deletePositions) {
                                     if (pos > bestPartner)
                                         newPositions.insert(pos - 2);
                                     else if (pos >= 2)
                                         newPositions.insert(pos - 2);
                                 }
                                 deletePositions = newPositions;

                                 for (auto& item : toDelete) {
                                     int64_t& itemPos = std::get<1>(item);
                                     if (itemPos > bestPartner)
                                         itemPos -= 2;
                                     else if (itemPos >= 2)
                                         itemPos -= 2;
                                 }

                                 LOG_DEBUG(
                                     "Applied OP_2DROP optimization with "
                                     "partner at position " +
                                     std::to_string(bestPartner)
                                 );
                                 optimizationApplied = true;
                             }
                         }

                         // 策略4: 回退到单元素删除
                         if (!optimizationApplied && !toDelete.empty()) {
                             auto current = toDelete[0];
                             std::string varName = std::get<0>(current);
                             int64_t position = std::get<1>(current);
                             int64_t stackSize = std::get<2>(current);

                             // 次栈顶且单槽: OP_NIP
                             if (position == 1 && stackSize == 1) {
                                 result += opcodeToHex(tbc::BytOpcode::OP_NIP);

                                 if (stackPtr->size() >= 2) {
                                     auto top = stackPtr->top();
                                     stackPtr->pop();
                                     if (!stackPtr->empty()) {
                                         stackPtr->pop();
                                     }
                                     stackPtr->getCurrentSymtab()
                                         .m_stackPtr->push(top);
                                 }
                             } else {
                                 if (position == 0) {
                                     result += opcodeToHex(
                                         tbc::BytOpcode::OP_DROP
                                     );
                                     if (!stackPtr->empty())
                                         stackPtr->pop();
                                } else {
                                    // pos==1: OP_NIP; pos==2: OP_ROT OP_DROP
                                    result += rollDropHex(position);

                                    stackPtr->roll(position);
                                    if (!stackPtr->empty())
                                        stackPtr->pop();
                                }
                            }

                             auto& currentSymtab = stackPtr->getCurrentSymtab();
                             currentSymtab.removeSymbol(varName);
                             toDelete.erase(toDelete.begin());
                             deletePositions.erase(position);

                             std::set<int64_t> newPositions;
                             for (auto pos : deletePositions) {
                                 if (pos > position)
                                     newPositions.insert(pos - 1);
                                 else
                                     newPositions.insert(pos);
                             }
                             deletePositions = newPositions;

                             for (auto& item : toDelete) {
                                 int64_t& itemPos = std::get<1>(item);
                                 if (itemPos > position)
                                     itemPos -= 1;
                             }

                             LOG_DEBUG(
                                 "Applied single delete for variable '" +
                                 varName + "' at position " +
                                 std::to_string(position)
                             );
                         }
                     }
                 }

                 return cleanHexString(result);
             };

             return std::make_shared<
                 VariableArgBuiltinFunction<decltype(func)>>(
                 size,
                 func,
                 0,                          // Delete函数不返回值
                 std::vector<tbc::OpType>{}, // 无返回值类型
                 std::vector<tbc::OpType>{}  // 接受任意类型的输入
             );
         }},
        {"Push",
         [](size_t size) -> std::shared_ptr<BuiltinFunction> {
             if (1 != size) {
                 return nullptr;
             }
             auto func = [size](
                             const StackElement& /*objElement*/,
                             const std::vector<StackElement>& stackArgs,
                             std::shared_ptr<Scope> stackPtr
                         ) -> std::string {
                 if (size != stackArgs.size()) {
                     LOG_ERROR(
                         "Push() function argument count mismatch: "
                         "expected 1 argument, got " +
                         std::to_string(stackArgs.size()) + " arguments"
                     );
                     throw std::invalid_argument(
                         "Push() function argument count mismatch: expected 1 "
                         "argument, got " +
                         std::to_string(stackArgs.size()) + " arguments"
                     );
                 }

                 if (!stackPtr) {
                     LOG_ERROR(
                         "Push() function execution failed: stack pointer is "
                         "null, cannot perform stack operations"
                     );
                     throw std::runtime_error(
                         "Push() function execution failed: stack pointer is "
                         "null, cannot perform stack operations"
                     );
                 }

                 std::string result;

                 try {
                     const StackElement& element = stackArgs[0];

                     // Push 只能处理 fixed 区的数据，必须不在主栈/副栈上
                     const std::string& varName = element.getName();

                     auto mainStackPos = stackPtr->getPos(varName, false);
                     if (mainStackPos.has_value()) {
                         LOG_ERROR(
                             "Push() function execution failed: variable '" +
                             varName +
                             "' is already on main stack at position " +
                             std::to_string(mainStackPos.value()) +
                             ". Push can only be used on fixed area data."
                         );
                         throw std::runtime_error(
                             "Push() function execution failed: variable '" +
                             varName + "' is already on main stack. " +
                             "Push can only be used on fixed area data."
                         );
                     }

                     auto altStackPos = stackPtr->getPos(varName, true);
                     if (altStackPos.has_value()) {
                         LOG_ERROR(
                             "Push() function execution failed: variable '" +
                             varName +
                             "' is already on alt stack at position " +
                             std::to_string(altStackPos.value()) +
                             ". Push can only be used on fixed area data."
                         );
                         throw std::runtime_error(
                             "Push() function execution failed: variable '" +
                             varName + "' is already on alt stack. " +
                             "Push can only be used on fixed area data."
                         );
                     }

                     // 字面量已转 hex; 变量名按其它分支处理
                     std::string elementData = element.getName();

                     // 内置对象成员（如 BVM.version）: 字节码 hex，
                     // 调用方再拆成索引 + OP_PUSH_META 两条
                     if (element.getType() == "builtin_member") {
                         result = element.getData();
                     }
                     // 脚本元素（<self.count> 等占位）原样返回, 由后续处理
                     else if (tbc::isScript(elementData)) {
                         result = elementData;
                     }
                     // 0x 开头的脚本数据: 去掉前缀即是推送字节码
                     else if (elementData.length() >= 2 &&
                              elementData.substr(0, 2) == "0x") {
                         result = elementData.substr(2);
                     } else {
                         // 其他情况由实际数据生成推送字节码
                        const auto& data = element.valueData();
                         if (data.empty()) {
                             result = opcodeToHex(tbc::BytOpcode::OP_0);
                         } else {
                             result = encodePushData(data);
                         }
                     }

                     LOG_DEBUG(
                         "Push() function: generated push opcode for argument "
                         "'" +
                         element.getName() + "'"
                     );

                 } catch (const std::invalid_argument& e) {
                     throw;
                 } catch (const std::runtime_error& e) {
                     throw;
                 } catch (const std::exception& e) {
                     LOG_ERROR(
                         "Push() function execution "
                         "encountered unknown error: " +
                         std::string(e.what())
                     );
                     throw std::runtime_error(
                         "Push() function execution "
                         "encountered unknown error: " +
                         std::string(e.what())
                     );
                 }

                 return result;
             };
             return std::make_shared<
                 BuiltinFunctionTemplate<1, decltype(func), 1>>(
                 func,
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES}
             );
         }},

        {"Pop",
         [](size_t size) -> std::shared_ptr<BuiltinFunction> {
             if (1 != size) {
                 return nullptr;
             }
             auto func = [size](
                             const StackElement& /*objElement*/,
                             const std::vector<StackElement>& stackArgs,
                             std::shared_ptr<Scope> stackPtr
                         ) -> std::string {
                 if (size != stackArgs.size()) {
                     LOG_ERROR(
                         "Pop() function argument count mismatch: "
                         "expected 1 argument, got " +
                         std::to_string(stackArgs.size()) + " arguments"
                     );
                     throw std::invalid_argument(
                         "Pop() function argument count mismatch: expected 1 "
                         "argument, got " +
                         std::to_string(stackArgs.size()) + " arguments"
                     );
                 }

                 if (!stackPtr) {
                     LOG_ERROR(
                         "Pop() function execution failed: stack pointer is "
                         "null, cannot perform stack operations"
                     );
                     throw std::runtime_error(
                         "Pop() function execution failed: stack pointer is "
                         "null, cannot perform stack operations"
                     );
                 }

                 // Pop(): 仅在编译期清除栈状态, 不发射任何字节码
                 const std::string& baseVarName = stackArgs[0].getName();
                 auto& currentSymtab = stackPtr->getCurrentSymtab();

                 // 收集 var.field 形式的结构体字段
                 std::vector<std::string> structFields;
                 const std::string structPrefix = baseVarName + ".";
                 auto allSymbols = currentSymtab.getCurrentScopeSymbols();
                 structFields.reserve(allSymbols.size());
                 for (const auto& symbol : allSymbols) {
                     const std::string& symbolName = symbol.getSymbolName();
                     if (symbolName.rfind(structPrefix, 0) == 0) {
                         structFields.push_back(symbolName);
                     }
                 }

                 // 删除顺序: 字段在前, 基础变量在后
                 std::vector<std::string> removalOrder;
                 removalOrder.reserve(structFields.size() + 1);
                 for (const auto& fieldName : structFields) {
                     removalOrder.push_back(fieldName);
                 }
                 removalOrder.push_back(baseVarName);

                 if (removalOrder.empty()) {
                     removalOrder.push_back(baseVarName);
                 }

                 auto removeFromMain = [&](const std::string& targetName
                                       ) -> bool {
                     auto posOpt = stackPtr->getPos(targetName, false);
                     if (!posOpt.has_value()) {
                         return false;
                     }
                     auto position = posOpt.value();
                     if (position < 0) {
                         LOG_ERROR(
                             "Pop() function: variable '" + targetName +
                             "' has invalid main stack position " +
                             std::to_string(position)
                         );
                         return false;
                     }
                     if (position > 0) {
                         stackPtr->roll(position);
                     }
                     if (!stackPtr->empty()) {
                         stackPtr->pop();
                     }
                     currentSymtab.removeSymbol(targetName);
                     LOG_DEBUG(
                         "Pop() function: removed variable '" + targetName +
                         "' from main stack at position " +
                         std::to_string(position) +
                         " (compiler-level only, no bytecode generated)"
                     );
                     return true;
                 };

                 auto removeFromAlt = [&](const std::string& targetName
                                      ) -> bool {
                     auto posOpt = stackPtr->getPos(targetName, true);
                     if (!posOpt.has_value()) {
                         return false;
                     }
                     auto position = posOpt.value();
                     if (position < 0) {
                         LOG_ERROR(
                             "Pop() function: variable '" + targetName +
                             "' has invalid alt stack position " +
                             std::to_string(position)
                         );
                         return false;
                     }

                     auto& altStackPtr = currentSymtab.m_altStackPtr;
                     std::vector<StackElement> tempElements;
                     tempElements.reserve(
                         static_cast<size_t>(std::max<int64_t>(0, position))
                     );

                     for (int64_t i = 0; i < position; i++) {
                         if (altStackPtr->empty()) {
                             break;
                         }
                         tempElements.push_back(altStackPtr->top());
                         altStackPtr->pop();
                     }

                     if (!altStackPtr->empty()) {
                         altStackPtr->pop();
                     }

                     for (auto it = tempElements.rbegin();
                          it != tempElements.rend();
                          ++it) {
                         altStackPtr->push(*it);
                     }

                     currentSymtab.removeSymbol(targetName);
                     LOG_DEBUG(
                         "Pop() function: removed variable '" + targetName +
                         "' from alt stack at position " +
                         std::to_string(position) +
                         " (compiler-level only, no bytecode generated)"
                     );
                     return true;
                 };

                 bool removedAny = false;
                 for (const auto& targetName : removalOrder) {
                     bool removed = removeFromMain(targetName);
                     if (!removed) {
                         removed = removeFromAlt(targetName);
                     }

                     if (removed) {
                         removedAny = true;
                     } else {
                         LOG_DEBUG(
                             "Pop() function: target '" + targetName +
                             "' not found on main or alt stack, skip"
                         );
                     }
                 }

                 if (!removedAny) {
                     LOG_WARNING(
                         "Pop() function: variable '" + baseVarName +
                         "' not found in main or alt stack "
                         "(including struct fields)"
                     );
                 }

                 return "";
             };
             return std::make_shared<
                 BuiltinFunctionTemplate<1, decltype(func), 0>>(
                 func,
                 std::vector<tbc::OpType>{},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES}
             );
         }},

        {"Move",
         [](size_t size) -> std::shared_ptr<BuiltinFunction> {
             if (1 != size) {
                 return nullptr;
             }
             auto func = [size](
                             const StackElement& /*objElement*/,
                             const std::vector<StackElement>& /*stackArgs*/,
                             std::shared_ptr<Scope> /*stackPtr*/
                         ) -> std::string {
                 // 零成本抽象: 所有权转移已由 PreAnalysisVisitor 处理
                 LOG_DEBUG("move() function called - zero-cost abstraction, no "
                           "bytecode generated");
                 return "";
             };
             return std::make_shared<
                 BuiltinFunctionTemplate<1, decltype(func), 1>>(
                 func,
                 std::vector<tbc::OpType>{tbc::OpType::BYTES},
                 std::vector<tbc::OpType>{tbc::OpType::BYTES}
             );
         }},

        {"Keep",
         [](size_t size) -> std::shared_ptr<BuiltinFunction> {
             if (0 == size) {
                 return nullptr; // 至少 1 个参数
             }

             auto func = [size](
                             const StackElement& /*objElement*/,
                             const std::vector<StackElement>& stackArgs,
                             std::shared_ptr<Scope> /*stackPtr*/
                         ) -> std::string {
                 if (size != stackArgs.size()) {
                     LOG_ERROR(
                         "Keep() function argument count mismatch: "
                         "expected " +
                         std::to_string(size) + " arguments, got " +
                         std::to_string(stackArgs.size()) + " arguments"
                     );
                     throw std::invalid_argument(
                         "Keep() function argument count mismatch"
                     );
                 }

                 // 零成本抽象: 所有权处理已在 PreAnalysisVisitor 完成
                 LOG_DEBUG(
                     "Keep() function called with " + std::to_string(size) +
                     " arguments - zero-cost abstraction, no bytecode generated"
                 );
                 return "";
             };

             return std::make_shared<
                 VariableArgBuiltinFunction<decltype(func)>>(
                 size,
                 func,
                 size, // 返回数与输入数相同
                 std::vector<tbc::OpType>(size, tbc::OpType::BYTES),
                 std::vector<tbc::OpType>(size, tbc::OpType::BYTES)
             );
         }},
};

static const std::unordered_set<std::string> consumeFunSet = {"Slice", "Move"};
inline bool isConsumeFun(const std::string& funNameStr)
{
    return consumeFunSet.find(funNameStr) != consumeFunSet.end();
}

// 内置函数工厂
class BuiltinFunctionFactory
{
public:
    static std::shared_ptr<BuiltinFunction>
    createFunction(const std::string& name, const size_t argCount)
    {
        auto it = buildinCreators.find(name);
        if (it != buildinCreators.end()) {
            return it->second(argCount);
        }
        return nullptr;
    }

    static bool
    isBuiltinFunction(const std::string& name, const size_t /*argCount*/)
    {
        if (buildinCreators.find(name) != buildinCreators.end()) {
            return true;
        }
        return false;
    }

    // 从栈执行内置函数 (主接口)
    static std::string executeFromStack(
        const std::string& name,
        const StackElement& objElement,
        std::shared_ptr<Scope> scopePtr
    )
    {
        auto func = createFunction(name, 0);
        if (!func) {
            LOG_ERROR("Function not found or parameter count mismatch");
            throw std::runtime_error(
                "BuiltinFunctionFactory::executeFromStack failed: function '" +
                name + "' not found or parameter count mismatch"
            );
        }

        auto args = BuiltinFunction::extractArgsFromStack(
            scopePtr, func->getExpectedArgCount()
        );
        return func->getOpcodeHex(objElement, args, scopePtr);
    }

    // 旧接口: 直接拿到字节码
    static std::string getOpcodeHex(
        const std::string& name,
        size_t argCount,
        std::shared_ptr<Scope> scopePtr,
        const StackElement& objElement,
        const std::vector<StackElement>& stackArgs
    )
    {
        auto func = createFunction(name, argCount);
        if (!func) {
            LOG_ERROR(
                "BuiltinFunctionFactory: function '" + name + "' with " +
                std::to_string(argCount) + " arguments not found or unsupported"
            );
            throw std::runtime_error(
                "BuiltinFunctionFactory: function '" + name + "' with " +
                std::to_string(argCount) + " arguments not found or unsupported"
            );
        }
        return func->getOpcodeHex(objElement, stackArgs, scopePtr);
    }

    static bool validateFunctionArgs(const std::string& name, size_t argCount)
    {
        auto func = createFunction(name, argCount);
        if (!func) {
            return false;
        }
        return func->validateArgCount(argCount);
    }

    static std::vector<tbc::OpType>
    getFunctionReturnTypes(const std::string& name, size_t argCount)
    {
        auto func = createFunction(name, argCount);
        return func ? func->getReturnTypes() : std::vector<tbc::OpType>{};
    }

    static std::vector<tbc::OpType>
    getFunctionInputTypes(const std::string& name, size_t argCount)
    {
        auto func = createFunction(name, argCount);
        return func ? func->getInputTypes() : std::vector<tbc::OpType>{};
    }

    static std::string getOpTypeString(tbc::OpType type)
    {
        return BytecodeFunctionFactory::getOpTypeString(type);
    }

    static std::vector<std::string> getOpTypeStrings(
        const std::vector<tbc::OpType>& types
    )
    {
        return BytecodeFunctionFactory::getOpTypeStrings(types);
    }

    static std::vector<std::string>
    getFunctionReturnTypeStrings(const std::string& name, size_t argCount)
    {
        return getOpTypeStrings(getFunctionReturnTypes(name, argCount));
    }

    static std::vector<std::string>
    getFunctionInputTypeStrings(const std::string& name, size_t argCount)
    {
        return getOpTypeStrings(getFunctionInputTypes(name, argCount));
    }
};
SPACE_TBC_END
#endif // BYTCODE_BUILTIN_FUNCTION_H
