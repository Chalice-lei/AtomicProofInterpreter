#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <thread>
#include <vector>

static const char* LogLevelToString(LogLevel level)
{
    switch (level) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARNING:
            return "WARNING";
        case LogLevel::ERROR:
            return "ERROR";
        case LogLevel::CRITICAL:
            return "CRITICAL";
        default:
            return "UNKNOWN";
    }
}

static std::string GetThreadIdStr()
{
    std::stringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}

Logger& Logger::GetInstance()
{
    static Logger instance;
    return instance;
}

void Logger::Initialize(
    LogLevel minLevel,
    const std::string& logFile,
    bool consoleOutput,
    size_t maxFileSize,
    size_t maxBackupFiles,
    size_t maxTotalSize,
    size_t cleanupInterval,
    bool forceCleanupOnInit
)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_minLevel.store(minLevel, std::memory_order_relaxed);
    m_consoleOutput.store(consoleOutput, std::memory_order_relaxed);
    m_maxFileSize = maxFileSize;
    m_maxBackupFiles = maxBackupFiles;
    m_maxTotalSize = maxTotalSize;
    m_cleanupInterval = cleanupInterval;
    m_writeCounter.store(0, std::memory_order_relaxed);

    if (!logFile.empty()) {
        // 自动放入 log/ 目录
        std::filesystem::path logPath(logFile);
        if (logPath.is_relative()) {
            m_logFilePath = "log/" + logFile;
        } else {
            m_logFilePath = logFile;
        }

        if (!CreateLogDirectory(m_logFilePath)) {
            std::cerr << "Failed to create log directory for: " << m_logFilePath
                      << std::endl;
        }

        OpenLogFile();

        // 启动时仅在必要时做完整检查
        if (forceCleanupOnInit && m_maxTotalSize > 0) {
            if (FastSizeCheck()) {
                CleanupOldLogs();
            }
        }
    }
}

void Logger::Log_base(
    LogLevel level,
    const std::string& message,
    const std::string& file,
    int line,
    bool pureText /* = false */,
    bool lineBreak /* = true*/
)
{
    if (level < m_minLevel.load(std::memory_order_relaxed)) {
        return;
    }

    std::ostringstream logStream;
    if (!pureText) {
        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()
                      ) %
                      1000;

        // 线程安全格式化
        std::tm tm;
#ifdef _WIN32
        localtime_s(&tm, &now_time);
#else
        localtime_r(&now_time, &tm);
#endif
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tm);

        logStream << "[" << timeStr << "." << std::setfill('0') << std::setw(3)
                  << now_ms.count() << "] " << "[" << GetThreadIdStr() << "] "
                  << "[" << LogLevelToString(level) << "] ";

        if (!file.empty()) {
            size_t lastSlash = file.find_last_of("/\\");
            logStream << "["
                      << (lastSlash == std::string::npos
                              ? file
                              : file.substr(lastSlash + 1))
                      << ":" << line << "] ";
        }
    }

    if (lineBreak) {
        logStream << message << "\n";
    } else {
        logStream << message;
    }

    std::string logMessage = logStream.str();

    // 输出: 加锁部分
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_consoleOutput.load(std::memory_order_relaxed)) {
        std::cout << logMessage;
        std::cout.flush();
    }

    if (m_fileStream.is_open()) {
        m_fileStream << logMessage;
        m_fileStream.flush();

        size_t currentCount =
            m_writeCounter.fetch_add(1, std::memory_order_relaxed);

        // 文件超限则轮转
        if (m_maxFileSize > 0 &&
            m_fileStream.tellp() > static_cast<std::streampos>(m_maxFileSize)) {
            RotateLogFiles();
        }

        // 防日志无限增长的多重保护
        bool shouldCheckCleanup = false;

        // 1. 延迟清理: 按间隔检查总大小
        if (m_maxTotalSize > 0 && m_cleanupInterval > 0 &&
            (currentCount % m_cleanupInterval) == 0) {
            shouldCheckCleanup = true;
        }

        // 2. 轮转后必查 (关键保护)
        static bool justRotated = false;
        if (m_maxTotalSize > 0 && justRotated) {
            shouldCheckCleanup = true;
            justRotated = false;
        }

        // 3. 启发式紧急保护: 文件突然很小可能刚轮转
        if (m_maxTotalSize > 0 && currentCount % 10 == 0) {
            std::streampos currentPos = m_fileStream.tellp();
            if (currentPos < 1024) {
                justRotated = true;
            }
        }

        if (shouldCheckCleanup && ShouldCleanup()) {
            CleanupOldLogs();
        }
    }
}

void Logger::SetLogLevel(LogLevel level)
{
    m_minLevel.store(level, std::memory_order_relaxed);
}

LogLevel Logger::GetCurrentLevel() const
{
    return m_minLevel.load(std::memory_order_relaxed);
}

void Logger::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }
}

Logger::~Logger()
{
    Shutdown();
}

void Logger::OpenLogFile()
{
    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }
    m_fileStream.open(m_logFilePath, std::ios::out | std::ios::app);
    if (!m_fileStream.is_open()) {
        std::cerr << "Failed to open log file: " << m_logFilePath << std::endl;
    }
}

