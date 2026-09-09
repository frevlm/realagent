/*
 * frontmatter.cpp — 切出 frontmatter，交给 fkYAML
 */
#include "agent/frontmatter.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

/* fkYAML 0.4.4 用了 std::is_trivial，C++26 里它被弃用了。头文件是逐字节 vendored 的
 * （与 json.hpp 同一条路子），不改它一个字符——升级时才好逐字节替换。 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "fkYAML.hpp"
#pragma clang diagnostic pop

namespace realagent {

namespace {

/* 去掉行尾空白：`---` 后面跟一个 \r（Windows 换行）仍然是 `---` */
std::string_view rstrip(std::string_view s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

} // namespace

std::optional<Frontmatter> read_frontmatter(const std::filesystem::path &md, bool with_body,
                                            std::string &err)
{
    std::ifstream f(md);
    std::string line;
    if (!std::getline(f, line) || rstrip(line) != "---")
    {
        err = "没有 YAML frontmatter";
        return std::nullopt;
    }
    std::ostringstream yaml;
    bool closed = false;
    while (std::getline(f, line))
    {
        if (rstrip(line) == "---")
        {
            closed = true;
            break;
        }
        yaml << line << '\n';
    }
    if (!closed)
    {
        err = "frontmatter 开了头没收尾"; // 那就不是 frontmatter
        return std::nullopt;
    }

    Frontmatter out;
    if (with_body)
    {
        std::ostringstream body;
        body << f.rdbuf();
        out.body = body.str();
    }
    try
    {
        fkyaml::node n = fkyaml::node::deserialize(yaml.str());
        if (!n.is_mapping())
        {
            err = "frontmatter 不是一组键值";
            return std::nullopt;
        }
        // 只收字符串标量。别的类型不是错，是 core 不要——跳过就好
        for (const auto &[k, v] : n.as_map())
            if (v.is_string()) out.fields[k.as_str()] = v.get_value<std::string>();
    } catch (const std::exception &e)
    {
        err = std::string("frontmatter 解析失败：") + e.what();
        return std::nullopt;
    }
    return out;
}

std::vector<std::pair<std::string, std::filesystem::path>>
md_files(const PluginRoot &r, const std::filesystem::path &dir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<std::string, fs::path>> out;
    if (!fs::is_directory(dir, ec)) return out;
    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".md")
            out.emplace_back(r.qualify(e.path().stem().string()), e.path());
    std::sort(out.begin(), out.end(),
              [](const auto &a, const auto &b) { return a.second < b.second; });
    return out;
}

std::optional<Frontmatter> load_md(const std::filesystem::path &md, const PluginRoot &r,
                                   const char *tag, bool with_body)
{
    std::string err;
    std::optional<Frontmatter> fm = read_frontmatter(md, with_body, err);
    if (!fm)
    {
        fprintf(stderr, "[%s] %s: %s，跳过\n", tag, md.c_str(), err.c_str());
        return std::nullopt;
    }
    /* 正文里的 ${CLAUDE_PLUGIN_ROOT}：隐式 plugin 那儿没有 plugin root，出现它就是错
     * （ADR-0024 §4）。with_body 为假时正文是空串，这一步什么都不做。 */
    if (!expand_plugin_root(fm->body, r.root))
    {
        fprintf(stderr, "[%s] %s: 正文里有 %s，它只在 plugin 里有意义，跳过\n", tag, md.c_str(),
                kPluginRootVar);
        return std::nullopt;
    }
    return fm;
}

} // namespace realagent
