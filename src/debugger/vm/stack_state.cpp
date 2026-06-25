#include "stack_state.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <iomanip>
#include <sstream>

namespace apc_debug
{

static std::vector<uint8_t> parseHexToBytes(std::string hex)
{
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex = hex.substr(2);
    }

    std::string clean;
    clean.reserve(hex.size());
    for (unsigned char c : hex) {
        if (!std::isspace(c)) {
            clean.push_back(static_cast<char>(c));
        }
    }

    if (clean.empty()) {
        return {};
    }

    // 奇数长度前补 0
    if (clean.size() % 2 != 0) {
        clean.insert(clean.begin(), '0');
    }

    std::vector<uint8_t> out;
    out.reserve(clean.size() / 2);

    auto hexVal = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9')
            return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f')
            return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F')
            return static_cast<uint8_t>(c - 'A' + 10);
        return 0;
    };

    for (size_t i = 0; i < clean.size(); i += 2) {
        uint8_t hi = hexVal(clean[i]);
        uint8_t lo = hexVal(clean[i + 1]);
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

// Bitcoin Script 数字编码：小端 + 符号位；0 表示空数组
static std::vector<uint8_t> serializeScriptNum(int64_t value)
{
    if (value == 0) {
        return {};
    }

    std::vector<uint8_t> result;
    bool neg = value < 0;
    uint64_t absvalue = neg ? static_cast<uint64_t>(-value)
                            : static_cast<uint64_t>(value);

    while (absvalue) {
        result.push_back(static_cast<uint8_t>(absvalue & 0xff));
        absvalue >>= 8;
    }

    // 最高位已是 1 时需额外字节存符号
    if (result.back() & 0x80) {
        result.push_back(neg ? 0x80 : 0x00);
    } else if (neg) {
        result.back() |= 0x80;
    }

    return result;
}

StackElement::StackElement(const std::vector<uint8_t>& bytes) : data(bytes)
{}

StackElement::StackElement(std::vector<uint8_t>&& bytes)
    : data(std::move(bytes))
{}

StackElement::StackElement(int64_t scriptNumValue)
    : data(serializeScriptNum(scriptNumValue))
{}

StackElement StackElement::fromHexLiteral(const std::string& hex)
{
    return StackElement(parseHexToBytes(hex));
}

StackElement StackElement::fromBytesString(const std::string& s)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(s.size());
    bytes.insert(bytes.end(), s.begin(), s.end());
    return StackElement(std::move(bytes));
}

