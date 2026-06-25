#ifndef LOGGER_H
#define LOGGER_H

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// 日志级别：DEBUG/INFO/WARNING/ERROR/CRITICAL，NONE 表示禁用输出
enum class LogLevel { DEBUG, INFO, WARNING, ERROR, CRITICAL, NONE };

class Logger
{
public:
    // 获取单例实例
    static Logger& GetInstance();

    /**
     * 初始化日志系统
     * @param logFile 日志文件路径，为空则不写入文件
     * @param maxFileSize 单文件上限（字节），超过即轮转
     * @param maxBackupFiles 备份文件数，0 表示不保留
     * @param maxTotalSize 所有日志总大小限制，超过后删除最旧备份
     * @param cleanupInterval 每写入 N 次检查一次总大小
     * @param forceCleanupOnInit 初始化时是否清理（编译器建议 false）
     */
    void Initialize(
        LogLevel minLevel = LogLevel::INFO,
        const std::string& logFile = "",
        bool consoleOutput = true,
        size_t maxFileSize = 500 * 1024 * 1024, // 单个默认500MB
        size_t maxBackupFiles = 20,
        size_t maxTotalSize = 2ULL * 1024 * 1024 * 1024, // 默认2GB总大小限制
        size_t cleanupInterval = 1000,           // 每1000次写入检查一次
        bool forceCleanupOnInit = false
    ); // 编译器建议设为false以提升启动速度

    // 设置最低日志级别
    void SetLogLevel(LogLevel level);

    // 获取当前最低日志级别
    LogLevel GetCurrentLevel() const;

    // 启用/禁用控制台输出
    void EnableConsoleOutput(bool enable);

    /**
     * 基础日志方法
     * @param pureText 纯文本模式（不附加时间戳等格式化信息）
     * @param lineBreak 是否在末尾追加换行
     */
    void Log_base(
        LogLevel level,
        const std::string& message,
        const std::string& file = "",
        int line = 0,
        bool pureText = false,
        bool lineBreak = true
    );

    // 变参模板日志方法（C++17 折叠表达式），args 支持任意数量与类型
    template <typename... Args>
    void Log(
        LogLevel level,
        const std::string& file,
        int line,
        bool pureText,
        bool lineBreak,
        Args&&... args
    )
    {
        if (level < m_minLevel.load(std::memory_order_relaxed))
            return;

        std::ostringstream oss;
        (oss << ... << std::forward<Args>(args));
        Log_base(level, oss.str(), file, line, pureText, lineBreak);
    }

    // 关闭日志系统并释放资源
    void Shutdown();

    // 禁止复制和移动
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger() = default;
    ~Logger();

    // 打开日志文件
    void OpenLogFile();

    // 轮转：当前文件重命名为备份，重新创建主日志文件
    void RotateLogFiles();

    // 删除最旧备份直到总大小回到限制内
    void CleanupOldLogs();

    // 主日志加所有备份的总字节数
    size_t GetTotalLogSize();

    // 判断是否需要清理旧日志
    bool ShouldCleanup();

    // 启动时的轻量级大小检查；false 表示肯定无需清理
    bool FastSizeCheck();

    // 按需创建日志目录；返回是否成功或已存在
    bool CreateLogDirectory(const std::string& filePath);

    // 生成形如 baseName_yyyyMMddHHmm.extension 的备份文件名
    std::string GenerateTimestampedBackupName(
        const std::string& baseName,
        const std::string& extension
    );

    // 按时间排序的备份文件列表（最旧在前）
    std::vector<std::string> GetBackupFilesList();

private:
    std::atomic<LogLevel> m_minLevel{LogLevel::INFO};
    std::atomic<bool> m_consoleOutput{true};
    std::ofstream m_fileStream;
    std::mutex m_mutex;
    std::string m_logFilePath;
    size_t m_maxFileSize = 0;
    size_t m_maxBackupFiles = 0;
    size_t m_maxTotalSize = 0;             // 总大小限制
    size_t m_cleanupInterval = 100;        // 清理检查间隔
    std::atomic<size_t> m_writeCounter{0}; // 写入计数器
};

/**
 * 通用日志宏：自动捕获 __FILE__/__LINE__，变参支持任意类型
 * 示例：LOG(LogLevel::INFO, "用户登录：", username, " ID:", userId);
 */
#define LOG(level, ...)                                                        \
    Logger::GetInstance()                                                      \
        .Log(level, __FILE__, __LINE__, false, true, __VA_ARGS__)

// 各级别日志宏，免去显式传入 LogLevel
// 示例：LOG_INFO("程序启动完成"); LOG_ERROR("文件打开失败：", filename);
#define LOG_DEBUG(...) LOG(LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...) LOG(LogLevel::INFO, __VA_ARGS__)
#define LOG_WARNING(...) LOG(LogLevel::WARNING, __VA_ARGS__)
#define LOG_ERROR(...) LOG(LogLevel::ERROR, __VA_ARGS__)
#define LOG_CRITICAL(...) LOG(LogLevel::CRITICAL, __VA_ARGS__)

#endif // LOGGER_H