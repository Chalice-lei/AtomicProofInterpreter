#ifndef IMPORT_RESOLVER_H
#define IMPORT_RESOLVER_H

#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../source/source_map.h"

// 源码级 import 解析器
//
// 语法：
//   import std.p2pkh            -> <stdlib>/std/p2pkh.ct
//   import "./relative/path"    -> 相对 import 所在文件目录
//
// 递归内联展开；重复幂等，循环报错。
//
// stdlib 根目录解析顺序：
//   1. APC_STDLIB_PATH 环境变量
//   2. user_preferences.json 的 paths.stdlib
//   3. 基于 exe 位置（标准安装 / 便携 / 开发构建）
//   4. 编译时宏 APC_DEFAULT_STDLIB_PATH
//   5. 当前工作目录下的 stdlib/
class ImportResolver
{
public:
    // sourceFilePath 用于解析相对 import 以及错误定位
    std::string resolve(
        const std::string& source, const std::string& sourceFilePath);
    ExpandedSource resolveWithMap(
        const std::string& source, const std::string& sourceFilePath);

private:
    void resolveFile(const std::string& absPath,
                     std::vector<std::string>& stack,
                     std::ostringstream& out,
                     SourceMap& sourceMap,
                     std::unordered_map<std::string, std::string>&
                         sourceContents);
    void processSourceLines(
        const std::string& source,
        const std::string& mappedFilename,
        const std::string& importDir,
        const std::string& errorFilename,
        std::vector<std::string>& stack,
        std::ostringstream& out,
        SourceMap& sourceMap,
        std::unordered_map<std::string, std::string>& sourceContents);
    std::string readFile(const std::string& path);
    std::string resolveImportTarget(
        const std::string& spec, const std::string& currentFileDir);
    std::string findStdlibRoot();
    void appendMappedLine(std::ostringstream& out,
                          SourceMap& sourceMap,
                          const std::string& line,
                          const std::string& filename,
                          int sourceLine,
                          bool generated);

    std::unordered_set<std::string> m_alreadyIncluded;
};

#endif // IMPORT_RESOLVER_H
