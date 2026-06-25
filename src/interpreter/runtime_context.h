#ifndef RUNTIME_CONTEXT_H
#define RUNTIME_CONTEXT_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../ast/ast.h"
#include "environment.h"
#include "runtime_value.h"

namespace apc_interpreter
{

struct RuntimeCallFrame
{
    std::string functionName;
    std::shared_ptr<Environment> environment;
    SourceLocation callLocation;
};

class RuntimeContext
{
public:
    RuntimeContext();

    void registerContract(ContractNode& contract);
    void clearProgram();

    ContractNode* contract() const
    {
        return m_contract;
    }

    FunctionNode* findFunction(const std::string& name) const;
    StructDefNode* findStruct(const std::string& name) const;

    const std::unordered_map<std::string, FunctionNode*>& functions() const
    {
        return m_functions;
    }

    const std::unordered_map<std::string, StructDefNode*>& structs() const
    {
        return m_structs;
    }

    std::shared_ptr<Environment> globalEnvironment() const
    {
        return m_globalEnvironment;
    }

    void setGlobalEnvironment(std::shared_ptr<Environment> environment);

    void pushFrame(RuntimeCallFrame frame);
    RuntimeCallFrame popFrame();
    RuntimeCallFrame* currentFrame();
    const RuntimeCallFrame* currentFrame() const;

    std::size_t callDepth() const
    {
        return m_callStack.size();
    }

    void setSelf(RuntimeValue value);
    const RuntimeValue& self() const
    {
        return m_self;
    }

    void setBvm(RuntimeValue value);
    const RuntimeValue& bvm() const
    {
        return m_bvm;
    }

private:
    ContractNode* m_contract = nullptr;
    std::unordered_map<std::string, FunctionNode*> m_functions;
    std::unordered_map<std::string, StructDefNode*> m_structs;
    std::shared_ptr<Environment> m_globalEnvironment;
    std::vector<RuntimeCallFrame> m_callStack;
    RuntimeValue m_self;
    RuntimeValue m_bvm;
};

} // namespace apc_interpreter

#endif // RUNTIME_CONTEXT_H
