#ifndef BUILTIN_UTILS_STRUCT_H
#define BUILTIN_UTILS_STRUCT_H

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

#include "byt_defs.h"
#include "bytecode_helper_fun.h"
#include "bytecode_opcodes.h"

#define CODE_TEMP(PassClass)                                                   \
public:                                                                        \
    std::string toString() const override                                      \
    {                                                                          \
        return #PassClass;                                                     \
    }                                                                          \
                                                                               \
    bool isConsistent(const std::string& str) const override                   \
    {                                                                          \
        if (#PassClass == str) {                                               \
            return true;                                                       \
        }                                                                      \
        return false;                                                          \
    }

SPACE_TBC_START
class Base
{
public:
    virtual ~Base() = default;
    virtual std::string toString() const = 0;
    virtual bool isConsistent(const std::string& str) const = 0;
    virtual std::string getOpcodeHex(const std::string& memberStr) const = 0;
};

class Self : public Base
{
public:
    Self(std::string str) : m_baseStr{str}
    {}
    std::string toString() const override
    {
        return "self";
    }

    bool isConsistent(const std::string& str) const override
    {
        if ("self" == str || "<self>" == str) {
            return true;
        }
        return false;
    }
    std::string getOpcodeHex(const std::string& memberStr) const override
    {
        auto baseStr = m_baseStr;
        if (baseStr.empty()) {
            return "<" + memberStr + ">";
        }
        if ('>' == baseStr.back()) {
            return baseStr.insert(baseStr.size() - 1, "." + memberStr);
        }
        return baseStr + "." + memberStr + ">";
    }

private:
    std::string m_baseStr;
};

class BVM : public Base
{
public:
    CODE_TEMP(BVM)
    std::string getOpcodeHex(const std::string& memberStr) const override
    {
        static const std::unordered_map<std::string, int> memberMap = {
            {"version", 1},
            {"locktime", 2},
            {"inputCount", 3},
            {"outputCount", 4},
            {"inputsHash", 5},
            {"unlockingInput", 6},
            {"outputsHash", 7},
        };

        int memberValue = 8; // 默认
        auto it = memberMap.find(memberStr);
        if (it != memberMap.end()) {
            memberValue = it->second;
        }
        if (8 == memberValue) {
            throw std::runtime_error("BVM member not found");
        }

        std::ostringstream oss;
        oss << numberToScriptHex(memberValue);
        oss << opcodeToHex(BytOpcode::OP_PUSH_META);
        return oss.str();
    }
};

static std::unordered_map<std::string,
                          std::shared_ptr<Base> (*)(std::string str)>
    g_builtinStructMap = {
        {
            "self",
            [](std::string str) -> std::shared_ptr<Base> {
                return std::make_shared<Self>(str);
            },
        },
        {
            "<self>",
            [](std::string str) -> std::shared_ptr<Base> {
                return std::make_shared<Self>(str);
            },
        },
        {
            "BVM",
            [](std::string) -> std::shared_ptr<Base> {
                return std::make_shared<BVM>();
            },
        },
};
class BuiltinUtilsStruct
{
public:
    explicit BuiltinUtilsStruct()
    {}
    ~BuiltinUtilsStruct() = default;

    static std::shared_ptr<Base> createBuiltStructOjb(const std::string& str)
    {
        auto searchStr = str;
        if (str.starts_with("<self")) {
            searchStr = "self";
        }
        auto it = g_builtinStructMap.find(searchStr);
        if (it != g_builtinStructMap.end()) {
            return it->second(str);
        }
        return nullptr;
    }

    static bool isBuiltinStructOjb(const std::string& str)
    {
        if (g_builtinStructMap.find(str) != g_builtinStructMap.end()) {
            return true;
        }
        return false;
    }

private:
};

SPACE_TBC_END
#endif