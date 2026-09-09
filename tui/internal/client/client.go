// Package client 封装 core 的 QUIC/HTTP3 客户端（PROTOCOL.md）
package client

import (
	"bufio"
	"bytes"
	"crypto/rand"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/quic-go/quic-go/http3"
)

// Client 是 core 的 QUIC/HTTP3 客户端
type Client struct {
	hc  *http.Client
	rt  *http3.Transport
	url string // 基地址 https://<addr>，路径按端点拼接

	// 这个客户端在跟哪个 agent 说话。core 里同时活着多个（ADR-0019），
	// 每个动 agent 的端点都要指名道姓——core 不猜"就那一个吧"。
	agentID int

	// 这个客户端拥有的那一组 agent（ADR-0021）。**每个进程一个、不落盘**：
	// 三个终端窗口就是三个 id、三组，互不干扰。core 里不存在没有所有者的 agent。
	clientID string
}

// Reply 是请求-响应端点的通用响应
type Reply struct {
	Status   string          `json:"status"`
	Reply    string          `json:"reply,omitempty"`
	Error    string          `json:"error,omitempty"`
	Ok       bool            `json:"ok,omitempty"`
	Command  string          `json:"command,omitempty"`
	Data     json.RawMessage `json:"data,omitempty"` // 斜杠命令结果载荷（如 /model 的 []ModelInfo JSON）
	Messages int             `json:"messages,omitempty"`
	AgentID  int             `json:"agent_id,omitempty"`
}

// Command 是一条可用的斜杠命令（GET /commands，core 是唯一真相源）
type Command struct {
	Name        string `json:"name"`        // 不带 '/'。plugin 来的是 "<plugin>:<名字>"
	Description string `json:"description"` // 菜单展示用
	// ArgumentHint 是 plugin 命令 frontmatter 里的 argument-hint，内置的没有
	ArgumentHint string `json:"argument_hint"`
	// Kind 是 "builtin"（core 的一个动作）或 "prompt"（plugin 带来的一段文字）
	Kind string `json:"kind"`
}

// New 创建客户端。addr 形如 "127.0.0.1:12345"。
func New(addr string) *Client {
	rt := &http3.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
	}
	var b [8]byte
	_, _ = rand.Read(b[:])
	return &Client{
		hc:       &http.Client{Transport: rt, Timeout: 120 * time.Second},
		rt:       rt,
		url:      fmt.Sprintf("https://%s", addr),
		clientID: hex.EncodeToString(b[:]),
	}
}

// ClientID 是这个进程拥有的那一组
func (c *Client) ClientID() string { return c.clientID }

// CreateAgent 建一个 agent（POST /agent）并记住它的 id，之后的请求都指向它。
//
// workdir 由客户端给：core 是全机单实例，自己的 cwd 是"启动它那个 shell 当时在哪"，
// 跟任何 agent 都无关（ADR-0019）。**这是客户端替用户填的默认值，不是 core 的默认值**——
// 客户端知道用户站在哪，core 不知道。
func (c *Client) CreateAgent(workdir string) error {
	r, err := c.postJSON("/agent", map[string]any{"workdir": workdir})
	if err != nil {
		return err
	}
	if r.AgentID <= 0 {
		return fmt.Errorf("建 agent 失败: %s", r.Error)
	}
	c.agentID = r.AgentID
	return nil
}

// AgentID 是当前连着的那个 agent
func (c *Client) AgentID() int { return c.agentID }

// Attach 改连到本组的另一个 agent。
func (c *Client) Attach(agentID int) { c.agentID = agentID }

// Frame 是一条回放帧（GET /session 或 GET /history）。**与推送流的帧同形**，所以客户端拿同一个
// 渲染器吃它——一份代码，实时看和翻历史看长得一样（ADR-0020）。
type Frame struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data"`
}

// FetchSession 取一个 agent 当前会话的内容，回放成事件帧（GET /session）。
//
// 读的是盘上那份，接缝在「最后一条已落盘的消息」：视图 = 这段回放 + 推送流
// 喂进来的活尾巴（ADR-0020）。每个 agent 都有会话可读，subagent 也不例外——
// 它的会话落在 sessions/sub/，只是不进会话清单。
func (c *Client) FetchSession(agentID int) ([]Frame, error) {
	var f []Frame
	body := map[string]any{"agent_id": agentID}
	if err := c.getJSON("/session", &f, body); err != nil {
		return nil, err
	}
	return f, nil
}

