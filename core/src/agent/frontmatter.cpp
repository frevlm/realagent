/*
 * frontmatter.cpp — 切出 frontmatter，交给 fkYAML
 */
#include "agent/frontmatter.hpp"

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

} // namespace realagent
