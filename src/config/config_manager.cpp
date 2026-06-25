#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "config_manager.h"
#include "../log/logger.h"

ConfigManager& ConfigManager::getInstance()
{
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::initialize(const std::string& userConfigPath)
{
    m_userConfigPath = userConfigPath;
    m_buildTimestamp = std::time(nullptr);

    LOG_INFO("Initializing ConfigManager");
    LOG_DEBUG("User config path: ", userConfigPath);

    loadUserConfig(userConfigPath);

    m_initialized = true;
    LOG_INFO("ConfigManager initialized successfully");
    LOG_DEBUG("Compiler version: ",
              getCompilerVersion(),
              " (from CompilerInfo)");
    LOG_DEBUG("Git commit hash: ", getGitCommitHash());
    LOG_DEBUG("Git branch: ", getGitBranch());

    return true;
}

std::string ConfigManager::getCompilerVersion() const
{
    return TBC::CompilerInfo::VERSION;
}

std::string ConfigManager::getCompilerName() const
{
    return TBC::CompilerInfo::NAME;
}

std::string ConfigManager::getCompilerDescription() const
{
    return TBC::CompilerInfo::DESCRIPTION;
}

std::string ConfigManager::getGitCommitHash() const
{
    return TBC::CompilerInfo::BUILD_GIT_COMMIT_HASH;
}

std::string ConfigManager::getGitBranch() const
{
    return TBC::CompilerInfo::BUILD_GIT_BRANCH_NAME;
}

std::time_t ConfigManager::getBuildTimestamp() const
{
    return m_buildTimestamp;
}

bool ConfigManager::isFeatureSupported(const std::string& featureName) const
{
    return TBC::CompilerInfo::isFeatureSupported(featureName);
}

nlohmann::ordered_json ConfigManager::generateMetadata(
    const std::string& sourceFile) const
{
    nlohmann::ordered_json metadata;

    // 编译时信息
    metadata["compiler_name"] = TBC::CompilerInfo::NAME;
    metadata["compiler_version"] = TBC::CompilerInfo::VERSION;
    metadata["compiler_description"] = TBC::CompilerInfo::DESCRIPTION;

    metadata["build_timestamp"] = getBuildTimestamp();

    std::string gitCommitHash = getGitCommitHash();
    std::string gitBranch = getGitBranch();
    if (!gitCommitHash.empty() && gitCommitHash != "unknown") {
        metadata["git_commit_hash"] = gitCommitHash;
    }
    if (!gitBranch.empty() && gitBranch != "unknown") {
        metadata["git_branch"] = gitBranch;
    }

    if (!sourceFile.empty()) {
        metadata["source_file"] = sourceFile;
    }

    auto capabilities = nlohmann::ordered_json::object();
    capabilities["ast_validation"] = TBC::CompilerInfo::SUPPORTS_AST_VALIDATION;
    capabilities["bytecode_optimization"] =
        TBC::CompilerInfo::SUPPORTS_BYTECODE_OPTIMIZATION;
    capabilities["script_verification"] =
        TBC::CompilerInfo::SUPPORTS_SCRIPT_VERIFICATION;
    metadata["compiler_capabilities"] = capabilities;

    // 用户偏好 (若有)
    if (!m_userConfig.empty()) {
        auto userPrefs = nlohmann::ordered_json::object();

        if (m_userConfig.contains("output")) {
            auto output = m_userConfig["output"];
            if (output.contains("include_metadata")) {
                userPrefs["include_metadata"] = output["include_metadata"];
            }
            if (output.contains("json_indent")) {
                userPrefs["json_indent"] = output["json_indent"];
            }
        }

        if (!userPrefs.empty()) {
            metadata["user_preferences"] = userPrefs;
        }
    }

    return metadata;
}

bool ConfigManager::loadUserConfig(const std::string& filePath)
{
    try {
        // 用户配置文件可选
        if (!std::filesystem::exists(filePath)) {
            LOG_INFO("User config file not found: ",
                     filePath,
                     " (this is optional)");
            m_userConfig = nlohmann::json::object();
            return true;
        }

        std::ifstream configFile(filePath);
        if (!configFile.is_open()) {
            LOG_WARNING("Failed to open user config file: ",
                        filePath,
                        " (continuing without user config)");
            m_userConfig = nlohmann::json::object();
            return true; // 失败不影响整体初始化
        }

        configFile >> m_userConfig;
        configFile.close();

        LOG_DEBUG("User config file loaded successfully: ", filePath);
        return true;

    } catch (const nlohmann::json::exception& e) {
        LOG_WARNING("JSON parsing error in user config file ",
                    filePath,
                    ": ",
                    e.what(),
                    " (continuing without user config)");
        m_userConfig = nlohmann::json::object();
        return true;
    } catch (const std::exception& e) {
        LOG_WARNING("Error loading user config file ",
                    filePath,
                    ": ",
                    e.what(),
                    " (continuing without user config)");
        m_userConfig = nlohmann::json::object();
        return true;
    }
}

nlohmann::json ConfigManager::getValueByPath(const std::string& path) const
{
    if (path.empty()) {
        return m_userConfig;
    }

    std::vector<std::string> keys;
    std::stringstream ss(path);
    std::string key;

    while (std::getline(ss, key, '.')) {
        if (!key.empty()) {
            keys.push_back(key);
        }
    }

    nlohmann::json current = m_userConfig;
    for (const auto& k : keys) {
        if (current.is_object() && current.contains(k)) {
            current = current[k];
        } else {
            return nlohmann::json(nullptr);
        }
    }

    return current;
}