#include "agent/session.hpp"

#include "tools/tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <unordered_map>

#include "config.hpp"

namespace realagent {
namespace fs = std::filesystem;

namespace {

/* 会话 id：`YYYYMMDD-HHMMSS-xxxx`。按字典序排 = 按时间排，所以清单不需要索引文件；
 * 后四位随机是为了同一秒内开两个会话不撞名（真会发生：客户端重连时连按两下 /new）。 */
std::string make_id()
{
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

    static std::mt19937 rng{std::random_device{}()};
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%04x", (unsigned)(rng() & 0xffff));
    return std::string(stamp) + "-" + suffix;
}

fs::path path_of(const std::string &dir, const std::string &id)
{
    return fs::path(dir) / (id + ".jsonl");
}

/* 标题：第一条 user 消息的第一个 text 块，截到 60 字节。
 * 从内容里现取，不另存——另存就得管它什么时候失效。 */
constexpr std::size_t kTitleMax = 60;

std::string title_of(const nlohmann::json &msg)
{
    // value() 在非对象上会抛。坏行是常态（断电写了一半），不能让它变成一次崩溃
    if (!msg.is_object() || msg.value("role", std::string()) != "user") return "";
    // find 在非对象上恒返回 end()，所以不必先问一句 is_object
    const auto content = msg.find("content");
    if (content == msg.end() || !content->is_array()) return "";
    for (const nlohmann::json &b : *content)
    {
        if (!b.is_object() || b.value("type", std::string()) != "text") continue;
        std::string s = b.value("text", std::string());
        // 换行会把清单一行撑成多行，直接压成空格
        std::replace(s.begin(), s.end(), '\n', ' ');
        if (s.size() > kTitleMax)
        {
            // UTF-8 不许切在半个字符上：退到最近的字符起始字节
            std::size_t cut = kTitleMax;
            while (cut > 0 && (s[cut] & 0xC0) == 0x80) --cut;
            s = s.substr(0, cut) + "…";
        }
        return s;
    }
    return "";
}

/* 帧的形状是 PROTOCOL.md 说了算的，这里只负责按同样的形状再摆一遍 */
void frame(nlohmann::json &out, const char *type, nlohmann::json data)
{
    out.push_back(nlohmann::json{{"type", type}, {"data", std::move(data)}});
}

std::string str(const nlohmann::json &j, const char *key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

Session::Session(std::string dir, std::string id)
    : dir_(std::move(dir)), id_(id.empty() ? make_id() : std::move(id)), path_(path_of(dir_, id_).string())
{
}

void Session::append(const nlohmann::json &msg)
{
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    std::ofstream f(path_, std::ios::app);
    if (!f)
    {
        fprintf(stderr, "[session] 写不进 %s，本条未落盘\n", path_.c_str());
        return;
    }
    // 一行一条，不缩进——JSONL 的行边界就是记录边界，dump 里出现换行就全散了
    f << msg.dump() << '\n';
}

bool Session::resume(const std::string &id, nlohmann::json &out)
{
    if (!read(dir_, id, out)) return false;
    id_ = id;
    path_ = path_of(dir_, id).string();
    return true;
}

std::string sessions_dir(const std::string &workdir)
{
    return (fs::path(workdir) / ".realagent" / "sessions").string();
}

bool Session::exists(const std::string &dir, const std::string &id)
{
    return fs::exists(path_of(dir, id));
}

bool Session::read(const std::string &dir, const std::string &id, nlohmann::json &out)
{
    if (id.empty() || id.find('/') != std::string::npos) return false;
    const fs::path p = path_of(dir, id);
    std::ifstream f(p);
    if (!f) return false;

    nlohmann::json msgs = nlohmann::json::array();
    std::string line;
    long long lineno = 0;
    while (std::getline(f, line))
    {
        ++lineno;
        if (line.empty()) continue;
        nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded())
        {
            // 坏行跳过（断电时写了一半的末行），不让整个会话作废
            fprintf(stderr, "[session] %s:%lld 不是合法 JSON，跳过\n", p.c_str(), lineno);
            continue;
        }
        msgs.push_back(std::move(parsed));
    }
    out = std::move(msgs);
    return true;
}

std::vector<SessionInfo> Session::list(const std::string &dir_arg)
{
    std::vector<SessionInfo> out;
    std::error_code ec;
    const fs::path dir(dir_arg);
    if (!fs::is_directory(dir, ec)) return out; // 还没开过会话，不是错

    for (const auto &e : fs::directory_iterator(dir, ec))
    {
        if (ec) break;
        if (!e.is_regular_file() || e.path().extension() != ".jsonl") continue;

        std::ifstream f(e.path());
        if (!f) continue;
        SessionInfo info;
        info.id = e.path().stem().string();
        info.messages = 0;
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty()) continue;
            ++info.messages;
            if (!info.title.empty()) continue;
            if (const nlohmann::json m = nlohmann::json::parse(line, nullptr, false); !m.is_discarded())
                info.title = title_of(m);
        }
        // 这个 libc++ 没有 clock_cast：用两个时钟的"此刻"之差换算
        const auto tp = fs::last_write_time(e.path(), ec);
        info.mtime = 0;
        if (!ec)
        {
            const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                tp - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            info.mtime = std::chrono::duration_cast<std::chrono::seconds>(
                             sys.time_since_epoch())
                             .count();
        }
        out.push_back(std::move(info));
    }
    // 最近写过的在前：恢复会话十有八九是恢复上一个
    std::sort(out.begin(), out.end(),
              [](const SessionInfo &a, const SessionInfo &b) { return a.mtime > b.mtime; });
    return out;
}

