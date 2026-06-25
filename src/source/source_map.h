#ifndef SOURCE_MAP_H
#define SOURCE_MAP_H

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct SourceLineMapping
{
    std::string filename;
    int line = 0;
    bool generated = false;

    SourceLineMapping() = default;
    SourceLineMapping(std::string file, int sourceLine, bool isGenerated)
        : filename(std::move(file)), line(sourceLine), generated(isGenerated)
    {}

    bool isValid() const
    {
        return !generated && !filename.empty() && line > 0;
    }
};

class SourceMap
{
public:
    void addLine(const std::string& filename,
                 int sourceLine,
                 bool generated = false)
    {
        m_lines.emplace_back(filename, sourceLine, generated);
    }

    const SourceLineMapping* lookup(int expandedLine) const
    {
        if (expandedLine <= 0 ||
            static_cast<size_t>(expandedLine) > m_lines.size()) {
            return nullptr;
        }
        return &m_lines[static_cast<size_t>(expandedLine - 1)];
    }

    bool empty() const
    {
        return m_lines.empty();
    }

    size_t size() const
    {
        return m_lines.size();
    }

private:
    std::vector<SourceLineMapping> m_lines;
};

struct ExpandedSource
{
    std::string code;
    SourceMap sourceMap;
    std::unordered_map<std::string, std::string> sourceContents;
};

#endif // SOURCE_MAP_H
