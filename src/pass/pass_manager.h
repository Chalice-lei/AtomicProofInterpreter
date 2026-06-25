#ifndef PASS_MANAGER_H
#define PASS_MANAGER_H

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pass.h"

class PassManager
{
public:
    // 名称重复时抛 std::invalid_argument
    void registerPass(const std::string& name,
                      std::function<std::unique_ptr<Pass>()> factory);

    // 未注册返回 false
    bool enablePass(const std::string& name, bool enable = true);

    // 依赖解析失败抛 std::runtime_error
    void run(PassContext& data);

    void setConfig(const std::string& key, const std::string& value);

    // 不存在返回空串
    std::string getConfig(const std::string& key) const;

    bool isPassRegistered(const std::string& name) const;
    bool isPassEnabled(const std::string& name) const;

private:
    // 循环或缺失依赖时抛 std::runtime_error
    void resolveDependencies();

private:
    std::vector<std::unique_ptr<Pass>> m_passes;
    std::unordered_map<std::string, std::function<std::unique_ptr<Pass>()>>
        m_passRegistry;
    std::unordered_map<std::string, bool> m_enabledPasses;
    std::unordered_map<std::string, std::string> m_config;
};

#endif // PASS_MANAGER_H
