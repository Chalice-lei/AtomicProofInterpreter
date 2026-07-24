//
// Created by Wayne on 25-2-25.
//

#ifndef OP_STACK_H
#define OP_STACK_H

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "util_defs.h"

SPACE_TBC_START

typedef std::vector<uint8_t> valtype;

class stack_overflow_error : public std::overflow_error
{
public:
    explicit stack_overflow_error(const std::string& str)
        : std::overflow_error(str)
    {}
};

class script_num_error : public std::runtime_error
{
public:
    explicit script_num_error(const std::string& str) : std::runtime_error(str)
    {}
};

class StackElement
{
public:
    // 元素开销常量（防空元素攻击），与 Bitcoin SV 一致
    static constexpr size_t ELEMENT_OVERHEAD = 32;

private:
    valtype m_name;
    valtype m_type;
    valtype m_data;

public:
    StackElement() = default;

    StackElement(const StackElement& other)
        : m_name(other.m_name), m_type(other.m_type), m_data(other.m_data)
    {}

    StackElement& operator=(const StackElement& other)
    {
        if (this != &other) {
            m_name = other.m_name;
            m_type = other.m_type;
            m_data = other.m_data;
        }
        return *this;
    }

    explicit StackElement(const valtype& data) : m_name(data)
    {}

    explicit StackElement(
        const valtype& name,
        const valtype& type,
        const valtype& data
    )
        : m_name(name), m_type(type), m_data(data)
    {}

    // 向后兼容
    explicit StackElement(const std::string& str)
        : m_name(str.begin(), str.end())
    {}

    explicit StackElement(
        const std::string& nameStr,
        const std::string& typeStr,
        const std::string& dataStr
    )
        : m_name(nameStr.begin(), nameStr.end()),
          m_type(typeStr.begin(), typeStr.end()),
          m_data(dataStr.begin(), dataStr.end())
    {}

    explicit StackElement(int64_t value);

    explicit StackElement(bool value) : m_data(value ? valtype{1} : valtype{})
    {}

    const valtype& nameData() const
    {
        return m_name;
    }
    valtype& nameData()
    {
        return m_name;
    }
    const valtype& typeData() const
    {
        return m_type;
    }
    valtype& typeData()
    {
        return m_type;
    }
    const valtype& valueData() const
    {
        return m_data;
    }
    valtype& valueData()
    {
        return m_data;
    }

    size_t size() const
    {
        return m_name.size();
    }
    bool empty() const
    {
        return m_name.empty();
    }

    uint8_t& operator[](size_t pos)
    {
        return m_name[pos];
    }
    const uint8_t& operator[](size_t pos) const
    {
        return m_name[pos];
    }
    uint8_t& front()
    {
        return m_name.front();
    }
    const uint8_t& front() const
    {
        return m_name.front();
    }
    uint8_t& back()
    {
        return m_name.back();
    }
    const uint8_t& back() const
    {
        return m_name.back();
    }

    std::vector<uint8_t>::iterator begin()
    {
        return m_name.begin();
    }
    std::vector<uint8_t>::iterator end()
    {
        return m_name.end();
    }
    std::vector<uint8_t>::const_iterator begin() const
    {
        return m_name.begin();
    }
    std::vector<uint8_t>::const_iterator end() const
    {
        return m_name.end();
    }

    std::string getName() const;
    std::string getType() const;
    std::string getData() const;

    void push_back(uint8_t byte)
    {
        m_name.push_back(byte);
    }
    void append(const StackElement& other)
    {
        m_name.insert(m_name.end(), other.m_name.begin(), other.m_name.end());
    }
    void clear()
    {
        m_name.clear();
    }

    // 重命名（零成本优化的核心接口）
    void setName(const std::string& newName)
    {
        m_name.assign(newName.begin(), newName.end());
    }

    void setType(const std::string& newType)
    {
        m_type.assign(newType.begin(), newType.end());
    }

    size_t getMemoryUsage() const
    {
        return ELEMENT_OVERHEAD + m_data.size();
    }

    // Bitcoin 标准最小编码
    void minimally_encode();
    bool is_minimally_encoded() const;

    bool operator==(const StackElement& other) const
    {
        return m_name == other.m_name;
    }
    bool operator==(const std::string& str) const
    {
        return getName() == str;
    }

    bool operator!=(const StackElement& other) const
    {
        return m_name != other.m_name;
    }
    bool operator!=(const std::string& str) const
    {
        return getName() != str;
    }
};

// Bitcoin 风格的安全数值类
class CScriptNum
{
private:
    int64_t m_value;

    static bool IsMinimallyEncoded(const valtype& data);
    static valtype Serialize(const int64_t& value);

public:
    explicit CScriptNum(const int64_t& n) : m_value(n)
    {}

