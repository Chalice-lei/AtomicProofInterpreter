#include <cstdlib>
#include <iostream>
#include <string>

#include "bytecode/symtab.h"

namespace
{
[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "symbol-table state regression failed: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

size_t contentSize(const tbc::OpStack& stack)
{
    size_t size = 0;
    for (const auto& element : stack.getStackContent()) {
        size += element.getMemoryUsage();
    }
    return size;
}
} // namespace

int main()
{
    tbc::SymbolTable firstSession;
    tbc::SymbolTable secondSession;
    firstSession.m_altStackPtr->push(tbc::StackElement("value", "int", "01"));

    require(
        secondSession.m_altStackPtr->empty(),
        "independent compiler sessions share the alternative stack"
    );

    tbc::SymbolTable branchSnapshot(firstSession);
    require(
        branchSnapshot.m_altStackPtr == firstSession.m_altStackPtr,
        "scope snapshots do not share their session alternative stack"
    );
    firstSession.resetFunctionState();
    require(
        !firstSession.m_altStackPtr->empty(),
        "function reset discarded the public-function handoff state"
    );

    firstSession.clearSharedAltStack();
    require(
        firstSession.m_altStackPtr->empty(),
        "session alternative stack was not cleared"
    );
    require(
        secondSession.m_altStackPtr->empty(),
        "clearing one session changed another session"
    );

    tbc::OpStack stack;
    stack.push(tbc::StackElement("first", "int", "01"));
    stack.push(tbc::StackElement("second", "int", "0102"));
    stack.replaceStackContent(
        {tbc::StackElement("replacement", "int", "010203")}
    );
    require(
        stack.getCombinedStackSize() == contentSize(stack),
        "replaceStackContent left stale memory accounting"
    );

    tbc::OpStack parentStack;
    parentStack.push(tbc::StackElement("parent", "int", "01"));
    auto childStack = parentStack.makeChildStackPtr();
    childStack->push(tbc::StackElement("child", "int", "02"));
    childStack->replaceStackContent(
        {tbc::StackElement("larger_child", "int", "0203")}
    );
    require(
        parentStack.getCombinedStackSize() ==
            contentSize(parentStack) + contentSize(*childStack),
        "child replaceStackContent did not update the root accounting"
    );

    std::string validationError;
    require(
        firstSession.validateState(&validationError),
        "valid session state rejected: " + validationError
    );

    return 0;
}
