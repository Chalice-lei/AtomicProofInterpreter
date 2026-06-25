#include "type_validator.h"

#include <algorithm>
#include <cctype>
#include <climits>

#include "bytecode_helper_fun.h"

using namespace tbc;

std::unordered_map<BytecodeType, TypeValidator::TypeConstraints>
    TypeValidator::m_typeConstraints;

void TypeValidator::initializeConstraints()
{
    m_typeConstraints[BytecodeType::Boolean] =
        {.minSize = 1,
         .maxSize = 1,
         .fixedSize = 1,
         .requiresValidation = false,
         .description = "Boolean - Fixed 1 byte"};

    m_typeConstraints[BytecodeType::Number] =
        {.minSize = 0,
         .maxSize = GENESIS_SCRIPT_NUM_POLICY, // 策略 10KB; 共识 750KB
         .fixedSize = 0,
         .requiresValidation = false,
         .description = "Number - Variable length, 10KB policy limit"};

    m_typeConstraints[BytecodeType::String] =
        {.minSize = 0,
         .maxSize = GENESIS_STACK_MEMORY_POLICY,
         .fixedSize = 0,
         .requiresValidation = false,
         .description = "String - Variable length, 100MB policy limit"};

    m_typeConstraints[BytecodeType::Hex] = {
        .minSize = 0,
        .maxSize = GENESIS_STACK_MEMORY_POLICY,
        .fixedSize = 0,
        .requiresValidation = true,
        .validator = [](const std::string& data) { return isValidHex(data); },
        .description = "Hexadecimal data - Variable length, 100MB policy limit"
    };

    m_typeConstraints[BytecodeType::FixedArray] =
        {.minSize = 0,
         .maxSize = GENESIS_STACK_MEMORY_POLICY,
         .fixedSize = 0, // 大小由模板参数决定
         .requiresValidation = false,
         .description = "Fixed-size array - 100MB policy limit"};

    // 密码学相关类型: 保持原本的固定大小
    m_typeConstraints[BytecodeType::PubKey] =
        {.minSize = 33,
         .maxSize = 65,
         .fixedSize = 0, // 33B 压缩 或 65B 未压缩
         .requiresValidation = true,
         .validator = [](const std::string& data
                      ) { return isValidPubKey(data); },
         .description =
             "Public key - 33 bytes (compressed) or 65 bytes (uncompressed)"};

    m_typeConstraints[BytecodeType::Sig] =
        {.minSize = 6,
         .maxSize = 73, // DER + sighash flag
         .fixedSize = 0,
         .requiresValidation = true,
         .validator = [](const std::string& data
                      ) { return isValidDERSignature(data); },
         .description = "DER format signature - 6-73 bytes"};

    m_typeConstraints[BytecodeType::Ripemd160] =
        {.minSize = 20,
         .maxSize = 20,
         .fixedSize = 20,
         .requiresValidation = true,
         .validator = [](const std::string& data
                      ) { return isValidHash(data, 20); },
         .description = "RIPEMD-160 hash - Fixed 20 bytes"};

    m_typeConstraints[BytecodeType::PubKeyHash] =
        {.minSize = 20,
         .maxSize = 20,
         .fixedSize = 20,
         .requiresValidation = true,
         .validator = [](const std::string& data
                      ) { return isValidHash(data, 20); },
         .description = "Public key hash - Fixed 20 bytes"};

    m_typeConstraints[BytecodeType::Sha1] =
        {.minSize = 20,
         .maxSize = 20,
         .fixedSize = 20,
         .requiresValidation = true,
         .validator = [](const std::string& data
                      ) { return isValidHash(data, 20); },
         .description = "SHA-1 hash - Fixed 20 bytes"};

    m_typeConstraints[BytecodeType::Sha256] =
        {.minSize = 32,
         .maxSize = 32,
         .fixedSize = 32,
         .requiresValidation = true,
         .validator = [](const std::string& data
                      ) { return isValidHash(data, 32); },
         .description = "SHA-256 hash - Fixed 32 bytes"};

    // BSV 脚本特有类型 (Genesis 后)
    m_typeConstraints[BytecodeType::SigHashType] =
        {.minSize = 1,
         .maxSize = 4,
         .fixedSize = 0,
         .requiresValidation = false,
         .description = "Signature hash type - 1-4 bytes"};

    m_typeConstraints[BytecodeType::SigHashPreimage] =
        {.minSize = 32,
         .maxSize = GENESIS_STACK_MEMORY_POLICY, // Genesis 后允许大型 preimage
         .fixedSize = 0,
         .requiresValidation = false,
         .description =
             "Signature hash preimage - Variable length, 100MB policy limit"};

    m_typeConstraints[BytecodeType::OpCodeType] =
        {.minSize = 1,
         .maxSize = 1,
         .fixedSize = 1,
         .requiresValidation = false,
         .description = "Opcode type - Fixed 1 byte"};

    m_typeConstraints[BytecodeType::Addr] =
        {.minSize = 0,
         .maxSize = GENESIS_STACK_MEMORY_POLICY,
         .fixedSize = 0,
         .requiresValidation = false, // 地址格式已在词法阶段校验
         .description = "Bitcoin address - Variable length string format"};

    m_typeConstraints[BytecodeType::PrivKey] =
        {.minSize = 32,
         .maxSize = 32,
         .fixedSize = 32,
         .requiresValidation = true,
         .validator = [](const std::string& data
                      ) { return isValidPrivKey(data); },
         .description = "Private key - Fixed 32 bytes"};
}

