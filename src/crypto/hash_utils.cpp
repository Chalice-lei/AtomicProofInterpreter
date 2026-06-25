#include "hash_utils.h"

#include <cstring>

namespace apc_crypto
{
namespace
{

uint32_t rotl32(uint32_t x, uint32_t n)
{
    return (x << n) | (x >> (32U - n));
}

uint32_t rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32U - n));
}

uint32_t readBE32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint32_t readLE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void writeBE32(uint8_t* out, uint32_t v)
{
    out[0] = static_cast<uint8_t>((v >> 24) & 0xff);
    out[1] = static_cast<uint8_t>((v >> 16) & 0xff);
    out[2] = static_cast<uint8_t>((v >> 8) & 0xff);
    out[3] = static_cast<uint8_t>(v & 0xff);
}

void writeLE32(uint8_t* out, uint32_t v)
{
    out[0] = static_cast<uint8_t>(v & 0xff);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    out[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    out[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

} // namespace

std::vector<uint8_t> sha1Digest(const std::vector<uint8_t>& data)
{
    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xefcdab89U;
    uint32_t h2 = 0x98badcfeU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xc3d2e1f0U;

    auto processBlock = [&](const uint8_t block[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = readBE32(block + i * 4);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcU;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6U;
            }

            uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    };

    const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8ULL;
    const size_t fullBlocks = data.size() / 64;
    for (size_t i = 0; i < fullBlocks; ++i) {
        processBlock(data.data() + i * 64);
    }

    uint8_t tail[128];
    const size_t rem = data.size() - fullBlocks * 64;
    std::memset(tail, 0, sizeof(tail));
    if (rem != 0) {
        std::memcpy(tail, data.data() + fullBlocks * 64, rem);
    }
    tail[rem] = 0x80;

    const size_t tailLen = (rem + 1 + 8 <= 64) ? 64 : 128;
    uint8_t* lenPos = tail + (tailLen - 8);
    for (int i = 0; i < 8; ++i) {
        lenPos[i] = static_cast<uint8_t>((bitLen >> (56 - 8 * i)) & 0xff);
    }

    processBlock(tail);
    if (tailLen == 128) {
        processBlock(tail + 64);
    }

    std::vector<uint8_t> out(20);
    writeBE32(out.data(), h0);
    writeBE32(out.data() + 4, h1);
    writeBE32(out.data() + 8, h2);
    writeBE32(out.data() + 12, h3);
    writeBE32(out.data() + 16, h4);
    return out;
}

std::vector<uint8_t> sha256Digest(const std::vector<uint8_t>& data)
{
    static const uint32_t K[64] =
        {0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
         0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
         0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
         0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
         0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
         0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
         0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
         0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
         0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
         0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
         0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
         0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
         0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    uint32_t h[8] =
        {0x6a09e667U,
         0xbb67ae85U,
         0x3c6ef372U,
         0xa54ff53aU,
         0x510e527fU,
         0x9b05688cU,
         0x1f83d9abU,
         0x5be0cd19U};

    auto ch = [](uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ ((~x) & z);
    };
    auto maj = [](uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (x & z) ^ (y & z);
    };
    auto bigSigma0 = [](uint32_t x) {
        return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
    };
    auto bigSigma1 = [](uint32_t x) {
        return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
    };
    auto smallSigma0 = [](uint32_t x) {
        return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
    };
    auto smallSigma1 = [](uint32_t x) {
        return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
    };

    auto processBlock = [&](const uint8_t block[64]) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = readBE32(block + i * 4);
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = smallSigma1(w[i - 2]) + w[i - 7] +
                   smallSigma0(w[i - 15]) + w[i - 16];
        }

        uint32_t a = h[0];
        uint32_t b = h[1];
        uint32_t c = h[2];
        uint32_t d = h[3];
        uint32_t e = h[4];
        uint32_t f = h[5];
        uint32_t g = h[6];
        uint32_t hh = h[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + bigSigma1(e) + ch(e, f, g) + K[i] + w[i];
            uint32_t t2 = bigSigma0(a) + maj(a, b, c);
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    };

    const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8ULL;
    const size_t fullBlocks = data.size() / 64;
    for (size_t i = 0; i < fullBlocks; ++i) {
        processBlock(data.data() + i * 64);
    }

    uint8_t tail[128];
    const size_t rem = data.size() - fullBlocks * 64;
    std::memset(tail, 0, sizeof(tail));
    if (rem != 0) {
        std::memcpy(tail, data.data() + fullBlocks * 64, rem);
    }
    tail[rem] = 0x80;

    const size_t tailLen = (rem + 1 + 8 <= 64) ? 64 : 128;
    uint8_t* lenPos = tail + (tailLen - 8);
    for (int i = 0; i < 8; ++i) {
        lenPos[i] = static_cast<uint8_t>((bitLen >> (56 - 8 * i)) & 0xff);
    }

    processBlock(tail);
    if (tailLen == 128) {
        processBlock(tail + 64);
    }

    std::vector<uint8_t> out(32);
    for (int i = 0; i < 8; ++i) {
        writeBE32(out.data() + i * 4, h[i]);
    }
    return out;
}

std::vector<uint8_t> ripemd160Digest(const std::vector<uint8_t>& data)
{
    static const uint8_t r[80] = {0,  1,  2,  3,  4,  5,  6,  7, 8,  9,  10, 11,
                                  12, 13, 14, 15, 7,  4,  13, 1, 10, 6,  15, 3,
                                  12, 0,  9,  5,  2,  14, 11, 8, 3,  10, 14, 4,
                                  9,  15, 8,  1,  2,  7,  0,  6, 13, 11, 5,  12,
                                  1,  9,  11, 10, 0,  8,  12, 4, 13, 3,  7,  15,
                                  14, 5,  6,  2,  4,  0,  5,  9, 7,  12, 2,  10,
                                  14, 1,  3,  8,  11, 6,  15, 13};
    static const uint8_t rp[80] = {5,  14, 7,  0,  9,  2,  11, 4,  13, 6,
                                   15, 8,  1,  10, 3,  12, 6,  11, 3,  7,
                                   0,  13, 5,  10, 14, 15, 8,  12, 4,  9,
                                   1,  2,  15, 5,  1,  3,  7,  14, 6,  9,
                                   11, 8,  12, 2,  10, 0,  4,  13, 8,  6,
                                   4,  1,  3,  11, 15, 0,  5,  12, 2,  13,
                                   9,  7,  10, 14, 12, 15, 10, 4,  1,  5,
                                   8,  7,  6,  2,  13, 14, 0,  3,  9,  11};
    static const uint8_t s[80] = {11, 14, 15, 12, 5,  8,  7,  9,  11, 13,
                                  14, 15, 6,  7,  9,  8,  7,  6,  8,  13,
                                  11, 9,  7,  15, 7,  12, 15, 9,  11, 7,
                                  13, 12, 11, 13, 6,  7,  14, 9,  13, 15,
                                  14, 8,  13, 6,  5,  12, 7,  5,  11, 12,
                                  14, 15, 14, 15, 9,  8,  9,  14, 5,  6,
                                  8,  6,  5,  12, 9,  15, 5,  11, 6,  8,
                                  13, 12, 5,  12, 13, 14, 11, 8,  5,  6};
    static const uint8_t sp[80] = {8,  9,  9,  11, 13, 15, 15, 5,  7,  7,
                                   8,  11, 14, 14, 12, 6,  9,  13, 15, 7,
                                   12, 8,  9,  11, 7,  7,  12, 7,  6,  15,
                                   13, 11, 9,  7,  15, 11, 8,  6,  6,  14,
                                   12, 13, 5,  14, 13, 13, 7,  5,  15, 5,
                                   8,  11, 14, 14, 6,  14, 6,  9,  12, 9,
                                   12, 5,  15, 8,  8,  5,  12, 9,  12, 5,
                                   14, 6,  8,  13, 6,  5,  15, 13, 11, 11};

    auto f = [](int j, uint32_t x, uint32_t y, uint32_t z) -> uint32_t {
        if (j <= 15) {
            return x ^ y ^ z;
        }
        if (j <= 31) {
            return (x & y) | (~x & z);
        }
        if (j <= 47) {
            return (x | ~y) ^ z;
        }
        if (j <= 63) {
            return (x & z) | (y & ~z);
        }
        return x ^ (y | ~z);
    };
    auto k = [](int j) -> uint32_t {
        if (j <= 15) {
            return 0x00000000U;
        }
        if (j <= 31) {
            return 0x5a827999U;
        }
        if (j <= 47) {
            return 0x6ed9eba1U;
        }
        if (j <= 63) {
            return 0x8f1bbcdcU;
        }
        return 0xa953fd4eU;
    };
    auto kp = [](int j) -> uint32_t {
        if (j <= 15) {
            return 0x50a28be6U;
        }
        if (j <= 31) {
            return 0x5c4dd124U;
        }
        if (j <= 47) {
            return 0x6d703ef3U;
        }
        if (j <= 63) {
            return 0x7a6d76e9U;
        }
        return 0x00000000U;
    };

    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xefcdab89U;
    uint32_t h2 = 0x98badcfeU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xc3d2e1f0U;

    auto processBlock = [&](const uint8_t block[64]) {
        uint32_t x[16];
        for (int i = 0; i < 16; ++i) {
            x[i] = readLE32(block + i * 4);
        }

        uint32_t al = h0;
        uint32_t bl = h1;
        uint32_t cl = h2;
        uint32_t dl = h3;
        uint32_t el = h4;
        uint32_t ar = h0;
        uint32_t br = h1;
        uint32_t cr = h2;
        uint32_t dr = h3;
        uint32_t er = h4;

        for (int j = 0; j < 80; ++j) {
            uint32_t tl =
                rotl32(al + f(j, bl, cl, dl) + x[r[j]] + k(j), s[j]) + el;
            al = el;
            el = dl;
            dl = rotl32(cl, 10);
            cl = bl;
            bl = tl;

            uint32_t tr =
                rotl32(ar + f(79 - j, br, cr, dr) + x[rp[j]] + kp(j), sp[j]) +
                er;
            ar = er;
            er = dr;
            dr = rotl32(cr, 10);
            cr = br;
            br = tr;
        }

        uint32_t t = h1 + cl + dr;
        h1 = h2 + dl + er;
        h2 = h3 + el + ar;
        h3 = h4 + al + br;
        h4 = h0 + bl + cr;
        h0 = t;
    };

    const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8ULL;
    const size_t fullBlocks = data.size() / 64;
    for (size_t i = 0; i < fullBlocks; ++i) {
        processBlock(data.data() + i * 64);
    }

    uint8_t tail[128];
    const size_t rem = data.size() - fullBlocks * 64;
    std::memset(tail, 0, sizeof(tail));
    if (rem != 0) {
        std::memcpy(tail, data.data() + fullBlocks * 64, rem);
    }
    tail[rem] = 0x80;

    const size_t tailLen = (rem + 1 + 8 <= 64) ? 64 : 128;
    uint8_t* lenPos = tail + (tailLen - 8);
    uint64_t bitLenCopy = bitLen;
    for (int i = 0; i < 8; ++i) {
        lenPos[i] = static_cast<uint8_t>(bitLenCopy & 0xff);
        bitLenCopy >>= 8;
    }

    processBlock(tail);
    if (tailLen == 128) {
        processBlock(tail + 64);
    }

    std::vector<uint8_t> out(20);
    writeLE32(out.data(), h0);
    writeLE32(out.data() + 4, h1);
    writeLE32(out.data() + 8, h2);
    writeLE32(out.data() + 12, h3);
    writeLE32(out.data() + 16, h4);
    return out;
}

std::vector<uint8_t> hash160Digest(const std::vector<uint8_t>& data)
{
    return ripemd160Digest(sha256Digest(data));
}

std::vector<uint8_t> hash256Digest(const std::vector<uint8_t>& data)
{
    return sha256Digest(sha256Digest(data));
}

} // namespace apc_crypto