nlohmann::json Session::to_frames(const nlohmann::json &messages)
{
    nlohmann::json out = nlohmann::json::array();
    if (!messages.is_array()) return out;

    // tool_use_id → 工具名：tool_execution_end 帧要名字，result 里没有。tool_use 总在前
    std::unordered_map<std::string, std::string> tool_names;

    for (const auto &msg : messages)
    {
        const std::string role = str(msg, "role");
        const auto content = msg.find("content");
        if (content == msg.end() || !content->is_array()) continue;

        // assistant 消息就是一个 turn 的产出：思考、正文、要调的工具
        if (role == "assistant") frame(out, "turn_start", nlohmann::json::object());

        for (const auto &b : *content)
        {
            const std::string type = str(b, "type");
            if (type == "text" && role == "user")
            {
                // 收件箱三种来源都是 user，发信人写在正文里
                frame(out, "message_start", nlohmann::json{{"role", "user"}, {"text", str(b, "text")}});
            }
            else if (type == "text")
            {
                // 实时是一串 delta，回放是一整块，帧类型相同
                frame(out, "message_update", nlohmann::json{{"delta", str(b, "text")}});
            }
            else if (type == "thinking")
            {
                frame(out, "thinking_start", nlohmann::json{{"signature", str(b, "signature")}});
                frame(out, "thinking_update", nlohmann::json{{"delta", str(b, "thinking")}});
                frame(out, "thinking_stop", nlohmann::json::object());
            }
            else if (type == "tool_use")
            {
                const std::string id = str(b, "id"), name = str(b, "name");
                tool_names[id] = name;
                frame(out, "tool_execution_start", nlohmann::json{{"name", name}, {"id", id}});
            }
            else if (type == "tool_result")
            {
                const std::string id = str(b, "tool_use_id");
                const bool err = b.value("is_error", false);
                // 同上：实时是一串 tool_output，回放是一整块
                frame(out, "tool_output",
                      nlohmann::json{{"call_id", id},
                                     {"stream", "output"},
                                     {"text", tool_content_text(b.at("content"))}});
                frame(out, "tool_execution_end", nlohmann::json{{"name", tool_names[id]}, {"id", id}, {"status", err ? 1 : 0}, {"interrupted", false}});
            }
        }

        if (role == "assistant") frame(out, "turn_end", nlohmann::json::object());
    }
    return out;
}

nlohmann::json Session::read_frames(const std::string &dir, const std::string &id)
{
    nlohmann::json msgs;
    if (!read(dir, id, msgs)) return nlohmann::json::array();
    return to_frames(msgs);
}

} // namespace realagent
