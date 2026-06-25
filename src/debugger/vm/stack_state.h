#ifndef STACK_STATE_H
#define STACK_STATE_H

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace apc_debug
{

struct StackElement
{
    std::vector<uint8_t> data;

    StackElement() = default;

    explicit StackElement(const std::vector<uint8_t>& bytes);
    explicit StackElement(std::vector<uint8_t>&& bytes);

    // ScriptNum 编码（小端+符号位）
    explicit StackElement(int64_t scriptNumValue);

    // 支持 0x 前缀/空白/奇数长度前补 0
    static StackElement fromHexLiteral(const std::string& hex);

    // 字符串按 UTF-8 字节写入
    static StackElement fromBytesString(const std::string& s);

    std::optional<int64_t> toInt() const;
    std::string toHexString(bool withPrefix = true) const;
    std::string toString() const;

    bool isEmpty() const
    {
        return data.empty();
    }
};

class StackState
{
public:
    StackState() = default;

    void push(const StackElement& element);
    void push(StackElement&& element);
    void pushBytes(const std::vector<uint8_t>& bytes);
    void pushBytes(std::vector<uint8_t>&& bytes);
    void pushInt(int64_t scriptNumValue);

    StackElement pop();

    const StackElement& peek(size_t depth = 0) const;

    void dup();
    void dup(size_t depth);
    void drop();
    void drop(size_t n);
    void swap();
    void rot(size_t n);
    void pick(size_t depth);
    void roll(size_t depth);

    size_t size() const
    {
        return m_stack.size();
    }

    bool empty() const
    {
        return m_stack.empty();
    }

    void clear()
    {
        m_stack.clear();
    }

    std::vector<StackElement> getAll() const
    {
        return m_stack;
    }

    std::vector<StackElement> getRange(size_t start, size_t count) const;

    std::string dump() const;
    bool validate() const;
    size_t getTotalBytes() const;

    std::vector<StackElement> snapshot() const
    {
        return m_stack;
    }

    void restore(const std::vector<StackElement>& snapshot)
    {
        m_stack = snapshot;
    }

private:
    std::vector<StackElement> m_stack; // 栈顶在末尾

    void checkDepth(size_t depth, const std::string& operation) const;
    size_t toIndex(size_t depth) const;
};

} // namespace apc_debug

#endif // STACK_STATE_H
