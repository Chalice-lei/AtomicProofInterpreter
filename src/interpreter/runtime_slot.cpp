#include "runtime_slot.h"

#include <utility>

namespace apc_interpreter
{

std::string ownershipStateName(OwnershipState state)
{
    switch (state) {
        case OwnershipState::Available:
            return "available";
        case OwnershipState::Consumed:
            return "consumed";
        case OwnershipState::Deleted:
            return "deleted";
    }
    return "unknown";
}

std::string storageClassName(StorageClass storage)
{
    switch (storage) {
        case StorageClass::MainStack:
            return "main_stack";
        case StorageClass::AltStack:
            return "alt_stack";
        case StorageClass::Fixed:
            return "fixed";
        case StorageClass::SelfMember:
            return "self_member";
        case StorageClass::BuiltinObject:
            return "builtin_object";
    }
    return "unknown";
}

} // namespace apc_interpreter
