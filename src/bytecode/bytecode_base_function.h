#ifndef BYTECODE_BASE_FUNCTION_H
#define BYTECODE_BASE_FUNCTION_H

#include <string>
#include <vector>

#include "byt_data_types.h"
#include "byt_defs.h"

SPACE_TBC_START

// 字节码函数统一基类：参数验证、返回值信息、类型系统支持
class BytecodeFunction
{
public:
    virtual ~BytecodeFunction() = default;

    virtual size_t getExpectedArgCount() const = 0;

    virtual bool validateArgCount(size_t actualCount) const
    {
        return actualCount == getExpectedArgCount();
    }

    virtual size_t getReturnCount() const = 0;

    // 类型列表顺序：从远离栈顶到栈顶
    virtual std::vector<tbc::OpType> getReturnTypes() const = 0;
    virtual std::vector<tbc::OpType> getInputTypes() const = 0;

    // true=对参数顺序敏感；false=满足交换律。内置函数通常为 true
    virtual bool isArgOrderSensitive() const
    {
        return true;
    }
};

// 字节码函数工厂统一接口：类型查询与字符串转换
class BytecodeFunctionFactory
{
public:
    static std::string getOpTypeString(tbc::OpType type)
    {
        return tbc::TypeMapper::toString(type);
    }

    static std::vector<std::string> getOpTypeStrings(
        const std::vector<tbc::OpType>& types)
    {
        std::vector<std::string> result;
        result.reserve(types.size());
        for (const auto& type : types) {
            result.push_back(getOpTypeString(type));
        }
        return result;
    }

    static std::string getBytecodeTypeString(tbc::BytecodeType type)
    {
        return tbc::TypeMapper::toString(type);
    }
};

SPACE_TBC_END

#endif // BYTECODE_BASE_FUNCTION_H