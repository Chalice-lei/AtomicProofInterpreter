#ifndef RUNTIME_CODEC_H
#define RUNTIME_CODEC_H

#include <cstdint>
#include <string>
#include <vector>

namespace apc_interpreter::runtime_codec
{

std::string trim(std::string value);
std::string toLower(std::string value);

std::vector<uint8_t> serializeScriptNum(int64_t value);
int64_t deserializeScriptNum(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> parseHex(
    std::string hex,
    const std::string& errorMessage = "invalid hex literal"
);
std::string bytesToHex(const std::vector<uint8_t>& bytes, bool withPrefix = true);

} // namespace apc_interpreter::runtime_codec

#endif // RUNTIME_CODEC_H
