#include "bytecode/bytecode_builtin_function.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string cleanHex(std::string value)
{
    size_t pos = 0;
    while ((pos = value.find("0x", pos)) != std::string::npos) {
        value.erase(pos, 2);
    }
    return value;
}

std::string op(tbc::BytOpcode opcode)
{
    return cleanHex(tbc::opcodeToHex(opcode));
}

std::shared_ptr<tbc::Scope> makeScope()
{
    auto scope = std::make_shared<tbc::Scope>();
    scope->getCurrentSymtab().m_altStackPtr->clear();
    return scope;
}

void addSlot(
    const std::shared_ptr<tbc::Scope>& scope,
    const std::string& name,
    const std::string& type = "number"
)
{
    if (!scope->getCurrentSymtab().hasScopeEntry(name)) {
        require(scope->defineSymbol(name, type), "failed to define " + name);
    }
    scope->push(name, type, name);
}

std::vector<std::string> topFirst(const std::shared_ptr<tbc::Scope>& scope)
{
    std::vector<std::string> names;
    names.reserve(scope->size());
    for (size_t i = 0; i < scope->size(); ++i) {
        names.push_back(scope->at(i).getName());
    }
    return names;
}

std::vector<std::string> altTopFirst(
    const std::shared_ptr<tbc::Scope>& scope
)
{
    std::vector<std::string> names;
    const auto& alt = scope->getCurrentSymtab().m_altStackPtr;
    names.reserve(alt->size());
    for (size_t i = 0; i < alt->size(); ++i) {
        names.push_back(alt->at(i).getName());
    }
    return names;
}

std::string deleteNames(
    const std::shared_ptr<tbc::Scope>& scope,
    std::vector<std::string> names
)
{
    std::vector<tbc::StackElement> args;
    args.reserve(names.size());
    for (const auto& name : names) {
        args.emplace_back(name);
    }
    auto function =
        tbc::BuiltinFunctionFactory::createFunction("Delete", args.size());
    require(function != nullptr, "Delete builtin was not created");
    return function->getOpcodeHex(tbc::StackElement(), args, scope);
}

