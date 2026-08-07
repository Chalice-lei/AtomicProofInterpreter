#ifndef BYT_LIB_H
#define BYT_LIB_H
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

#include "../bytecode/script_codec.h"
#include "util_defs.h"

SPACE_TBC_START

inline std::string byteToHex(uint8_t byte)
{
    static const char hexDigits[] = "0123456789abcdef";
    return std::string{hexDigits[byte >> 4], hexDigits[byte & 0x0f]};
}

inline std::string encodeLittleEndianUnsigned(size_t value, size_t byteCount)
{
    std::string result;
    result.reserve(byteCount * 2);
    for (size_t i = 0; i < byteCount; ++i) {
        result += byteToHex(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
    }
    return result;
}

// 小端序长度编码
inline std::string encodeLE16(int length)
{
    return encodeLittleEndianUnsigned(
        static_cast<uint16_t>(length),
        2
    );
}

inline std::string encodeLE32(int length)
{
    return encodeLittleEndianUnsigned(
        static_cast<uint32_t>(length),
        4
    );
}

inline std::string encodePushDataPrefix(size_t length)
{
    return ScriptCodec::legacyPushPrefixHex(length).value_or(std::string{});
}

// 选择 push 操作码并编码长度
inline std::string bytEncodeLengthOpcode(int length)
{
    if (length < 0) {
        return "";
    }
    return encodePushDataPrefix(static_cast<size_t>(length));
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
