#ifndef COMPILER_PLACEHOLDER_H
#define COMPILER_PLACEHOLDER_H

#include <atomic>
#include <cstring>
#include <string>

namespace tbc
{

// 编译器栈占位符。每个实例持有唯一 ID 便于调试。
class CompilerPlaceholder
{
private:
    int m_id;
    static std::atomic<int> s_nextId;

    static constexpr const char* PREFIX = "/CompilerPlaceholder_";
    static constexpr const char* SUFFIX = "/";

public:
    CompilerPlaceholder() : m_id(s_nextId.fetch_add(1))
    {}

    explicit CompilerPlaceholder(int id) : m_id(id)
    {}

    int getId() const
    {
        return m_id;
    }

    // 格式 "/CompilerPlaceholder_<id>/"
    std::string toString() const
    {
        return PREFIX + std::to_string(m_id) + SUFFIX;
    }

    static bool isPlaceholder(const std::string& str)
    {
        if (str.empty()) {
            return false;
        }

        const size_t prefixLen = std::strlen(PREFIX);
        const size_t suffixLen = std::strlen(SUFFIX);

        if (str.size() <= prefixLen + suffixLen) {
            return false;
        }

        return str.compare(0, prefixLen, PREFIX) == 0 &&
               str.compare(str.size() - suffixLen, suffixLen, SUFFIX) == 0;
    }

    bool operator==(const CompilerPlaceholder& other) const
    {
        return m_id == other.m_id;
    }

    bool operator!=(const CompilerPlaceholder& other) const
    {
        return m_id != other.m_id;
    }

    bool operator<(const CompilerPlaceholder& other) const
    {
        return m_id < other.m_id;
    }

    operator std::string() const
    {
        return toString();
    }
};

} // namespace tbc

#endif // COMPILER_PLACEHOLDER_H
