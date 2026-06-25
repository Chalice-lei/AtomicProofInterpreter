#include "runtime_context.h"

#include <utility>

#include "runtime_error.h"

namespace apc_interpreter
{

RuntimeContext::RuntimeContext()
    : m_globalEnvironment(Environment::createRoot()),
      m_self(RuntimeValue::fromStruct({}, "self")),
      m_bvm(RuntimeValue::fromBuiltinObject("BVM"))
{}

void RuntimeContext::registerContract(ContractNode& contract)
{
    clearProgram();
    m_contract = &contract;

    for (auto& member : contract.members) {
        if (auto* function = dynamic_cast<FunctionNode*>(member.get())) {
            m_functions[function->name] = function;
        } else if (auto* structDef = dynamic_cast<StructDefNode*>(member.get())) {
            m_structs[structDef->name] = structDef;
        }
    }
}

void RuntimeContext::clearProgram()
{
    m_contract = nullptr;
    m_functions.clear();
    m_structs.clear();
    m_callStack.clear();
}

FunctionNode* RuntimeContext::findFunction(const std::string& name) const
{
    auto it = m_functions.find(name);
    return it == m_functions.end() ? nullptr : it->second;
}

StructDefNode* RuntimeContext::findStruct(const std::string& name) const
{
    auto it = m_structs.find(name);
    return it == m_structs.end() ? nullptr : it->second;
}

void RuntimeContext::setGlobalEnvironment(std::shared_ptr<Environment> environment)
{
    if (!environment) {
        throw RuntimeError(
            RuntimeErrorKind::Generic,
            "global environment cannot be null"
        );
    }
    m_globalEnvironment = std::move(environment);
}

void RuntimeContext::pushFrame(RuntimeCallFrame frame)
{
    m_callStack.push_back(std::move(frame));
}

RuntimeCallFrame RuntimeContext::popFrame()
{
    if (m_callStack.empty()) {
        throw RuntimeError(RuntimeErrorKind::Generic, "call stack is empty");
    }

    RuntimeCallFrame frame = std::move(m_callStack.back());
    m_callStack.pop_back();
    return frame;
}

RuntimeCallFrame* RuntimeContext::currentFrame()
{
    return m_callStack.empty() ? nullptr : &m_callStack.back();
}

const RuntimeCallFrame* RuntimeContext::currentFrame() const
{
    return m_callStack.empty() ? nullptr : &m_callStack.back();
}

void RuntimeContext::setSelf(RuntimeValue value)
{
    m_self = std::move(value);
}

void RuntimeContext::setBvm(RuntimeValue value)
{
    m_bvm = std::move(value);
}

} // namespace apc_interpreter
