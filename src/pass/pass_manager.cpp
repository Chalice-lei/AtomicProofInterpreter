#include "pass_manager.h"

void PassManager::registerPass(const std::string& name,
                               std::function<std::unique_ptr<Pass>()> factory)
{
    if (m_passRegistry.find(name) != m_passRegistry.end()) {
        throw std::invalid_argument("Pass '" + name + "' already registered");
    }
    m_passRegistry[name] = std::move(factory);
    m_enabledPasses[name] = true;
}

bool PassManager::enablePass(const std::string& name, bool enable /*= true*/)
{
    if (m_passRegistry.find(name) == m_passRegistry.end()) {
        return false;
    }
    m_enabledPasses[name] = enable;
    return true;
}

void PassManager::run(PassContext& data)
{
    m_passes.clear();

    for (const auto& [name, enabled] : m_enabledPasses) {
        if (enabled && m_passRegistry.find(name) != m_passRegistry.end()) {
            m_passes.push_back(m_passRegistry[name]());
        }
    }

    resolveDependencies();

    for (auto& pass : m_passes) {
        pass->initialize();
        pass->execute(data);
        pass->finalize();
    }
}

void PassManager::setConfig(const std::string& key, const std::string& value)
{
    m_config[key] = value;
}

std::string PassManager::getConfig(const std::string& key) const
{
    auto it = m_config.find(key);
    return it != m_config.end() ? it->second : "";
}

bool PassManager::isPassRegistered(const std::string& name) const
{
    return m_passRegistry.find(name) != m_passRegistry.end();
}

bool PassManager::isPassEnabled(const std::string& name) const
{
    auto it = m_enabledPasses.find(name);
    return it != m_enabledPasses.end() ? it->second : false;
}

void PassManager::resolveDependencies()
{
    std::vector<std::unique_ptr<Pass>> sortedPasses;
    std::unordered_map<std::string, bool> visited;
    std::unordered_map<std::string, bool> onStack;
    std::unordered_map<std::string, size_t> passIndices;

    for (size_t i = 0; i < m_passes.size(); ++i) {
        passIndices[m_passes[i]->getName()] = i;
    }

    auto dfs = [&](const auto& self, const std::string& passName) -> void {
        if (onStack[passName]) {
            throw std::runtime_error(
                "Circular dependency detected involving pass: " + passName);
        }

        if (visited[passName])
            return;

        visited[passName] = true;
        onStack[passName] = true;

        auto it = passIndices.find(passName);
        if (it != passIndices.end()) {
            size_t idx = it->second;
            if (idx < m_passes.size() && m_passes[idx]) {
                for (const auto& dep : m_passes[idx]->getDependencies()) {
                    if (!isPassRegistered(dep)) {
                        throw std::runtime_error("Missing dependency: " + dep +
                                                 " for pass: " + passName);
                    }
                    self(self, dep);
                }

                // 避免重复加入 sortedPasses
                bool alreadyAdded = std::any_of(sortedPasses.begin(),
                                                sortedPasses.end(),
                                                [&passName](const auto& p) {
                                                    return p->getName() ==
                                                           passName;
                                                });

                if (!alreadyAdded) {
                    sortedPasses.push_back(std::move(m_passes[idx]));
                }
            }
        }

        onStack[passName] = false;
    };

    for (size_t i = 0; i < m_passes.size(); ++i) {
        if (m_passes[i]) {
            dfs(dfs, m_passes[i]->getName());
        }
    }

    m_passes = std::move(sortedPasses);
}
