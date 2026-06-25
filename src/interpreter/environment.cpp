#include "environment.h"

#include <algorithm>
#include <utility>

#include "runtime_error.h"

namespace apc_interpreter
{

Environment::Environment(std::shared_ptr<Environment> parent, std::string name)
    : m_parent(std::move(parent)), m_name(std::move(name))
{}

std::shared_ptr<Environment> Environment::createRoot(std::string name)
{
    return std::make_shared<Environment>(nullptr, std::move(name));
}

std::shared_ptr<Environment> Environment::createChild(std::string name) const
{
    return std::make_shared<Environment>(
        const_cast<Environment*>(this)->shared_from_this(),
        std::move(name)
    );
}

void Environment::define(const std::string& name, RuntimeSlot slot)
{
    if (name.empty()) {
        throw RuntimeError(
            RuntimeErrorKind::UndefinedVariable,
            "cannot define an empty variable name"
        );
    }

    if (containsLocal(name)) {
        throw RuntimeError(
            RuntimeErrorKind::DuplicateVariable,
            "variable '" + name + "' is already defined in this environment"
        );
    }

    if (slot.declaredType.empty()) {
        slot.declaredType = slot.value.declaredType();
    }
    m_slots.emplace(name, std::move(slot));
}

RuntimeSlot& Environment::assign(const std::string& name, RuntimeValue value)
{
    RuntimeSlot& slot = resolve(name);
    slot.value = std::move(value);
    slot.ownership = OwnershipState::Available;
    if (slot.declaredType.empty()) {
        slot.declaredType = slot.value.declaredType();
    }
    return slot;
}

RuntimeSlot& Environment::resolve(const std::string& name)
{
    if (auto* slot = tryResolve(name)) {
        return requireUsable(name, *slot);
    }

    throw RuntimeError(
        RuntimeErrorKind::UndefinedVariable,
        "variable '" + name + "' is not defined"
    );
}

const RuntimeSlot& Environment::resolve(const std::string& name) const
{
    if (auto* slot = tryResolve(name)) {
        return requireUsable(name, *slot);
    }

    throw RuntimeError(
        RuntimeErrorKind::UndefinedVariable,
        "variable '" + name + "' is not defined"
    );
}

RuntimeSlot* Environment::tryResolve(const std::string& name)
{
    auto it = m_slots.find(name);
    if (it != m_slots.end()) {
        return &it->second;
    }

    return m_parent ? m_parent->tryResolve(name) : nullptr;
}

const RuntimeSlot* Environment::tryResolve(const std::string& name) const
{
    auto it = m_slots.find(name);
    if (it != m_slots.end()) {
        return &it->second;
    }

    return m_parent ? m_parent->tryResolve(name) : nullptr;
}

bool Environment::containsLocal(const std::string& name) const
{
    return m_slots.find(name) != m_slots.end();
}

bool Environment::contains(const std::string& name) const
{
    return tryResolve(name) != nullptr;
}

void Environment::markConsumed(const std::string& name)
{
    resolve(name).ownership = OwnershipState::Consumed;
}

void Environment::markDeleted(const std::string& name)
{
    RuntimeSlot* slot = tryResolve(name);
    if (!slot) {
        throw RuntimeError(
            RuntimeErrorKind::UndefinedVariable,
            "variable '" + name + "' is not defined"
        );
    }
    slot->ownership = OwnershipState::Deleted;
}

void Environment::markKeep(const std::string& name, bool keep)
{
    resolve(name).keepOnScopeExit = keep;
}

std::vector<std::string> Environment::localNames() const
{
    std::vector<std::string> names;
    names.reserve(m_slots.size());
    for (const auto& [name, _] : m_slots) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

RuntimeSlot&
Environment::requireUsable(const std::string& name, RuntimeSlot& slot) const
{
    if (slot.ownership == OwnershipState::Consumed) {
        throw RuntimeError(
            RuntimeErrorKind::OwnershipViolation,
            "variable '" + name + "' has been consumed"
        );
    }
    if (slot.ownership == OwnershipState::Deleted) {
        throw RuntimeError(
            RuntimeErrorKind::OwnershipViolation,
            "variable '" + name + "' has been deleted"
        );
    }
    return slot;
}

const RuntimeSlot& Environment::requireUsable(
    const std::string& name,
    const RuntimeSlot& slot
) const
{
    if (slot.ownership == OwnershipState::Consumed) {
        throw RuntimeError(
            RuntimeErrorKind::OwnershipViolation,
            "variable '" + name + "' has been consumed"
        );
    }
    if (slot.ownership == OwnershipState::Deleted) {
        throw RuntimeError(
            RuntimeErrorKind::OwnershipViolation,
            "variable '" + name + "' has been deleted"
        );
    }
    return slot;
}

} // namespace apc_interpreter