    explicit CScriptNum(const valtype& vch, bool fRequireMinimal = true);

    // 带最大大小限制（用于特定操作码限制）
    explicit CScriptNum(
        const valtype& vch,
        bool fRequireMinimal,
        size_t maxSize
    );

    int64_t getint() const
    {
        return m_value;
    }
    valtype getvch() const
    {
        return Serialize(m_value);
    }

    CScriptNum operator+(const CScriptNum& rhs) const
    {
        return CScriptNum(m_value + rhs.m_value);
    }
    CScriptNum operator-(const CScriptNum& rhs) const
    {
        return CScriptNum(m_value - rhs.m_value);
    }
    CScriptNum operator*(const CScriptNum& rhs) const
    {
        return CScriptNum(m_value * rhs.m_value);
    }
    CScriptNum operator/(const CScriptNum& rhs) const
    {
        return CScriptNum(m_value / rhs.m_value);
    }
    CScriptNum operator%(const CScriptNum& rhs) const
    {
        return CScriptNum(m_value % rhs.m_value);
    }

    bool operator==(const CScriptNum& rhs) const
    {
        return m_value == rhs.m_value;
    }
    bool operator!=(const CScriptNum& rhs) const
    {
        return m_value != rhs.m_value;
    }
    bool operator<=(const CScriptNum& rhs) const
    {
        return m_value <= rhs.m_value;
    }
    bool operator<(const CScriptNum& rhs) const
    {
        return m_value < rhs.m_value;
    }
    bool operator>=(const CScriptNum& rhs) const
    {
        return m_value >= rhs.m_value;
    }
    bool operator>(const CScriptNum& rhs) const
    {
        return m_value > rhs.m_value;
    }
};

class OpStack
{
public:
    // 默认最大栈大小（参考 Bitcoin SV）
    static constexpr size_t DEFAULT_MAX_STACK_SIZE = 1000000; // 1MB

    OpStack() : OpStack(DEFAULT_MAX_STACK_SIZE){};
    explicit OpStack(size_t maxStackSize);
    ~OpStack() = default;

    OpStack(const OpStack&) = default;
    OpStack(OpStack&&) = default;
    OpStack& operator=(OpStack&&) = default;
    OpStack& operator=(const OpStack&) = delete;

    void push(const StackElement& element, std::string* statusStr = nullptr);
    void push(StackElement&& element, std::string* statusStr = nullptr);
    void push(const valtype& data);
    void push(const std::string& str);
    void push(int64_t value);
    void push(bool value);

    void pop(std::string* statusStr = nullptr);
    const StackElement& stacktop(int index) const;
    StackElement& stacktop(int index);
    StackElement& top()
    {
        return m_stack.back();
    }

    size_t size() const
    {
        return m_stack.size();
    }
    bool empty() const
    {
        return m_stack.empty();
    }

    size_t getCombinedStackSize() const;
    void setCombinedStackSize(size_t size);
    size_t getMaxStackSize() const
    {
        return m_maxStackSize;
    }

    const StackElement& front() const;
    const StackElement& back() const;
    const StackElement& at(uint64_t index) const;
    StackElement& at(uint64_t index);

    void erase(int index);
    void insert(int position, const StackElement& element);
    void swap(int index1, int index2);

    // 重命名（零成本优化）
    bool rename(const std::string& oldName, const std::string& newName);
    // 从栈顶向栈底查找
    bool
    renameTopToBottom(const std::string& oldName, const std::string& newName);
    bool renameAtPosition(int position, const std::string& newName);

    // 父子栈关系
    OpStack makeChildStack();
    std::shared_ptr<OpStack> makeChildStackPtr();
    OpStack makeRootStackCopy() const;
    const OpStack* getParentStack() const
    {
        return m_parentStack;
    }

    // 仅限父子栈关系，或 forceAllow=true
    void moveTopToStack(OpStack& otherStack, bool forceAllow = false);

    void clear();

    // 原子替换栈内容并同步内存计数。调用方不应再单独修正
    // combinedStackSize，否则内容与计数很容易在异常路径上漂移。
    void replaceStackContent(const std::vector<StackElement>& newContent);

    const std::vector<StackElement>& getStackContent() const
    {
        return m_stack;
    }

    // 比较（忽略父栈关系）
    bool operator==(const OpStack& other) const;

    void stackStatus(std::string& statusStr);

private:
    void checkStackLimit(size_t additionalSize) const;
    void increaseCombinedStackSize(size_t additionalSize);
    void decreaseCombinedStackSize(size_t additionalSize);

    // 子栈构造
    OpStack(OpStack* parent);

private:
    std::vector<StackElement> m_stack;
    size_t m_maxStackSize;
    size_t m_combinedStackSize{0};
    OpStack* m_parentStack{nullptr};
};

SPACE_TBC_END
#endif // OP_STACK_H