std::optional<int64_t> StackElement::toInt() const
{
    try {
        // 空数组等价 0
        if (data.empty()) {
            return static_cast<int64_t>(0);
        }

        // 限 8 字节避免 int64 溢出
        if (data.size() > 8) {
            return std::nullopt;
        }

        uint64_t v = 0;
        for (size_t i = 0; i < data.size(); ++i) {
            v |= (static_cast<uint64_t>(data[i]) << (8 * i));
        }

        if (data.back() & 0x80) {
            uint64_t mask = ~(
                static_cast<uint64_t>(0x80) << (8 * (data.size() - 1))
            );
            uint64_t absVal = v & mask;
            if (absVal > static_cast<uint64_t>(INT64_MAX) + 1ULL) {
                return std::nullopt;
            }
            // absVal == INT64_MAX+1 即 INT64_MIN
            if (absVal == static_cast<uint64_t>(INT64_MAX) + 1ULL) {
                return static_cast<int64_t>(INT64_MIN);
            }
            return -static_cast<int64_t>(absVal);
        }

        if (v > static_cast<uint64_t>(INT64_MAX)) {
            return std::nullopt;
        }
        return static_cast<int64_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

std::string StackElement::toHexString(bool withPrefix) const
{
    std::ostringstream oss;
    if (withPrefix) {
        oss << "0x";
    }
    oss << std::hex << std::setfill('0');
    for (uint8_t b : data) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

std::string StackElement::toString() const
{
    return toHexString(true);
}

void StackState::push(const StackElement& element)
{
    m_stack.push_back(element);
}

void StackState::push(StackElement&& element)
{
    m_stack.push_back(std::move(element));
}

void StackState::pushBytes(const std::vector<uint8_t>& bytes)
{
    m_stack.emplace_back(bytes);
}

void StackState::pushBytes(std::vector<uint8_t>&& bytes)
{
    m_stack.emplace_back(std::move(bytes));
}

void StackState::pushInt(int64_t scriptNumValue)
{
    m_stack.emplace_back(scriptNumValue);
}

StackElement StackState::pop()
{
    if (m_stack.empty()) {
        throw std::runtime_error("Stack underflow: cannot pop from empty stack"
        );
    }

    StackElement element = m_stack.back();
    m_stack.pop_back();
    return element;
}

const StackElement& StackState::peek(size_t depth) const
{
    checkDepth(depth, "peek");
    size_t index = toIndex(depth);
    return m_stack[index];
}

void StackState::dup()
{
    if (m_stack.empty()) {
        throw std::runtime_error("Stack underflow: cannot dup empty stack");
    }

    m_stack.push_back(m_stack.back());
}

void StackState::dup(size_t depth)
{
    checkDepth(depth, "dup");
    size_t index = toIndex(depth);
    m_stack.push_back(m_stack[index]);
}

void StackState::drop()
{
    if (m_stack.empty()) {
        throw std::runtime_error("Stack underflow: cannot drop from empty stack"
        );
    }

    m_stack.pop_back();
}

void StackState::drop(size_t n)
{
    if (n > m_stack.size()) {
        throw std::runtime_error(
            "Stack underflow: cannot drop " + std::to_string(n) +
            " elements from stack of size " + std::to_string(m_stack.size())
        );
    }

    m_stack.erase(m_stack.end() - n, m_stack.end());
}

void StackState::swap()
{
    if (m_stack.size() < 2) {
        throw std::runtime_error(
            "Stack underflow: need at least 2 elements to swap"
        );
    }

    std::swap(m_stack[m_stack.size() - 1], m_stack[m_stack.size() - 2]);
}

void StackState::rot(size_t n)
{
    if (n > m_stack.size()) {
        throw std::runtime_error(
            "Stack underflow: cannot rotate " + std::to_string(n) +
            " elements in stack of size " + std::to_string(m_stack.size())
        );
    }

    if (n <= 1)
        return;

    // 栈顶元素移到第 n 个位置
    auto element = m_stack.back();
    m_stack.pop_back();
    m_stack.insert(m_stack.end() - (n - 1), element);
}

void StackState::pick(size_t depth)
{
    checkDepth(depth, "pick");
    size_t index = toIndex(depth);
    m_stack.push_back(m_stack[index]);
}

void StackState::roll(size_t depth)
{
    checkDepth(depth, "roll");
    size_t index = toIndex(depth);

    auto element = m_stack[index];
    m_stack.erase(m_stack.begin() + index);
    m_stack.push_back(element);
}

std::vector<StackElement> StackState::getRange(size_t start, size_t count) const
{
    if (start >= m_stack.size()) {
        return std::vector<StackElement>();
    }

    size_t end = std::min(start + count, m_stack.size());
    return std::vector<StackElement>(
        m_stack.begin() + start, m_stack.begin() + end
    );
}

std::string StackState::dump() const
{
    std::ostringstream oss;

    oss << "Stack (size=" << m_stack.size() << "):\n";

    if (m_stack.empty()) {
        oss << "  <empty>\n";
        return oss.str();
    }

    // 从栈顶到栈底
    for (size_t i = m_stack.size(); i > 0; --i) {
        size_t depth = m_stack.size() - i;
        const auto& elem = m_stack[i - 1];

        oss << "  [" << std::setw(3) << depth << "] ";
        oss << elem.toString() << "\n";
    }

    return oss.str();
}

bool StackState::validate() const
{
    return true;
}

size_t StackState::getTotalBytes() const
{
    size_t total = 0;
    for (const auto& elem : m_stack) {
        total += elem.data.size();
    }
    return total;
}

void StackState::checkDepth(size_t depth, const std::string& operation) const
{
    if (depth >= m_stack.size()) {
        throw std::runtime_error(
            "Stack underflow in " + operation + ": depth " +
            std::to_string(depth) + " exceeds stack size " +
            std::to_string(m_stack.size())
        );
    }
}

size_t StackState::toIndex(size_t depth) const
{
    // depth 0 == 栈顶；栈顶在末尾
    return m_stack.size() - 1 - depth;
}

} // namespace apc_debug
