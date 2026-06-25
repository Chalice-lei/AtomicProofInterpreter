#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime_slot.h"

namespace apc_interpreter
{

class Environment : public std::enable_shared_from_this<Environment>
{
public:
    explicit Environment(
        std::shared_ptr<Environment> parent = nullptr,
        std::string name = ""
    );

    static std::shared_ptr<Environment> createRoot(std::string name = "global");
    std::shared_ptr<Environment> createChild(std::string name = "") const;

    const std::string& name() const
    {
        return m_name;
    }

    std::shared_ptr<Environment> parent() const
    {
        return m_parent;
    }

    void define(const std::string& name, RuntimeSlot slot);
    RuntimeSlot& assign(const std::string& name, RuntimeValue value);
    RuntimeSlot& resolve(const std::string& name);
    const RuntimeSlot& resolve(const std::string& name) const;

    RuntimeSlot* tryResolve(const std::string& name);
    const RuntimeSlot* tryResolve(const std::string& name) const;

    bool containsLocal(const std::string& name) const;
    bool contains(const std::string& name) const;

    void markConsumed(const std::string& name);
    void markDeleted(const std::string& name);
    void markKeep(const std::string& name, bool keep = true);

    std::vector<std::string> localNames() const;
    std::size_t localSize() const
    {
        return m_slots.size();
    }

private:
    RuntimeSlot& requireUsable(const std::string& name, RuntimeSlot& slot) const;
    const RuntimeSlot&
    requireUsable(const std::string& name, const RuntimeSlot& slot) const;

    std::unordered_map<std::string, RuntimeSlot> m_slots;
    std::shared_ptr<Environment> m_parent;
    std::string m_name;
};

} // namespace apc_interpreter

#endif // ENVIRONMENT_H
