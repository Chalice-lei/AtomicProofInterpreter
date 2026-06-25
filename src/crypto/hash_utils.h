#ifndef APC_CRYPTO_HASH_UTILS_H
#define APC_CRYPTO_HASH_UTILS_H

#include <cstdint>
#include <vector>

namespace apc_crypto
{

std::vector<uint8_t> sha1Digest(const std::vector<uint8_t>& data);
std::vector<uint8_t> sha256Digest(const std::vector<uint8_t>& data);
std::vector<uint8_t> ripemd160Digest(const std::vector<uint8_t>& data);
std::vector<uint8_t> hash160Digest(const std::vector<uint8_t>& data);
std::vector<uint8_t> hash256Digest(const std::vector<uint8_t>& data);

} // namespace apc_crypto

#endif // APC_CRYPTO_HASH_UTILS_H
