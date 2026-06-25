#ifndef PASS_H
#define PASS_H

#include <functional>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

class PassContext;

class Pass
{
public:
    virtual ~Pass() = default;
    virtual void execute(PassContext& data) = 0;

    virtual std::string getName() const = 0;
    virtual std::vector<std::string> getDependencies() const
    {
        return {};
    }

    virtual void initialize()
    {}
    virtual void finalize()
    {}
};

#endif
