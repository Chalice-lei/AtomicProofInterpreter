#ifndef TYPE_VALIDATOR_H
#define TYPE_VALIDATOR_H

#include <stdint.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "byt_data_types.h"
#include "byt_defs.h"

SPACE_TBC_START

// 类型验证器: 基于 BSV Genesis 之后的策略 / 共识限制
class TypeValidator
{
public:
    // 策略层面 (节点可放宽)
    static constexpr size_t GENESIS_STACK_MEMORY_POLICY = 100 * 1024 * 1024; // 100MB
    static constexpr size_t GENESIS_SCRIPT_SIZE_POLICY = 500 * 1024;          // 500KB
    static constexpr size_t GENESIS_SCRIPT_NUM_POLICY = 10 * 1024;            // 10KB

    // 共识层面 (硬上限)
    static constexpr size_t GENESIS_SCRIPT_SIZE_CONSENSUS = UINT32_MAX;       // 4GB
    static constexpr size_t GENESIS_SCRIPT_NUM_CONSENSUS = 750 * 1024;        // 750KB

    struct TypeConstraints
    {
        size_t minSize = 0;
        size_t maxSize = SIZE_MAX;       // 策略上限
        size_t fixedSize = 0;            // 0 表示可变长度
        bool requiresValidation = false;
        std::function<bool(const std::string&)> validator = nullptr;
        std::string description;
    };

public:
    // data 为 hex 字符串
    static bool validateType(BytecodeType type, const std::string& data);

    static bool validateSize(BytecodeType type, size_t dataSize);

    static const TypeConstraints& getConstraints(BytecodeType type);

    static bool isRestrictedType(BytecodeType type);

    static size_t getMaxSize(BytecodeType type);
    static size_t getMinSize(BytecodeType type);

    static bool isValidHex(const std::string& hex);

    // 公钥: 33 或 65 字节
    static bool isValidPubKey(const std::string& pubkey);

    static bool isValidDERSignature(const std::string& signature);

    static bool isValidHash(const std::string& hash, size_t expectedSize);

    // 私钥: 32 字节
    static bool isValidPrivKey(const std::string& privkey);

    static bool isValidInt32(int64_t value);

    // 去掉 0x 前缀后实际数据字节数
    static size_t getHexDataSize(const std::string& hex);

    static void initializeConstraints();

    static std::string getValidationError(BytecodeType type,
                                          const std::string& data);

    static bool isValidRabinSignature(const std::string& signature);
    static bool isValidRabinPubKey(const std::string& pubkey);

private:
    static std::unordered_map<BytecodeType, TypeConstraints> m_typeConstraints;
};

SPACE_TBC_END

#endif // TYPE_VALIDATOR_H