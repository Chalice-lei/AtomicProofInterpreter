#include "import_resolver.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#endif

#include "../config/config_manager.h"

namespace fs = std::filesystem;

namespace
{
// `import <target>`: target 为 dotted path 或双引号相对路径
const std::regex kImportRegex(
    R"(^\s*import\s+(?:\"([^\"]+)\"|([A-Za-z_][A-Za-z0-9_.]*))\s*$)");

// 当前可执行文件所在目录（已解析 symlink），失败返回空 path
fs::path getExecutableDir()
{
    std::error_code ec;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH)
        return {};
    fs::path p(std::wstring(buf, n));
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return {};
    fs::path p(buf);
#else
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (ec)
        return {};
#endif
    fs::path canon = fs::weakly_canonical(p, ec);
    if (ec)
        canon = p;
    return canon.parent_path();
}

} // namespace

std::string ImportResolver::readFile(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs)
        throw std::runtime_error("import: cannot open file '" + path + "'");
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::string ImportResolver::findStdlibRoot()
{
    if (const char* env = std::getenv("APC_STDLIB_PATH")) {
        if (*env && fs::exists(env))
            return fs::absolute(env).string();
    }

    auto& cfg = ConfigManager::getInstance();
    if (auto v = cfg.getConfigValue<std::string>("paths.stdlib")) {
        if (!v->empty() && fs::exists(*v))
            return fs::absolute(*v).string();
    }

    // 基于 exe 位置查找：标准安装 / 便携 tarball / 开发构建
    fs::path exeDir = getExecutableDir();
    if (!exeDir.empty()) {
        const fs::path candidates[] = {
            exeDir / ".." / "share" / "apc" / "stdlib", // <prefix>/bin + share
            exeDir / "stdlib",                          // 便携：exe 同级
            exeDir / ".." / "stdlib",                   // 便携：bin/ 上一级
            exeDir / ".." / ".." / "stdlib",            // 开发：build/bin/ -> 项目根
        };
        std::error_code ec;
        for (const fs::path& cand : candidates) {
            fs::path canon = fs::weakly_canonical(cand, ec);
            if (!ec && fs::exists(canon))
                return canon.string();
            ec.clear();
        }
    }

#ifdef APC_DEFAULT_STDLIB_PATH
    if (fs::exists(APC_DEFAULT_STDLIB_PATH))
        return APC_DEFAULT_STDLIB_PATH;
#endif

    fs::path cwdLocal = fs::current_path() / "stdlib";
    if (fs::exists(cwdLocal))
        return cwdLocal.string();

    throw std::runtime_error(
        "import: cannot locate stdlib root (set APC_STDLIB_PATH or "
        "paths.stdlib in user_preferences.json)");
}

std::string ImportResolver::resolveImportTarget(
    const std::string& spec, const std::string& currentFileDir)
{
    // / 或 . 开头视作相对路径；否则 dotted 在 stdlib 下将 '.' 替换为 '/' 并加 .ct
    bool isRelative =
        (!spec.empty() &&
         (spec[0] == '.' || spec[0] == '/' || spec.find('/') != std::string::npos));

    if (isRelative) {
        fs::path p(spec);
        if (p.extension().empty())
            p += ".ct";
        fs::path full =
            p.is_absolute() ? p : fs::path(currentFileDir) / p;
        return fs::weakly_canonical(full).string();
    }

    // dotted
    std::string relPath = spec;
    std::replace(relPath.begin(), relPath.end(), '.', '/');
    relPath += ".ct";
    fs::path full = fs::path(findStdlibRoot()) / relPath;
    if (!fs::exists(full))
        throw std::runtime_error(
            "import: module '" + spec + "' not found at " + full.string());
    return fs::weakly_canonical(full).string();
}

