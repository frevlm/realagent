/*
 * session.hpp — 会话持久化：`<workdir>/.realagent/sessions/<id>.jsonl`
 *
 * 一行就是内存里 messages 的一条，原样，没有信封：落盘与内存同形，恢复就是读取，
 * 不需要会丢信息的转换函数。清单元数据不另存：id 按时间可排序，条数是行数，
 * 标题是第一条 user 消息。
 */
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace realagent {

/* 会话清单一条。字段名即 PROTOCOL.md 的响应契约。 */
struct SessionInfo {
    std::string id;         // 文件名去掉 .jsonl，形如 20260816-143022-a1b2
    std::string title;      // 第一条 user 消息（截断）
    long long messages = 0; // 行数
    long long mtime = 0;    // Unix 秒
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionInfo, id, title, messages, mtime)

class Session {
  public:
    /* 新会话：生成 id，不建文件（空会话不进清单），第一次 append 时才出现。 */
    explicit Session(std::string dir);

    const std::string &id() const { return id_; }

    /* 追加一行。写不进去只报 stderr，不打断对话。 */
    void append(const nlohmann::json &msg);

    /* 切到已有会话并读进 out。读不到返回 false，本对象不变。 */
    bool resume(const std::string &id, nlohmann::json &out);

    /* 按 mtime 倒序。目录不存在返回空。 */
    static std::vector<SessionInfo> list(const std::string &dir);

    static bool read(const std::string &dir, const std::string &id, nlohmann::json &out);

    /* 消息历史 → 事件帧 `[{"type", "data"}]`，与实时推送同形（ADR-0020）。 */
    static nlohmann::json to_frames(const nlohmann::json &messages);
    static nlohmann::json read_frames(const std::string &dir, const std::string &id);

  private:
    std::string dir_;
    std::string id_;
    std::string path_;
};

} // namespace realagent
