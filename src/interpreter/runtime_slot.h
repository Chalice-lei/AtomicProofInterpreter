#ifndef RUNTIME_SLOT_H
#define RUNTIME_SLOT_H

#include <string>
#include <utility>

#include "runtime_value.h"

namespace apc_interpreter
{

enum class OwnershipState {
    Available,
    Consumed,
    Deleted
};

enum class StorageClass {
    MainStack,
    AltStack,
    Fixed,
    SelfMember,
    BuiltinObject
};

struct RuntimeSlot
{
    RuntimeValue value;
    std::string declaredType;
    OwnershipState ownership = OwnershipState::Available;
    StorageClass storage = StorageClass::MainStack;
    bool keepOnScopeExit = false;

    RuntimeSlot() = default;

    RuntimeSlot(
        RuntimeValue value,
        std::string declaredType = "",
        StorageClass storage = StorageClass::MainStack
    )
        : value(std::move(value)),
          declaredType(std::move(declaredType)),
          storage(storage)
    {}

    bool isAvailable() const
    {
        return ownership == OwnershipState::Available;
    }

    bool isConsumed() const
    {
        return ownership == OwnershipState::Consumed;
    }

    bool isDeleted() const
    {
        return ownership == OwnershipState::Deleted;
    }
};

std::string ownershipStateName(OwnershipState state);
std::string storageClassName(StorageClass storage);

} // namespace apc_interpreter

#endif // RUNTIME_SLOT_H
