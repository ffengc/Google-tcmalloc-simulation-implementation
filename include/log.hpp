

#ifndef __YUFC_LOG__
#define __YUFC_LOG__

#include <iostream>
#include <string>

enum STATUES // 日志等级
{
    INFO,
    DEBUG,
    WARNING,
    ERROR,
    FATAL
};
// LOG() << "message"
inline std::ostream& Log(const std::string& level, const std::string& file_name, int line) {
#ifdef PROJECT_DEBUG
    // 添加日志等级
    std::string message = "[";
    message += level;
    message += "]";
    // 添加报错文件名称
    message += "[";
    message += file_name;
    message += "]";
    // 添加当前文件的行数
    message += "[";
    message += std::to_string(line);
    message += "]";
    // cout 本质内部是包含缓冲区的
    std::cout << message << " "; // 不要endl进行刷新
#else
#endif
    return std::cout;
}

// 这种就是开放式的日志
#define LOG(level) Log(#level, __FILE__, __LINE__)

#endif