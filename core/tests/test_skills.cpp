/*
 * test_skills.cpp — skill 扫盘与 frontmatter 解析（ADR-0022）
 *
 * 直接编译 src/agent/skills.cpp + src/config.cpp，不起服务、不碰网络。验证：
 *   - 两处来源都扫得到，同名时 workdir 那份覆盖全局那份
 *   - plugin 来的名字带前缀（`<plugin>:<目录名>`），不同名 plugin 之间撞不上（ADR-0024）
 *   - `description: >`（YAML 折叠标量）解析成一整句——这正是不手写解析器的理由
 *     （值原样交出，YAML 怎么定义就是什么，core 不加工）
 *   - 名字取目录名，路径是绝对路径
 *   - 没有 SKILL.md 的目录不是 skill；坏 frontmatter 跳过且不牵连别的 skill
 *   - 清单为空时提示词是空串（system prompt 与加这个功能之前一个字不差）
 *
 * 隔离：HOME 与 workdir 都指到临时目录——全局那一处是从 HOME 算出来的。
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/skills.hpp"

namespace fs = std::filesystem;
using realagent::scan_skills;
using realagent::Skill;
using realagent::skills_prompt;

static int failures = 0;
#define CHECK(cond, msg)                 \
    do                                   \
    {                                    \
        if (cond)                        \
        {                                \
            printf("  ok: %s\n", msg);   \
        }                                \
        else                             \
        {                                \
            printf("  FAIL: %s\n", msg); \
            ++failures;                  \
        }                                \
    } while (0)

/* 写一份 <root>/.realagent/skills/<name>/SKILL.md，内容原样给 */
static void put_skill(const fs::path &root, const std::string &name, const std::string &body)
{
    const fs::path dir = root / ".realagent" / "skills" / name;
    fs::create_directories(dir);
    std::ofstream(dir / "SKILL.md") << body;
}

/* 写一份装来的 plugin 里的 skill：<root>/.realagent/plugins/<plugin>/skills/<name>/SKILL.md
 * 注意布局跟隐式那两处不同——装来的用 Claude Code 的文件名（ADR-0024 §1）。 */
static void put_plugin_skill(const fs::path &root, const std::string &plugin,
                             const std::string &name, const std::string &body)
{
    const fs::path dir = root / ".realagent" / "plugins" / plugin / "skills" / name;
    fs::create_directories(dir);
    std::ofstream(dir / "SKILL.md") << body;
}

static const Skill *find(const std::vector<Skill> &v, const std::string &name)
{
    for (const Skill &s : v)
        if (s.name == name) return &s;
    return nullptr;
}

