#ifndef PASS_CONTEXT_H
#define PASS_CONTEXT_H

#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>

struct PairHash
{
    template <typename T1, typename T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const
    {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 << 1);
    }
};

struct PairEqual
{
    template <typename T1, typename T2>
    bool operator()(const std::pair<T1, T2>& lhs,
                    const std::pair<T1, T2>& rhs) const
    {
        return lhs.first == rhs.first && lhs.second == rhs.second;
    }
};

class PassContext
{
public:
    explicit PassContext() = default;

    size_t size() const
    {
        return m_data.size();
    }

    template <typename T>
    void set(const std::string& key, std::shared_ptr<T> value)
    {
        m_data[std::make_pair(key, std::type_index(typeid(T)))] = value;
    }

    // 强类型获取，缺失抛异常
    template <typename T>
    std::shared_ptr<T> get(const std::string& key) const
    {
        auto it = m_data.find(std::make_pair(key, std::type_index(typeid(T))));
        if (it == m_data.end()) {
            throw std::runtime_error("Data not found for key: " + key +
                                     " with specified type");
        }
        return std::static_pointer_cast<T>(it->second);
    }

    // 缺失返回 nullptr
    template <typename T>
    std::shared_ptr<T> tryGet(const std::string& key) const noexcept
    {
        auto it = m_data.find(std::make_pair(key, std::type_index(typeid(T))));
        return it != m_data.end() ? std::static_pointer_cast<T>(it->second)
                                  : nullptr;
    }

    template <typename T>
    bool contains(const std::string& key) const noexcept
    {
        return m_data.count(std::make_pair(key, std::type_index(typeid(T)))) >
               0;
    }

private:
    std::unordered_map<std::pair<std::string, std::type_index>,
                       std::shared_ptr<void>,
                       PairHash,
                       PairEqual>
        m_data;
};

#endif // PASS_CONTEXT_H