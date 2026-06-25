#include "compiler_repl/repl_frontend.h"

#include <string>

#include "config/config_manager.h"
#include "log/logger.h"

int main()
{
    std::string logFileName = "apc-repl.log";

    Logger::GetInstance().Initialize(LogLevel::WARNING, logFileName, false);
    ConfigManager::getInstance().initialize("user_preferences.json");

    apc::repl::ReplFrontend frontend;
    int rc = frontend.run();

    Logger::GetInstance().Shutdown();
    return rc;
}
