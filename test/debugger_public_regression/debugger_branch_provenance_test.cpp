#include "debugger/breakpoint/breakpoint_manager.h"
#include "debugger/info/debug_info.h"
#include "debugger/inspector/expression_evaluator.h"
#include "debugger/inspector/scope_inspector.h"
#include "debugger/inspector/variable_inspector.h"
#include "debugger/vm/bvm_simulator.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{

using namespace apc_debug;

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "debugger_branch_provenance_test: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

std::shared_ptr<DebugInfo> makeDebugInfo()
{
    auto info = std::make_shared<DebugInfo>();
    info->sourceFilename = "branch.ct";
    info->version = "2.0";

    auto functionScope = std::make_shared<ScopeDebugInfo>(
        "main", ScopeType::FUNCTION
    );
    functionScope->scopeId = 1;
    functionScope->setRange(0, 5);
    VariableDebugInfo expiredVariable;
    expiredVariable.name = "expired";
    expiredVariable.type = "int";
    expiredVariable.scopeName = "main";
    expiredVariable.scopeId = 1;
    expiredVariable.setAvailabilityRange(0, 4);
    functionScope->addVariable(expiredVariable);
    info->addScope(functionScope);

    auto thenScope = std::make_shared<ScopeDebugInfo>(
        "then", ScopeType::CONDITIONAL
    );
    thenScope->scopeId = 2;
    thenScope->setRange(4, 5);
    thenScope->parent = functionScope;
    VariableDebugInfo thenVariable;
    thenVariable.name = "thenOnly";
    thenVariable.type = "int";
    thenVariable.scopeName = "then";
    thenVariable.declLine = 10;
    thenVariable.declColumn = 1;
    thenVariable.isStackVar = true;
    thenVariable.stackOffset = 0;
    thenVariable.scopeId = 2;
    thenScope->addVariable(thenVariable);
    info->addVariable(thenVariable);
    functionScope->children.push_back(thenScope);
    info->addScope(thenScope);

    auto elseScope = std::make_shared<ScopeDebugInfo>(
        "else", ScopeType::CONDITIONAL
    );
    elseScope->scopeId = 3;
    elseScope->setRange(4, 5);
    elseScope->parent = functionScope;
    VariableDebugInfo elseVariable;
    elseVariable.name = "elseOnly";
    elseVariable.type = "int";
    elseVariable.scopeName = "else";
    elseVariable.declLine = 20;
    elseVariable.declColumn = 1;
    elseVariable.isStackVar = true;
    elseVariable.stackOffset = 0;
    elseVariable.scopeId = 3;
    elseScope->addVariable(elseVariable);
    info->addVariable(elseVariable);
    functionScope->children.push_back(elseScope);
    info->addScope(elseScope);

    const std::vector<std::string> opcodes{
        "OP_1", "OP_IF", "OP_ELSE", "OP_ENDIF"
    };
    for (size_t pc = 0; pc < opcodes.size(); ++pc) {
        info->addInstruction(
            pc,
            opcodes[pc],
            "",
            SourceLocation("branch.ct", pc + 1, 1)
        );
    }
    InstructionDebugInfo tail(4, SourceLocation("branch.ct", 10, 1));
    tail.opcode = "OP_1";
    SourceOrigin thenOrigin(SourceLocation("branch.ct", 10, 1));
    thenOrigin.scopeId = 2;
    thenOrigin.originalPC = 2;
    thenOrigin.path = {{1, BranchArm::Then}};
    SourceOrigin elseOrigin(SourceLocation("branch.ct", 20, 1));
    elseOrigin.scopeId = 3;
    elseOrigin.originalPC = 5;
    elseOrigin.path = {{1, BranchArm::Else}};
    tail.origins = {thenOrigin, elseOrigin};
    info->addInstruction(tail);
    info->addVariable(expiredVariable);
    return info;
}