void Logger::RotateLogFiles()
{
    if (m_maxBackupFiles == 0) {
        m_fileStream.close();
        m_fileStream.open(m_logFilePath, std::ios::out | std::ios::trunc);
        return;
    }

    m_fileStream.close();

    std::filesystem::path logPath(m_logFilePath);
    std::string baseName = logPath.stem().string();
    std::string extension = logPath.extension().string();
    if (extension.empty()) {
        extension = ".log";
    }

    // 生成带时间戳的备份名
    std::string timestampedBackup = GenerateTimestampedBackupName(
        (logPath.parent_path() / baseName).string(),
        extension.substr(1) // 移除点号
    );

    // 重命名当前日志为时间戳备份
    std::rename(m_logFilePath.c_str(), timestampedBackup.c_str());

    OpenLogFile();

    // 轮转后立即检查总大小, 不等计数器到点
    if (m_maxTotalSize > 0 && ShouldCleanup()) {
        CleanupOldLogs();
    }
}

bool Logger::ShouldCleanup()
{
    return GetTotalLogSize() > m_maxTotalSize;
}

size_t Logger::GetTotalLogSize()
{
    size_t totalSize = 0;

    // 主日志文件
    std::ifstream file(m_logFilePath, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        totalSize += file.tellg();
        file.close();
    }

    // 所有时间戳备份文件
    std::vector<std::string> backupFiles = GetBackupFilesList();
    for (const std::string& backupFile : backupFiles) {
        std::ifstream backup(backupFile, std::ios::binary | std::ios::ate);
        if (backup.is_open()) {
            totalSize += backup.tellg();
            backup.close();
        }
    }

    return totalSize;
}

void Logger::CleanupOldLogs()
{
    // 已按时间排序 (最旧在前)
    std::vector<std::string> backupFiles = GetBackupFilesList();
    size_t totalSize = GetTotalLogSize();

    // 从最旧开始删除, 直到总大小和数量都在限制内
    auto it = backupFiles.begin();
    while (it != backupFiles.end()) {
        size_t currentBackupCount = backupFiles.end() - it;

        if (totalSize <= m_maxTotalSize &&
            currentBackupCount <= m_maxBackupFiles) {
            break;
        }

        bool needCleanupBySize = (totalSize > m_maxTotalSize);
        bool needCleanupByCount = (currentBackupCount > m_maxBackupFiles);

        if (needCleanupBySize || needCleanupByCount) {
            std::ifstream file(*it, std::ios::binary | std::ios::ate);
            size_t fileSize = 0;
            if (file.is_open()) {
                fileSize = file.tellg();
                file.close();
            }

            if (std::remove(it->c_str()) == 0) {
                totalSize -= fileSize;
                it = backupFiles.erase(it);
            } else {
                ++it; // 删除失败, 跳过
            }
        } else {
            break;
        }
    }
}
bool Logger::FastSizeCheck()
{
    // 启发式: 只看主日志和最新几个备份, 避免完整统计
    size_t quickSize = 0;
    size_t filesChecked = 0;
    const size_t maxFilesToCheck = 3;

    std::ifstream file(m_logFilePath, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        quickSize += file.tellg();
        file.close();
        filesChecked++;
    }

    // 反向遍历, 从最新备份开始
    std::vector<std::string> backupFiles = GetBackupFilesList();
    for (auto it = backupFiles.rbegin();
         it != backupFiles.rend() && filesChecked < maxFilesToCheck;
         ++it) {
        std::ifstream backup(*it, std::ios::binary | std::ios::ate);
        if (backup.is_open()) {
            quickSize += backup.tellg();
            backup.close();
            filesChecked++;
        }
    }

    // 接近 70% 限制即触发完整检查
    return quickSize > (m_maxTotalSize * 7 / 10);
}

bool Logger::CreateLogDirectory(const std::string& filePath)
{
    try {
        std::filesystem::path logPath(filePath);
        std::filesystem::path logDir = logPath.parent_path();

        if (!logDir.empty() && !std::filesystem::exists(logDir)) {
            return std::filesystem::create_directories(logDir);
        }
        return true;
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Failed to create log directory: " << e.what()
                  << std::endl;
        return false;
    }
}

std::string Logger::GenerateTimestampedBackupName(
    const std::string& baseName,
    const std::string& extension
)
{
    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);

    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now_time);
#else
    localtime_r(&now_time, &tm);
#endif

    char timeStr[32];
    // yyyyMMddHHmm, 精确到分钟
    snprintf(
        timeStr,
        sizeof(timeStr),
        "%04d%02d%02d%02d%02d",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min
    );

    return baseName + "_" + timeStr + "." + extension;
}

std::vector<std::string> Logger::GetBackupFilesList()
{
    std::vector<std::string> backupFiles;

    try {
        std::filesystem::path logPath(m_logFilePath);
        std::filesystem::path logDir = logPath.parent_path();
        std::string baseName = logPath.stem().string();
        std::string extension = logPath.extension().string();
        if (extension.empty()) {
            extension = "log";
        } else {
            extension = extension.substr(1); // 移除点号
        }

        // baseName_yyyyMMddHHmm.extension (12 位时间戳)
        std::regex backupPattern(baseName + R"(_\d{12}\.)" + extension);

        if (std::filesystem::exists(logDir)) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(logDir)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    if (std::regex_match(filename, backupPattern)) {
                        backupFiles.push_back(entry.path().string());
                    }
                }
            }
        }

        // 按文件名排序 (时间戳天然有序)
        std::sort(backupFiles.begin(), backupFiles.end());

    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Error scanning backup files: " << e.what() << std::endl;
    }

    return backupFiles;
}