int main()
{
    const fs::path tmp = fs::temp_directory_path() / "realagent_skills_test";
    fs::remove_all(tmp);
    const fs::path home = tmp / "home";
    const fs::path work = tmp / "work";
    fs::create_directories(home);
    fs::create_directories(work);
    setenv("HOME", home.c_str(), 1);

    printf("== 空目录 ==\n");
    {
        const std::vector<Skill> v = scan_skills(work.string());
        CHECK(v.empty(), "一个 skill 都没有时清单是空的");
        CHECK(skills_prompt(v).empty(), "空清单的提示词是空串");
    }

    printf("== 两处来源 + 同名覆盖 ==\n");
    put_skill(home, "commit", "---\nname: commit\ndescription: 全局那份\n---\n正文\n");
    put_skill(home, "onlyglobal", "---\nname: onlyglobal\ndescription: 只在全局\n---\n");
    put_skill(work, "commit", "---\nname: commit\ndescription: 仓库那份\n---\n");
    put_skill(work, "onlylocal", "---\nname: onlylocal\ndescription: 只在仓库\n---\n");
    {
        const std::vector<Skill> v = scan_skills(work.string());
        CHECK(v.size() == 3, "三个 skill（同名的两份算一个）");
        const Skill *c = find(v, "commit");
        CHECK(c != nullptr && c->description == "仓库那份", "同名时 workdir 那份赢");
        CHECK(find(v, "onlyglobal") != nullptr, "只在全局的那份也在");
        CHECK(find(v, "onlylocal") != nullptr, "只在仓库的那份也在");
        CHECK(v[0].name <= v[1].name && v[1].name <= v[2].name, "按名字排序，两次运行同一份清单");
        CHECK(c != nullptr && fs::path(c->path).is_absolute(), "路径是绝对路径");
        CHECK(c != nullptr && c->path.ends_with("SKILL.md"), "路径指到 SKILL.md 本身");
    }

    printf("== 折叠标量（现实里三成 skill 这么写）==\n");
    put_skill(work, "folded", "---\nname: folded\ndescription: >\n  第一行\n  第二行\n---\n正文\n");
    {
        const std::vector<Skill> v = scan_skills(work.string());
        const Skill *f = find(v, "folded");
        CHECK(f != nullptr && f->description == "第一行 第二行\n", "`>` 折成一整句，不是一个 `>`");
    }

    printf("== 名字取目录名，不读 frontmatter 里的 name ==\n");
    put_skill(work, "dirname-wins", "---\nname: something-else\ndescription: 描述\n---\n");
    {
        const std::vector<Skill> v = scan_skills(work.string());
        CHECK(find(v, "dirname-wins") != nullptr, "叫的是目录名");
        CHECK(find(v, "something-else") == nullptr, "frontmatter 里那个 name 不作数");
    }

    printf("== 坏 skill 跳过，不牵连别的 ==\n");
    fs::create_directories(work / ".realagent" / "skills" / "no-md"); // 目录里没有 SKILL.md
    put_skill(work, "no-frontmatter", "这份文件直接就是正文\n");
    put_skill(work, "no-description", "---\nname: no-description\nlicense: MIT\n---\n");
    put_skill(work, "bad-yaml", "---\nname: bad\ndescription: [未闭合\n---\n");
    put_skill(work, "empty-description", "---\nname: e\ndescription: \"\"\n---\n");
    {
        const std::vector<Skill> v = scan_skills(work.string());
        CHECK(find(v, "no-md") == nullptr, "没有 SKILL.md 的目录不是 skill");
        CHECK(find(v, "no-frontmatter") == nullptr, "没有 frontmatter 的跳过");
        CHECK(find(v, "no-description") == nullptr, "没有 description 的跳过");
        CHECK(find(v, "bad-yaml") == nullptr, "YAML 解析失败的跳过");
        CHECK(find(v, "empty-description") == nullptr, "description 是空串的跳过");
        CHECK(find(v, "onlylocal") != nullptr, "好的那些一个不少");
        CHECK(find(v, "commit") != nullptr, "坏 skill 不牵连同目录的别人");
    }

    printf("== plugin 来的 skill（ADR-0024）==\n");
    {
        const std::string md = "---\ndescription: 装来的\n---\n";
        put_plugin_skill(home, "p1", "review", md);
        put_plugin_skill(home, "p2", "review", md);                       // 另一个 plugin，同一个 skill 名
        put_skill(home, "review", "---\ndescription: 我自己写的\n---\n"); // 隐式，无前缀
        const std::vector<Skill> v = scan_skills(work.string());
        CHECK(find(v, "p1:review") != nullptr, "装来的 skill 名字是 <plugin>:<目录名>");
        CHECK(find(v, "p2:review") != nullptr, "不同名 plugin 的同名 skill 不互相覆盖");
        const Skill *mine = find(v, "review");
        CHECK(mine != nullptr && mine->description == "我自己写的",
              "隐式 plugin 前缀为空——既有的名字一个字符都不变");
    }

    printf("== 同名 plugin 的两处：近的覆盖远的 ==\n");
    {
        put_plugin_skill(home, "dup", "one", "---\ndescription: 全局那份\n---\n");
        put_plugin_skill(work, "dup", "one", "---\ndescription: 仓库那份\n---\n");
        const std::vector<Skill> v = scan_skills(work.string());
        const Skill *d = find(v, "dup:one");
        CHECK(d != nullptr && d->description == "仓库那份", "同一个 plugin 的两份，workdir 那份赢");
        int n = 0;
        for (const Skill &k : v)
            if (k.name == "dup:one") ++n;
        CHECK(n == 1, "覆盖不是并存：只剩一条");
    }

    printf("== 提示词形状 ==\n");
    {
        const std::vector<Skill> v = scan_skills(work.string());
        const std::string p = skills_prompt(v);
        const Skill *c = find(v, "commit");
        CHECK(p.find("- commit: 仓库那份 (") != std::string::npos, "一行一个：名字、描述、路径");
        CHECK(c != nullptr && p.find(c->path) != std::string::npos, "路径原样写进去，模型照着 read");
        CHECK(p.starts_with("\n\n"), "接在 system prompt 后面，不是顶头");
    }

    fs::remove_all(tmp);
    printf(failures == 0 ? "\nPASS\n" : "\nFAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
