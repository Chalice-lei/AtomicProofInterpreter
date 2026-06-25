#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "compiler_info.h"

// 配置管理器: 编译时信息 + 可选的用户偏好配置, 提供元数据生成接口.
class ConfigManager
{
public:
    static ConfigManager& getInstance();

    // 用户配置文件缺失也视为成功.
    bool initialize(
        const std::string& userConfigPath = "user_preferences.json");

    std::string getCompilerVersion() const;
    std::string getCompilerName() const;
    std::string getCompilerDescription() const;
    std::string getGitCommitHash() const;
    std::string getGitBranch() const;
    std::time_t getBuildTimestamp() const;

    // 按点分路径取用户配置值, 例如 "output.default_log_level".
    template <typename T>
    std::optional<T> getConfigValue(const std::string& path) const;

    bool isFeatureSupported(const std::string& featureName) const;

    nlohmann::ordered_json generateMetadata(
        const std::string& sourceFile = "") const;

    bool isInitialized() const
    {
        return m_initialized;
    }

    const nlohmann::json& getRawUserConfig() const
    {
        return m_userConfig;
    }

private:
    ConfigManager() = default;
    ~ConfigManager() = default;

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    bool loadUserConfig(const std::string& filePath);

    nlohmann::json getValueByPath(const std::string& path) const;

private:
    bool m_initialized = false;
    nlohmann::json m_userConfig;
    std::time_t m_buildTimestamp;
    std::string m_userConfigPath;
};

template <typename T>
std::optional<T> ConfigManager::getConfigValue(const std::string& path) const
{
    if (!m_initialized) {
        return std::nullopt;
    }

    try {
        nlohmann::json value = getValueByPath(path);
        if (value.is_null()) {
            return std::nullopt;
        }
        return value.get<T>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

#endif // CONFIG_MANAGER_H