void runPath(bool thenPath)
{
    const std::vector<std::string> bytecode{
        thenPath ? "OP_1" : "OP_0",
        "OP_IF",
        "OP_ELSE",
        "OP_ENDIF",
        "OP_1"
    };
    auto info = makeDebugInfo();
    require(info->validate(), "branch fixture is not valid V2 debug info");
    auto breakpoints = std::make_shared<BreakpointManager>(info);
    const size_t thenBreakpoint =
        breakpoints->addLineBreakpoint("branch.ct", 10);
    const size_t elseBreakpoint =
        breakpoints->addLineBreakpoint("branch.ct", 20);
    require(thenBreakpoint != 0 && elseBreakpoint != 0,
        "failed to resolve both many-to-one line breakpoints");

    BVMSimulator vm(bytecode, info);
    vm.setBreakpointManager(breakpoints);
    vm.run();

    require(vm.getState() == VMState::PAUSED && vm.getPC() == 4,
        "VM did not pause at the hoisted tail");
    require(vm.getCurrentLocation().line == (thenPath ? 10 : 20),
        "runtime trace selected the wrong source origin");
    require(
        breakpoints->getBreakpoint(thenBreakpoint)->getHitCount() ==
            (thenPath ? 1U : 0U),
        "then breakpoint crossed branch paths"
    );
    require(
        breakpoints->getBreakpoint(elseBreakpoint)->getHitCount() ==
            (thenPath ? 0U : 1U),
        "else breakpoint crossed branch paths"
    );

    const auto scope = info->getScopeAtPC(4, vm.getActiveBranchTrace());
    require(scope && scope->scopeId == (thenPath ? 2U : 3U),
        "runtime trace selected the wrong lexical scope");
    const auto variables =
        info->getVariablesInScope(4, vm.getActiveBranchTrace());
    require(variables.size() == 1 &&
                variables.front().name ==
                    (thenPath ? "thenOnly" : "elseOnly"),
        "inactive branch local leaked into variable lookup");
    require(
        info->getVariableInfo(
            thenPath ? "elseOnly" : "thenOnly",
            4,
            vm.getActiveBranchTrace()
        ) == nullptr,
        "scope lookup fell back to an inactive branch variable map copy"
    );
    require(
        info->getVariableInfo(
            "expired", 4, vm.getActiveBranchTrace()
        ) == nullptr,
        "half-open availability exposed a variable at its end PC"
    );

    const std::string activeName = thenPath ? "thenOnly" : "elseOnly";
    const std::string inactiveName = thenPath ? "elseOnly" : "thenOnly";
    const auto inspectedVariables = vm.getScopeInspector()->getVisibleVariables(
        4, vm.getActiveBranchTrace()
    );
    require(
        inspectedVariables.size() == 1 &&
            inspectedVariables.front().name == activeName,
        "ScopeInspector ignored the active branch trace"
    );

    StackState inspectorStack;
    inspectorStack.pushInt(7);
    require(
        vm.getVariableInspector()
                ->readVariable(
                    activeName,
                    inspectorStack,
                    4,
                    vm.getActiveBranchTrace()
                )
                .has_value() &&
            !vm.getVariableInspector()
                 ->readVariable(
                     inactiveName,
                     inspectorStack,
                     4,
                     vm.getActiveBranchTrace()
                 )
                 .has_value(),
        "VariableInspector crossed branch paths"
    );
    require(
        vm.getExpressionEvaluator()
                ->evaluate(
                    activeName,
                    inspectorStack,
                    4,
                    vm.getActiveBranchTrace()
                )
                .success &&
            !vm.getExpressionEvaluator()
                 ->evaluate(
                     inactiveName,
                     inspectorStack,
                     4,
                     vm.getActiveBranchTrace()
                 )
                 .success,
        "ExpressionEvaluator crossed branch paths"
    );
}

} // namespace

int main()
{
    runPath(true);
    runPath(false);
    std::cout << "debugger_branch_provenance_test: PASS\n";
    return 0;
}