bool TypeValidator::validateType(BytecodeType type, const std::string& data)
{
    if (m_typeConstraints.empty()) {
        initializeConstraints();
    }

    auto it = m_typeConstraints.find(type);
    if (it == m_typeConstraints.end()) {
        return false;
    }

    size_t dataSize = getHexDataSize(data);

    if (!validateSize(type, dataSize)) {
        return false;
    }

    const auto& constraints = it->second;
    if (constraints.requiresValidation && constraints.validator) {
        return constraints.validator(data);
    }

    return true;
}

bool TypeValidator::validateSize(BytecodeType type, size_t dataSize)
{
    if (m_typeConstraints.empty()) {
        initializeConstraints();
    }

    auto it = m_typeConstraints.find(type);
    if (it == m_typeConstraints.end()) {
        return false;
    }

    const auto& constraints = it->second;

    if (constraints.fixedSize > 0) {
        return dataSize == constraints.fixedSize;
    }

    return dataSize >= constraints.minSize && dataSize <= constraints.maxSize;
}

const TypeValidator::TypeConstraints& TypeValidator::getConstraints(
    BytecodeType type
)
{
    if (m_typeConstraints.empty()) {
        initializeConstraints();
    }

    static TypeConstraints defaultConstraints;
    auto it = m_typeConstraints.find(type);
    return (it != m_typeConstraints.end()) ? it->second : defaultConstraints;
}

bool TypeValidator::isRestrictedType(BytecodeType type)
{
    const auto& constraints = getConstraints(type);

    return constraints.fixedSize > 0 ||
           constraints.maxSize < GENESIS_STACK_MEMORY_POLICY ||
           constraints.requiresValidation;
}

size_t TypeValidator::getMaxSize(BytecodeType type)
{
    return getConstraints(type).maxSize;
}

size_t TypeValidator::getMinSize(BytecodeType type)
{
    return getConstraints(type).minSize;
}

bool TypeValidator::isValidHex(const std::string& hex)
{
    if (hex.empty()) {
        return false;
    }

    std::string hexData = hex;

    if (hexData.length() >= 2 &&
        (hexData.substr(0, 2) == "0x" || hexData.substr(0, 2) == "0X")) {
        hexData = hexData.substr(2);
    }

    if (hexData.empty()) {
        return false;
    }

    if (hexData.length() % 2 != 0) {
        return false;
    }

    return std::all_of(hexData.begin(), hexData.end(), [](char c) {
        return std::isxdigit(c);
    });
}