// FetchHistory 是 FetchSession 的兼容别名。
func (c *Client) FetchHistory(agentID int) ([]Frame, error) {
	return c.FetchSession(agentID)
}

// SendTo 往指定 agent 发。
func (c *Client) SendTo(agentID int, message string) (Reply, error) {
	return c.postJSON("/message", map[string]any{
		"agent_id": agentID, "message": message})
}

// Send 提交用户消息。core 立即返回 {"status":"processing"}，完整回复经 /events 推送流送达。
func (c *Client) Send(message string) (Reply, error) {
	body, _ := json.Marshal(map[string]any{"agent_id": c.agentID, "message": message})
	resp, err := c.hc.Post(c.url+"/message", "application/json", bytes.NewReader(body))
	if err != nil {
		return Reply{}, fmt.Errorf("发送失败: %w", err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return Reply{}, fmt.Errorf("读取响应失败: %w", err)
	}
	var r Reply
	if err := json.Unmarshal(data, &r); err != nil {
		return Reply{}, fmt.Errorf("解析响应失败: %s", string(data))
	}
	return r, nil
}

// Interrupt 请求中断当前 agent run（POST /interrupt）
func (c *Client) Interrupt() error {
	body, _ := json.Marshal(map[string]any{"agent_id": c.agentID})
	resp, err := c.hc.Post(c.url+"/interrupt", "application/json", bytes.NewReader(body))
	if err != nil {
		return fmt.Errorf("中断请求失败: %w", err)
	}
	defer resp.Body.Close()
	io.ReadAll(resp.Body)
	return nil
}

// RespondApproval 回传审批裁决（POST /approval-response，PROTOCOL.md）
func (c *Client) RespondApproval(id string, allow bool) error {
	body, _ := json.Marshal(map[string]any{"id": id, "allow": allow})
	resp, err := c.hc.Post(c.url+"/approval-response", "application/json", bytes.NewReader(body))
	if err != nil {
		return fmt.Errorf("回传审批失败: %w", err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return fmt.Errorf("读取审批响应失败: %w", err)
	}
	var r Reply
	if err := json.Unmarshal(data, &r); err != nil {
		return fmt.Errorf("解析审批响应失败: %s", string(data))
	}
	if r.Error != "" {
		return fmt.Errorf("审批回传被拒: %s", r.Error)
	}
	return nil
}

// FetchCommands 拉取可用斜杠命令列表（GET /commands）。失败返回错误（TUI 降级为无菜单）。
//
// 带上 agentID：plugin 带来的 prompt 命令跟着那个 agent 的工作目录走（ADR-0024），
// 不带就只有内置那三条。
func (c *Client) FetchCommands(agentID int) ([]Command, error) {
	var cmds []Command
	if err := c.getJSON("/commands", &cmds, map[string]any{"agent_id": agentID}); err != nil {
		return nil, err
	}
	return cmds, nil
}

// getJSON 发起 GET 并把响应体 JSON 解码到 v（传指针）。失败返回错误。
// getJSON 发起 GET。body 非空时随请求带上——core 侧统一按 JSON 体读参数，
// 不为几个 GET 再养一套 query string 解析（`/events` 是唯一例外：
// 那一处身份必须让传输层看见，它是连接死活的判据）。
func (c *Client) getJSON(path string, v any, body ...any) error {
	var rd io.Reader
	if len(body) > 0 {
		data, _ := json.Marshal(body[0])
		rd = bytes.NewReader(data)
	}
	req, err := http.NewRequest(http.MethodGet, c.url+path, rd)
	if err != nil {
		return fmt.Errorf("构造请求失败: %w", err)
	}
	resp, err := c.hc.Do(req)
	if err != nil {
		return fmt.Errorf("请求失败: %w", err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return fmt.Errorf("读取响应失败: %w", err)
	}
	if err := json.Unmarshal(data, v); err != nil {
		return fmt.Errorf("解析响应失败: %s", string(data))
	}
	return nil
}

// postJSON 发起 POST（JSON 体），返回解析后的 Reply（ok/error 由调用方判断）。
func (c *Client) postJSON(path string, body any) (Reply, error) {
	data, _ := json.Marshal(body)
	resp, err := c.hc.Post(c.url+path, "application/json", bytes.NewReader(data))
	if err != nil {
		return Reply{}, fmt.Errorf("请求失败: %w", err)
	}
	defer resp.Body.Close()
	respBody, err := io.ReadAll(resp.Body)
	if err != nil {
		return Reply{}, fmt.Errorf("读取响应失败: %w", err)
	}
	var r Reply
	if err := json.Unmarshal(respBody, &r); err != nil {
		return Reply{}, fmt.Errorf("解析响应失败: %s", string(respBody))
	}
	return r, nil
}

// ModelInfo 是一条模型记录（/model 命令的 data 载荷）。
// 无单价——单价留在 core 的模型数据表里，客户端看不到（ADR-0009）。
type ModelInfo struct {
	Name    string `json:"name"`
	OwnedBy string `json:"owned_by"`
	Context int64  `json:"context"`
	Current bool   `json:"current"`
}

// SessionInfo 是一条会话记录（/new /resume 与 GET /sessions 的 data 载荷）。
// Title 是第一条 user 消息的正文（core 现取，不另存）；Mtime 是最后写入的 Unix 秒。
//
// OpenedBy 是打开着这个会话的 agent（没人打开就是空串，core 那头是 null）。
// **它取代了从前的 current**：多 agent 之后「当前」没有主语了，同一个目录下可以有
// N 个 agent 各自打开着一个会话（ADR-0019 §10）。要问「是不是我这个」，
// 拿它跟自己的 agent id 比一下就是了。
type SessionInfo struct {
	ID       string `json:"id"`
	Title    string `json:"title"`
	Messages int64  `json:"messages"`
	Mtime    int64  `json:"mtime"`
	OpenedBy int    `json:"opened_by"`
}

// AgentInfo 是一条 agent 记录（GET /agents）。
type AgentInfo struct {
	ID        int   `json:"id"`
	Workdir   string `json:"workdir"`
	State     string `json:"state"` // running | idle
	SessionID string `json:"session_id"`
	InEdges   []int  `json:"in_edges"`
	OutEdges  []int  `json:"out_edges"`
}

// FetchAgents 拉取 agent 清单（GET /agents）。
func (c *Client) FetchAgents() ([]AgentInfo, error) {
	var a []AgentInfo
	if err := c.getJSON("/agents", &a); err != nil {
		return nil, err
	}
	return a, nil
}

// Statusline 是状态栏数据（GET /statusline）：会话身份信息。
// OwnedBy/Context 来自 core 的模型数据表，配了表外的模型时为空——
// 那不是错误，模型表是参考资料不是白名单（ADR-0009）。
type Statusline struct {
	Model   string `json:"model"`
	OwnedBy string `json:"owned_by"`
	Context int64  `json:"context"`
}

// FetchStatusline 拉取状态栏数据（GET /statusline）。失败返回错误（TUI 降级为隐藏该段）。
func (c *Client) FetchStatusline() (Statusline, error) {
	var s Statusline
	if err := c.getJSON("/statusline", &s); err != nil {
		return Statusline{}, err
	}
	return s, nil
}

// CloseGroup 关掉这个客户端拥有的那一组 agent（POST /group/close）。
// 正常退出前发一次；断线满 60 秒 core 自己也会关，那是兜底不是主路（ADR-0021）。
func (c *Client) CloseGroup() error {
	_, err := c.postJSON("/group/close", map[string]string{"client_id": c.clientID})
	return err
}

// Close 关闭传输层
func (c *Client) Close() {
	if c.rt != nil {
		c.rt.Close()
	}
}

// Event 是推送流中的一条事件（PROTOCOL.md 帧）
type Event struct {
	Type    string // message_start / message_update / tool_execution_* / turn_end / permission_request ...
	Payload string // JSON 载荷
}

// SubscribeEvents 订阅 /events 推送流，把事件持续发送到 ch。
// 流断开或出错时关闭 ch 返回。阻塞调用（goroutine 中使用）。
func (c *Client) SubscribeEvents(ch chan<- Event) error {
	// 推送流带上身份：core 靠**这条流所在的连接**判断客户端死活（ADR-0021）——
	// 一个客户端一条、长生命周期，它死就是客户端死。不自建心跳。
	resp, err := c.hc.Get(c.url + "/events?client_id=" + c.clientID)
	if err != nil {
		close(ch)
		return fmt.Errorf("订阅事件流失败: %w", err)
	}
	defer resp.Body.Close()
	defer close(ch)

	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 65536), 65536)
	var evType string
	for sc.Scan() {
		line := sc.Text()
		if strings.HasPrefix(line, "event: ") {
			evType = strings.TrimPrefix(line, "event: ")
		} else if strings.HasPrefix(line, "data: ") {
			payload := strings.TrimPrefix(line, "data: ")
			ch <- Event{Type: evType, Payload: payload}
			evType = ""
		}
	}
	return sc.Err()
}
