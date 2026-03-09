#pragma once

#include <string>
#include <cstdio>
#include <ctime>
#include <mutex>

// 终端颜色（ANSI，Windows 10+ 支持）
#ifdef _WIN32
#  define LOG_RESET  ""
#  define LOG_RED    ""
#  define LOG_YELLOW ""
#  define LOG_GREEN  ""
#  define LOG_CYAN   ""
#else
#  define LOG_RESET  "\033[0m"
#  define LOG_RED    "\033[31m"
#  define LOG_YELLOW "\033[33m"
#  define LOG_GREEN  "\033[32m"
#  define LOG_CYAN   "\033[36m"
#endif

enum class LogLevel { DEBUG = 0, INFO, WARN, ERROR };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void setLevel(LogLevel lvl) { level_ = lvl; }
    void setVerbose(bool v)     { level_ = v ? LogLevel::DEBUG : LogLevel::INFO; }

    void log(LogLevel lvl, const std::string& msg) {
        if (lvl < level_) return;
        std::lock_guard<std::mutex> lock(mtx_);
        const char* tag = lvlTag(lvl);
        std::fprintf(stdout, "[%s] %s\n", tag, msg.c_str());
        std::fflush(stdout);
    }

    void info (const std::string& m) { log(LogLevel::INFO,  m); }
    void warn (const std::string& m) { log(LogLevel::WARN,  m); }
    void error(const std::string& m) { log(LogLevel::ERROR, m); }
    void debug(const std::string& m) { log(LogLevel::DEBUG, m); }

private:
    Logger() = default;
    LogLevel level_ = LogLevel::INFO;
    std::mutex mtx_;

    const char* lvlTag(LogLevel l) {
        switch(l) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
        }
        return "?    ";
    }
};

// 全局便捷宏
#define LOG_INFO(msg)  Logger::instance().info(msg)
#define LOG_WARN(msg)  Logger::instance().warn(msg)
#define LOG_ERROR(msg) Logger::instance().error(msg)
#define LOG_DEBUG(msg) Logger::instance().debug(msg)