void ImportResolver::appendMappedLine(std::ostringstream& out,
                                      SourceMap& sourceMap,
                                      const std::string& line,
                                      const std::string& filename,
                                      int sourceLine,
                                      bool generated)
{
    out << line << "\n";
    sourceMap.addLine(filename, sourceLine, generated);
}

void ImportResolver::processSourceLines(
    const std::string& source,
    const std::string& mappedFilename,
    const std::string& importDir,
    const std::string& errorFilename,
    std::vector<std::string>& stack,
    std::ostringstream& out,
    SourceMap& sourceMap,
    std::unordered_map<std::string, std::string>& sourceContents)
{
    std::istringstream iss(source);
    std::string line;
    int lineNo = 0;

    while (std::getline(iss, line)) {
        ++lineNo;
        std::smatch m;
        if (std::regex_match(line, m, kImportRegex)) {
            std::string spec = m[1].matched ? m[1].str() : m[2].str();
            try {
                std::string target = resolveImportTarget(spec, importDir);
                appendMappedLine(
                    out,
                    sourceMap,
                    "# --- begin import " + spec + " (" + target + ") ---",
                    "",
                    0,
                    true
                );
                resolveFile(target, stack, out, sourceMap, sourceContents);
                appendMappedLine(
                    out,
                    sourceMap,
                    "# --- end import " + spec + " ---",
                    "",
                    0,
                    true
                );
            } catch (const std::exception& e) {
                throw std::runtime_error(
                    errorFilename + ":" + std::to_string(lineNo) + ": " +
                    e.what()
                );
            }
            continue;
        }
        appendMappedLine(out, sourceMap, line, mappedFilename, lineNo, false);
    }
}

void ImportResolver::resolveFile(
    const std::string& absPath,
    std::vector<std::string>& stack,
    std::ostringstream& out,
    SourceMap& sourceMap,
    std::unordered_map<std::string, std::string>& sourceContents)
{
    if (std::find(stack.begin(), stack.end(), absPath) != stack.end()) {
        std::string chain;
        for (auto& p : stack)
            chain += p + " -> ";
        chain += absPath;
        throw std::runtime_error("import: circular import detected: " + chain);
    }
    if (m_alreadyIncluded.count(absPath))
        return; // 幂等：重复 import 产生空

    m_alreadyIncluded.insert(absPath);
    stack.push_back(absPath);

    std::string raw = readFile(absPath);
    sourceContents[absPath] = raw;
    std::string dir = fs::path(absPath).parent_path().string();
    processSourceLines(
        raw, absPath, dir, absPath, stack, out, sourceMap, sourceContents
    );

    stack.pop_back();
}

std::string ImportResolver::resolve(
    const std::string& source, const std::string& sourceFilePath)
{
    return resolveWithMap(source, sourceFilePath).code;
}

ExpandedSource ImportResolver::resolveWithMap(
    const std::string& source, const std::string& sourceFilePath)
{
    m_alreadyIncluded.clear();

    std::ostringstream out;
    SourceMap sourceMap;
    std::unordered_map<std::string, std::string> sourceContents;
    std::string rootFilename = sourceFilePath.empty() ? std::string("<main>")
                                                      : sourceFilePath;
    std::string dir =
        sourceFilePath.empty() ? fs::current_path().string()
                               : fs::path(sourceFilePath).parent_path().string();
    if (dir.empty())
        dir = fs::current_path().string();

    std::vector<std::string> stack;
    std::string rootKey =
        sourceFilePath.empty() ? std::string("<main>")
                               : fs::weakly_canonical(sourceFilePath).string();
    m_alreadyIncluded.insert(rootKey);
    sourceContents[rootFilename] = source;

    processSourceLines(
        source,
        rootFilename,
        dir,
        rootFilename,
        stack,
        out,
        sourceMap,
        sourceContents
    );

    ExpandedSource expanded;
    expanded.code = out.str();
    expanded.sourceMap = std::move(sourceMap);
    expanded.sourceContents = std::move(sourceContents);
    return expanded;
}
