#ifndef BYT_LIB_H
#define BYT_LIB_H
#include <iomanip>
#include <sstream>
#include <string>

#include "util_defs.h"

SPACE_TBC_START

// 小端序长度编码
inline std::string encodeLE16(int length)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(2) << (length & 0xff);
    oss << std::hex << std::setfill('0') << std::setw(2)
        << ((length >> 8) & 0xff);
    return oss.str();
}

inline std::string encodeLE32(int length)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(2) << (length & 0xff);
    oss << std::hex << std::setfill('0') << std::setw(2)
        << ((length >> 8) & 0xff);
    oss << std::hex << std::setfill('0') << std::setw(2)
        << ((length >> 16) & 0xff);
    oss << std::hex << std::setfill('0') << std::setw(2)
        << ((length >> 24) & 0xff);
    return oss.str();
}

// 选择 push 操作码并编码长度
inline std::string bytEncodeLengthOpcode(int length)
{
    std::ostringstream oss;

    if (length < 76) {
        // 0-75 字节：长度值即操作码
        oss << std::hex << std::setfill('0') << std::setw(2) << length;
        return oss.str();
    } else if (length <= 255) {
        oss << "4c" << std::hex << std::setfill('0') << std::setw(2) << length;
        return oss.str();
    } else if (length <= 65535) {
        oss << "4d" << encodeLE16(length);
        return oss.str();
    } else {
        oss << "4e" << encodeLE32(length);
        return oss.str();
    }
}

inline void escapeString(std::string& str)
{
    std::string result;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '\\' && i + 1 < str.length()) {
            if (str[i + 1] == 'n') {
                result += "\\n";
                ++i;
            } else if (str[i + 1] == 't') {
                result += "\\t";
                ++i;
            } else if (str[i + 1] == '"') {
                result += "\\\"";
                ++i;
            } else if (str[i + 1] == '\\') {
                result += "\\\\";
                ++i;
            } else {
                result += str[i];
            }
        } else if ('\n' == str[i]) {
            result += "\\n";
        } else if ('\t' == str[i]) {
            result += "\\t";
        } else if ('\0' == str[i]) {
            result += "\\0";
        } else if ('\r' == str[i]) {
            result += "\\r";
        } else {
            result += str[i];
        }
    }
    str = result;
}

SPACE_TBC_END
#endif