size_t TypeValidator::getHexDataSize(const std::string& hex)
{
    std::string hexData = tbc::hexData(hex);

    if (hexData.empty()) {
        return 0;
    }

    // 奇数位前补 0 (如 "a" -> "0a")
    if (hexData.length() % 2 != 0) {
        hexData = "0" + hexData;
    }

    return hexData.length() / 2;
}

bool TypeValidator::isValidPubKey(const std::string& pubkey)
{
    if (!isValidHex(pubkey)) {
        return false;
    }

    size_t size = getHexDataSize(pubkey);

    // 压缩公钥: 33 字节, 02/03 前缀
    if (size == 33) {
        return pubkey.substr(0, 2) == "02" || pubkey.substr(0, 2) == "03";
    }

    // 未压缩公钥: 65 字节, 04 前缀
    if (size == 65) {
        return pubkey.substr(0, 2) == "04";
    }

    return false;
}

bool TypeValidator::isValidDERSignature(const std::string& signature)
{
    if (!isValidHex(signature)) {
        return false;
    }

    size_t size = getHexDataSize(signature);
    if (size < 6 || size > 73) {
        return false;
    }

    // DER 头部 0x30
    if (signature.substr(0, 2) != "30") {
        return false;
    }

    return true;
}

bool TypeValidator::isValidHash(const std::string& hash, size_t expectedSize)
{
    if (!isValidHex(hash)) {
        return false;
    }

    return getHexDataSize(hash) == expectedSize;
}

bool TypeValidator::isValidPrivKey(const std::string& privkey)
{
    if (!isValidHex(privkey)) {
        return false;
    }

    if (getHexDataSize(privkey) != 32) {
        return false;
    }

    // 简化: 仅排除全 0; 严格应校验在 secp256k1 阶内
    return privkey != std::string(64, '0');
}

bool TypeValidator::isValidInt32(int64_t value)
{
    return value >= INT32_MIN && value <= INT32_MAX;
}

std::string
TypeValidator::getValidationError(BytecodeType type, const std::string& data)
{
    if (m_typeConstraints.empty()) {
        initializeConstraints();
    }

    auto it = m_typeConstraints.find(type);
    if (it == m_typeConstraints.end()) {
        return "Unknown type";
    }

    const auto& constraints = it->second;
    size_t dataSize = getHexDataSize(data);

    if (constraints.fixedSize > 0 && dataSize != constraints.fixedSize) {
        return "Data size must be " + std::to_string(constraints.fixedSize) +
               " bytes, actual is " + std::to_string(dataSize) + " bytes";
    }

    if (dataSize < constraints.minSize) {
        return "Data size cannot be less than " +
               std::to_string(constraints.minSize) + " bytes";
    }

    if (dataSize > constraints.maxSize) {
        return "Data size cannot be greater than " +
               std::to_string(constraints.maxSize) +
               " bytes (current limit: 100MB)";
    }

    if (constraints.requiresValidation && constraints.validator &&
        !constraints.validator(data)) {
        return "Data format does not meet the requirements for " +
               constraints.description;
    }

    return "Validation passed";
}

bool TypeValidator::isValidRabinSignature(const std::string& signature)
{
    if (!isValidHex(signature)) {
        return false;
    }

    size_t dataSize = getHexDataSize(signature);

    // Rabin 签名: 64-1024 字节
    if (dataSize < 64 || dataSize > 1024) {
        return false;
    }

    // TODO: Rabin 签名结构性校验
    return true;
}

bool TypeValidator::isValidRabinPubKey(const std::string& pubkey)
{
    if (!isValidHex(pubkey)) {
        return false;
    }

    size_t dataSize = getHexDataSize(pubkey);

    // Rabin 公钥: 64-512 字节
    if (dataSize < 64 || dataSize > 512) {
        return false;
    }

    // TODO: Rabin 公钥数学属性校验
    return true;
}
