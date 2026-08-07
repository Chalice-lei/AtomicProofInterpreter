#include <cstdlib>
#include <iostream>
#include <string>

#include "bytecode/scope.h"
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

void requireShadowedStackRename(
    const tbc::OpStack& stack,
    const std::string& outerName,
    const std::string& innerName,
    const std::string& context
)
{
    const auto& content = stack.getStackContent();
    require(content.size() == 2, context + " changed stack depth");
    require(
        content[0].getName() == outerName,
        context + " renamed the shadowed outer slot"
    );
    require(
        content[1].getName() == innerName,
        context + " did not rename the innermost slot"
    );
}

void testShadowSafeRenames()
{
    tbc::Scope scope;
    auto& scopeSymbols = scope.getCurrentSymtab();
    scopeSymbols.m_stackPtr->push(tbc::StackElement("value", "int", "01"));
    scopeSymbols.m_stackPtr->push(tbc::StackElement("value", "int", "02"));
    require(scope.rename("value", "inner"), "Scope::rename failed");
    requireShadowedStackRename(
        *scopeSymbols.m_stackPtr, "value", "inner", "Scope::rename"
    );

    tbc::SymbolTable scalar;
    require(scalar.defineSymbol("value", "int"), "outer scalar declaration failed");
    scalar.m_stackPtr->push(tbc::StackElement("value", "int", "01"));
    scalar.m_declaredSymbols.clear();
    require(scalar.defineSymbol("value", "int"), "inner scalar declaration failed");
    scalar.m_stackPtr->push(tbc::StackElement("value", "int", "02"));
    require(
        scalar.renameSymbolEntry("value", "result"),
        "shadowed scalar rename failed"
    );
    require(
        scalar.m_currentScope.size() == 2 &&
            scalar.m_currentScope[0].first == "value" &&
            scalar.m_currentScope[1].first == "result",
        "scalar rename selected the wrong lexical entry"
    );
    requireShadowedStackRename(
        *scalar.m_stackPtr, "value", "result", "renameSymbolEntry"
    );

    tbc::SymbolTable array;
    require(array.defineArray("items", "int", 1), "outer array declaration failed");
    const std::string itemLabel = array.getArrayElementLabel("items", 0);
    array.m_stackPtr->push(tbc::StackElement(itemLabel, "int", "01"));
    array.m_declaredSymbols.clear();
    require(array.defineArray("items", "int", 1), "inner array declaration failed");
    array.m_stackPtr->push(tbc::StackElement(itemLabel, "int", "02"));
    require(
        array.renameArraySymbol("items", "selected"),
        "shadowed array rename failed"
    );
    require(
        array.m_currentScope.size() == 2 &&
            array.m_currentScope[0].first == "items" &&
            array.m_currentScope[1].first == "selected",
        "array rename selected the wrong lexical entry"
    );
    requireShadowedStackRename(
        *array.m_stackPtr,
        itemLabel,
        array.m_currentScope[1].second.getArrayElementLabel(0),
        "renameArraySymbol"
    );

    tbc::SymbolTable compound;
    const tbc::CompoundTypeInfo record("record", {});
    compound.m_currentScope.emplace_back("record", tbc::SymbolInfo(record));
    compound.m_stackPtr->push(tbc::StackElement("record", "Pair", "01"));
    compound.m_currentScope.emplace_back("record", tbc::SymbolInfo(record));
    compound.m_declaredSymbols.push_back("record");
    compound.m_stackPtr->push(tbc::StackElement("record", "Pair", "02"));
    require(
        compound.renameCompoundSymbol("record", "chosen"),
        "shadowed compound rename failed"
    );
    require(
        compound.m_currentScope.size() == 2 &&
            compound.m_currentScope[0].first == "record" &&
            compound.m_currentScope[1].first == "chosen",
        "compound rename selected the wrong lexical entry"
    );
    requireShadowedStackRename(
        *compound.m_stackPtr, "record", "chosen", "renameCompoundSymbol"
    );

    tbc::SymbolTable fields;
    fields.m_currentScope.emplace_back(
        "record.left", tbc::SymbolInfo("record.left", "int")
    );
    fields.m_currentScope.emplace_back(
        "chosen.left", tbc::SymbolInfo("chosen.left", "int")
    );
    fields.m_currentScope.emplace_back(
        "record.left", tbc::SymbolInfo("record.left", "int")
    );
    fields.m_currentScope.emplace_back(
        "chosen.left", tbc::SymbolInfo("chosen.left", "int")
    );
    fields.m_stackPtr->push(
        tbc::StackElement("record.left", "int", "01")
    );
    fields.m_stackPtr->push(
        tbc::StackElement("record.left", "int", "02")
    );
    fields.renameEntriesByPrefix("record", "chosen");
    requireShadowedStackRename(
        *fields.m_stackPtr,
        "record.left",
        "chosen.left",
        "renameEntriesByPrefix"
    );
    size_t oldFieldCount = 0;
    size_t newFieldCount = 0;
    for (const auto& [name, info] : fields.m_currentScope) {
        (void)info;
        oldFieldCount += name == "record.left" ? 1U : 0U;
        newFieldCount += name == "chosen.left" ? 1U : 0U;
    }
    require(
        oldFieldCount == 1 && newFieldCount == 2,
        "prefix rename removed or renamed an outer lexical field"
    );
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

    testShadowSafeRenames();

    return 0;
}