bool contains(const std::vector<std::string>& names, const std::string& name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

void testNonAdjacentPartner()
{
    auto scope = makeScope();
    addSlot(scope, "b");
    addSlot(scope, "live");
    addSlot(scope, "a");

    const std::string bytecode = deleteNames(scope, {"a", "b"});
    require(
        bytecode == op(tbc::BytOpcode::OP_ROT) +
                        op(tbc::BytOpcode::OP_2DROP),
        "non-adjacent pair did not use ROT 2DROP"
    );
    require(
        topFirst(scope) == std::vector<std::string>{"live"},
        "non-adjacent pair deleted a live slot"
    );
}

void testPartnerPositionUpdates()
{
    auto scope = makeScope();
    // top-first: [a, live1, b, live2, c]
    addSlot(scope, "c");
    addSlot(scope, "live2");
    addSlot(scope, "b");
    addSlot(scope, "live1");
    addSlot(scope, "a");

    const std::string bytecode = deleteNames(scope, {"a", "b", "c"});
    require(
        bytecode == op(tbc::BytOpcode::OP_ROT) +
                        op(tbc::BytOpcode::OP_2DROP) +
                        op(tbc::BytOpcode::OP_ROT) +
                        op(tbc::BytOpcode::OP_DROP),
        "{0,2,4} emitted the wrong deletion sequence"
    );
    require(
        topFirst(scope) == std::vector<std::string>({"live1", "live2"}),
        "{0,2,4} updated a remaining depth incorrectly"
    );
}

void testForwardIteratorOrder()
{
    auto scope = makeScope();
    addSlot(scope, "b");
    addSlot(scope, "a");

    require(
        deleteNames(scope, {"a", "b"}) == op(tbc::BytOpcode::OP_2DROP),
        "adjacent forward-order targets did not use 2DROP"
    );
    require(scope->empty(), "adjacent targets were not both deleted");
}

void testNipPreservesNewSymbolTracking()
{
    auto scope = makeScope();
    require(scope->defineSymbol("s", "Pair"), "failed to define struct root");
    addSlot(scope, "s.a");
    addSlot(scope, "s.b");
    addSlot(scope, "marker");

    const std::string bytecode = deleteNames(scope, {"s"});
    require(
        bytecode == op(tbc::BytOpcode::OP_NIP) +
                        op(tbc::BytOpcode::OP_NIP),
        "struct NIP sequence changed"
    );
    require(
        topFirst(scope) == std::vector<std::string>{"marker"},
        "struct NIP did not preserve marker"
    );
    const auto& tracked = scope->getCurrentSymtab().m_newSymbol;
    require(contains(tracked, "marker"), "NIP lost marker tracking");
    require(!contains(tracked, "s.a"), "NIP retained s.a tracking");
    require(!contains(tracked, "s.b"), "NIP retained s.b tracking");
}

void testSingleFieldStructOnMainAndAlt()
{
    {
        auto scope = makeScope();
        require(scope->defineSymbol("s", "Single"), "failed to define root");
        addSlot(scope, "s.x");

        require(
            deleteNames(scope, {"s"}) == op(tbc::BytOpcode::OP_DROP),
            "single-field main struct did not emit DROP"
        );
        require(scope->empty(), "single-field main struct was not deleted");
        require(
            !scope->getCurrentSymtab().hasScopeEntry("s") &&
                !scope->getCurrentSymtab().hasScopeEntry("s.x"),
            "single-field main struct metadata survived"
        );
    }

    {
        auto scope = makeScope();
        require(scope->defineSymbol("s", "Single"), "failed to define root");
        addSlot(scope, "s.x");
        auto& symtab = scope->getCurrentSymtab();
        symtab.m_altStackPtr->moveTopToStack(*symtab.m_stackPtr, true);

        require(
            deleteNames(scope, {"s"}) ==
                op(tbc::BytOpcode::OP_FROMALTSTACK) +
                    op(tbc::BytOpcode::OP_DROP),
            "single-field alt struct did not emit FROMALTSTACK DROP"
        );
        require(altTopFirst(scope).empty(), "single-field alt struct survived");
        require(
            !contains(scope->getCurrentSymtab().m_newSymbol, "s.x"),
            "single-field alt struct retained symbol tracking"
        );
    }
}

void testAllAltReturnsBytecode()
{
    auto scope = makeScope();
    addSlot(scope, "sentinel");
    addSlot(scope, "b");
    addSlot(scope, "a");
    auto& symtab = scope->getCurrentSymtab();
    symtab.m_altStackPtr->moveTopToStack(*symtab.m_stackPtr, true);
    symtab.m_altStackPtr->moveTopToStack(*symtab.m_stackPtr, true);

    const std::string bytecode = deleteNames(scope, {"a", "b"});
    require(!bytecode.empty(), "all-alt Delete discarded its bytecode");
    require(altTopFirst(scope).empty(), "all-alt Delete left target slots");
    require(
        topFirst(scope) == std::vector<std::string>{"sentinel"},
        "all-alt Delete changed the main stack"
    );
    const auto& tracked = scope->getCurrentSymtab().m_newSymbol;
    require(contains(tracked, "sentinel"), "all-alt Delete lost sentinel tracking");
    require(!contains(tracked, "a"), "all-alt Delete retained a tracking");
    require(!contains(tracked, "b"), "all-alt Delete retained b tracking");
}

void testTypeTextCannotDeleteAdjacentSlots()
{
    auto scope = makeScope();
    addSlot(scope, "target", "Struct fields=3");
    addSlot(scope, "live");

    require(
        deleteNames(scope, {"target"}) == op(tbc::BytOpcode::OP_NIP),
        "type-text target did not use one-slot NIP"
    );
    require(
        topFirst(scope) == std::vector<std::string>{"live"},
        "type-text heuristic deleted an adjacent live slot"
    );
}

void testArrayRootExpandsBracketedSlots()
{
    auto scope = makeScope();
    require(scope->defineSymbol("arr", "number[2]"), "failed to define array root");
    addSlot(scope, "arr[0x00]");
    addSlot(scope, "arr[0x51]");
    addSlot(scope, "marker");

    require(
        deleteNames(scope, {"arr"}) ==
            op(tbc::BytOpcode::OP_NIP) + op(tbc::BytOpcode::OP_NIP),
        "array root did not delete its bracketed slots"
    );
    require(
        topFirst(scope) == std::vector<std::string>{"marker"},
        "array root deletion changed an unrelated slot"
    );
    require(
        !scope->getCurrentSymtab().hasScopeEntry("arr") &&
            !scope->getCurrentSymtab().hasScopeEntry("arr[0x00]") &&
            !scope->getCurrentSymtab().hasScopeEntry("arr[0x51]"),
        "array deletion left semantic metadata behind"
    );
}

void testBoundStructRootExpandsBindingLeaves()
{
    auto scope = makeScope();
    addSlot(scope, "caller.left");
    addSlot(scope, "caller.right");
    addSlot(scope, "marker");

    std::pair<std::string, std::string> leftBinding{
        "param.left", "caller.left"
    };
    std::pair<std::string, std::string> rightBinding{
        "param.right", "caller.right"
    };
    scope->getCurrentSymtab().addBindSymbol(leftBinding);
    scope->getCurrentSymtab().addBindSymbol(rightBinding);

    require(
        deleteNames(scope, {"param"}) ==
            op(tbc::BytOpcode::OP_NIP) + op(tbc::BytOpcode::OP_NIP),
        "bound struct root did not expand its binding leaves"
    );
    require(
        topFirst(scope) == std::vector<std::string>{"marker"},
        "bound struct root deletion changed an unrelated slot"
    );
}

void testDeleteRootUsesOnlyActiveBindingFrame()
{
    auto scope = makeScope();
    addSlot(scope, "outer.oldField");
    addSlot(scope, "inner.newField");
    addSlot(scope, "marker");

    auto& symtab = scope->getCurrentSymtab();
    std::pair<std::string, std::string> outerBinding{
        "value.oldField", "outer.oldField"
    };
    symtab.addBindSymbol(outerBinding);
    symtab.beginBindSymbolFrame();
    std::pair<std::string, std::string> innerBinding{
        "value.newField", "inner.newField"
    };
    symtab.addBindSymbol(innerBinding);

    require(
        deleteNames(scope, {"value"}) == op(tbc::BytOpcode::OP_NIP),
        "active binding frame did not isolate the inner Delete root"
    );
    require(
        topFirst(scope) ==
            std::vector<std::string>({"marker", "outer.oldField"}),
        "Delete root consumed a same-named outer-frame binding"
    );
}

void testDeleteRootUsesOnlyActiveScopeFrame()
{
    auto scope = makeScope();
    addSlot(scope, "value.oldField");
    scope->getCurrentSymtab().beginScopeEntryFrame();
    addSlot(scope, "value.newField");
    addSlot(scope, "marker");

    require(
        deleteNames(scope, {"value"}) == op(tbc::BytOpcode::OP_NIP),
        "active scope frame did not isolate the inner Delete root"
    );
    require(
        topFirst(scope) ==
            std::vector<std::string>({"marker", "value.oldField"}),
        "Delete root consumed a same-named outer scope entry"
    );
}

void testDropAtTracksTheRemovedSlot()
{
    auto scope = makeScope();
    addSlot(scope, "a");
    addSlot(scope, "b");
    addSlot(scope, "c");

    scope->dropAt(1);

    require(
        topFirst(scope) == std::vector<std::string>({"c", "a"}),
        "dropAt removed the wrong physical slot"
    );
    const auto& tracked = scope->getCurrentSymtab().m_newSymbol;
    require(contains(tracked, "a"), "dropAt lost lower-slot tracking");
    require(contains(tracked, "c"), "dropAt lost top-slot tracking");
    require(!contains(tracked, "b"), "dropAt retained removed-slot tracking");
}

void testDuplicateAndAliasPreflightAreAtomic()
{
    {
        auto scope = makeScope();
        addSlot(scope, "a");
        const auto beforeStack = topFirst(scope);
        const auto beforeTracked = scope->getCurrentSymtab().m_newSymbol;
        bool rejected = false;
        try {
            (void)deleteNames(scope, {"a", "a"});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "duplicate Delete target was accepted");
        require(topFirst(scope) == beforeStack, "duplicate changed main stack");
        require(
            scope->getCurrentSymtab().m_newSymbol == beforeTracked,
            "duplicate changed symbol tracking"
        );
    }

    {
        auto scope = makeScope();
        require(scope->defineSymbol("s", "Single"), "failed to define root");
        addSlot(scope, "s.x");
        bool rejected = false;
        try {
            (void)deleteNames(scope, {"s", "s.x"});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "overlapping root/leaf targets were accepted");
        require(
            topFirst(scope) == std::vector<std::string>{"s.x"},
            "overlapping root/leaf rejection changed main stack"
        );
    }

    {
        auto scope = makeScope();
        addSlot(scope, "a");
        require(scope->defineSymbol("alias", "number"), "failed to define alias");
        std::pair<std::string, std::string> binding{"alias", "a"};
        scope->getCurrentSymtab().addBindSymbol(binding);
        const auto before = scope->captureControlFlowState();
        bool rejected = false;
        try {
            (void)deleteNames(scope, {"a", "alias"});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "aliased Delete target was accepted");
        require(
            topFirst(scope) == std::vector<std::string>{"a"},
            "alias rejection changed main stack"
        );
        require(
            scope->getCurrentSymtab().m_bindSymbol ==
                before.symbolTable.m_bindSymbol,
            "alias rejection changed bindings"
        );
    }
}

} // namespace

int main()
{
    try {
        testNonAdjacentPartner();
        testPartnerPositionUpdates();
        testForwardIteratorOrder();
        testNipPreservesNewSymbolTracking();
        testSingleFieldStructOnMainAndAlt();
        testAllAltReturnsBytecode();
        testTypeTextCannotDeleteAdjacentSlots();
        testArrayRootExpandsBracketedSlots();
        testBoundStructRootExpandsBindingLeaves();
        testDeleteRootUsesOnlyActiveBindingFrame();
        testDeleteRootUsesOnlyActiveScopeFrame();
        testDropAtTracksTheRemovedSlot();
        testDuplicateAndAliasPreflightAreAtomic();
        std::cout << "delete_builtin_function_test: all checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "delete_builtin_function_test: " << error.what() << '\n';
        return 1;
    }
}
