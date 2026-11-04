// Package client 封装 core 的端点：请求走 HTTP，推送走 WebSocket（PROTOCOL.md）
package client

import (
	"bytes"
	"cmp"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"

	"github.com/gorilla/websocket"
)

// Client 是 core 的客户端
type Client struct {
	hc   *http.Client
	addr string

	clientID  string // 本进程一个，不落盘
	workdir   string // 用户站在哪；core 不猜（ADR-0019）
	sessionID string // 当前那段对话。新对话的 id 由这里生成，第一条消息到了 core 才开出它（ADR-0029）
}

// Reply 是请求-响应端点的通用响应
type Reply struct {
	Status  string          `json:"status"`
	Error   string          `json:"error,omitempty"`
	Ok      bool            `json:"ok,omitempty"`
	Command string          `json:"command,omitempty"`
	Data    json.RawMessage `json:"data,omitempty"` // 斜杠命令结果载荷
}

// Command 是一条斜杠命令（GET /commands）
type Command struct {
	Name         string `json:"name"` // 不带 '/'。plugin 来的是 "<plugin>:<名字>"
	Description  string `json:"description"`
	ArgumentHint string `json:"argument_hint"`
	Kind         string `json:"kind"` // "builtin" | "prompt"
}

// New 创建客户端。addr 形如 "127.0.0.1:12345"，workdir 是对话在哪个目录里开。
func New(addr, workdir string) *Client {
	return &Client{
		hc:        &http.Client{Timeout: 120 * time.Second},
		addr:      addr,
		clientID:  randomID(),
		workdir:   workdir,
		sessionID: randomID(),
	}
}

func randomID() string {
	var b [8]byte
	_, _ = rand.Read(b[:])
	return hex.EncodeToString(b[:])
}

func (c *Client) SessionID() string { return c.sessionID }

// With 返回绑到另一段对话的副本，共享连接与 clientID：一个进程多段对话时用。
// sessionID 为空则开新对话。
func (c *Client) With(workdir, sessionID string) *Client {
	cp := *c
	cp.workdir = workdir
	cp.sessionID = sessionID
	if sessionID == "" {
		cp.sessionID = randomID()
	}
	return &cp
}

// Use 换到一段已有的对话。
func (c *Client) Use(sessionID string) { c.sessionID = sessionID }

// Fresh 换到一段新对话。
func (c *Client) Fresh() { c.sessionID = randomID() }

// conv 补上「我是谁、在哪、说的是哪段」：动对话的端点都要这三样
func (c *Client) conv(body map[string]any) map[string]any {
	if body == nil {
		body = map[string]any{}
	}
	body["client_id"] = c.clientID
	body["workdir"] = c.workdir
	body["session_id"] = c.sessionID
	return body
}

// Frame 是一帧事件。推送流与 GET /session 回放同形（ADR-0020），载荷在这里解一次。
// 字段是 PROTOCOL.md 各帧载荷里客户端用得上的那些，帧没带的为零值。
type Frame struct {
	Event string `json:"-"` // 帧名

	Root      string `json:"root"`       // 哪段对话
	SessionID string `json:"session_id"` // 谁说的：不等于 Root 就是它派生出去的子 agent

	Text  string `json:"text"`  // message_start 的正文；tool_output 的一段输出；compact 的摘要
	Delta string `json:"delta"` // message_update / thinking_update 的增量

	Name   string `json:"name"`    // 工具名
	ID     string `json:"id"`      // 工具调用 id；permission_request 的审批 id
	CallID string `json:"call_id"` // tool_output 挂在哪次工具调用下面：就是 tool_execution_start 的 id
	Status int    `json:"status"`  // tool_execution_end：非零 = 失败

	Tool   string          `json:"tool"`   // permission_request 要用的工具
	Params json.RawMessage `json:"params"` // permission_request 的工具参数，原样展示

	Error  string  `json:"error"`  // turn_end：core 报的失败
	Cost   float64 `json:"cost"`   // status_update / agent_end：本次跑的累计花费，算不出钱时不带
	Model  string  `json:"model"`  // statusline
	Effort string  `json:"effort"` // statusline：主模型的思考量级，没配为空

	Context int64 `json:"context"` // statusline：模型的上下文窗口，模型表里查不到时为 0
	Live          // status_update 里喂状态栏的那几个键
}

// Live 是 status_update 帧里喂状态栏的键。指针：nil = 这一帧没带
type Live struct {
	ContextTokens *int64   `json:"context_tokens"` // 主模型上一次请求的提示词 token 数
	CacheHit      *float64 `json:"cache_hit"`      // 其中从缓存读的比例
	TTFTMs        *int64   `json:"ttft_ms"`        // 首 token 时延
	TPS           *float64 `json:"tps"`            // 出字速度
}

// UnmarshalJSON 解线上的一帧：推送流用 event 作键，回放用 type，载荷都在 data 里。
// 载荷是开放键集（PROTOCOL.md status_update）：类型对不上的键留零值，其余照收
func (f *Frame) UnmarshalJSON(b []byte) error {
	type data Frame // 不带方法，免得解 data 时绕回这里
	w := struct {
		Event, Type string
		Data        *data
	}{Data: (*data)(f)}
	_ = json.Unmarshal(b, &w)
	f.Event = cmp.Or(w.Event, w.Type)
	return nil
}

// FetchSession 取一段对话，回放成事件帧（GET /session）。读的是盘上那份。
func (c *Client) FetchSession(sessionID string) ([]Frame, error) {
	var f []Frame
	err := c.do(http.MethodGet, "/session", map[string]any{"workdir": c.workdir, "session_id": sessionID}, &f)
	return f, err
}

// Send 往当前对话发。core 立即返回 {"status":"processing"}，回复走推送流。
func (c *Client) Send(message string) (Reply, error) {
	return c.postJSON("/message", c.conv(map[string]any{"message": message}))
}

// Interrupt 停下当前对话，连同它派生出去的（POST /interrupt）
func (c *Client) Interrupt() error {
	_, err := c.postJSON("/interrupt", c.conv(nil))
	return err
}

// RespondApproval 回传审批裁决（POST /approval-response）
func (c *Client) RespondApproval(id string, allow bool) error {
	r, err := c.postJSON("/approval-response", map[string]any{"id": id, "allow": allow})
	if err == nil && r.Error != "" {
		err = fmt.Errorf("审批回传被拒: %s", r.Error)
	}
	return err
}

// FetchCommands 拉取斜杠命令列表：plugin 命令跟着工作目录走。
func (c *Client) FetchCommands() ([]Command, error) {
	var cmds []Command
	err := c.do(http.MethodGet, "/commands", map[string]any{"workdir": c.workdir}, &cmds)
	return cmds, err
}

// FetchSessions 拉这个目录下的对话清单（GET /sessions），最近的在前。
func (c *Client) FetchSessions() ([]SessionInfo, error) {
	var l []SessionInfo
	err := c.do(http.MethodGet, "/sessions", c.conv(nil), &l)
	return l, err
}

// do 发一个请求，把 JSON 响应解到 out。core 的 GET 也从 JSON 体读参数。
func (c *Client) do(method, path string, body, out any) error {
	var rd io.Reader
	if body != nil {
		data, _ := json.Marshal(body)
		rd = bytes.NewReader(data)
	}
	req, err := http.NewRequest(method, "http://"+c.addr+path, rd)
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
	if err := json.Unmarshal(data, out); err != nil {
		return fmt.Errorf("解析响应失败: %s", string(data))
	}
	return nil
}

func (c *Client) postJSON(path string, body any) (Reply, error) {
	var r Reply
	err := c.do(http.MethodPost, path, body, &r)
	return r, err
}

// ModelInfo 是 /model 的一条（不含单价）
type ModelInfo struct {
	Name    string `json:"name"`
	OwnedBy string `json:"owned_by"`
	Context int64  `json:"context"`
	Current bool   `json:"current"`
}

// SessionInfo 是对话清单的一条。State：running = 正在跑，elsewhere = 在别的窗口里开着，空 = 都不是。
type SessionInfo struct {
	ID       string `json:"id"`
	Title    string `json:"title"`
	Messages int64  `json:"messages"`
	Mtime    int64  `json:"mtime"`
	State    string `json:"state"`
}

// Statusline 是 GET /statusline。配了模型表外的模型时 OwnedBy / Context 为空。
type Statusline struct {
	Model   string `json:"model"`
	Effort  string `json:"effort"`
	OwnedBy string `json:"owned_by"`
	Context int64  `json:"context"`
}

func (c *Client) FetchStatusline() (Statusline, error) {
	var s Statusline
	err := c.do(http.MethodGet, "/statusline", nil, &s)
	return s, err
}

// Setup 是首启引导的那几项：GET /setup 回整棵配置树，取这几个；POST /setup 原样写回。
type Setup struct {
	Done          bool   `json:"setup_done,omitempty"`
	Protocol      string `json:"protocol"`
	BaseURL       string `json:"base_url"`
	APIKey        string `json:"api_key"`
	Model         string `json:"model"`
	SmallModel    string `json:"small_model"`
	DecisionModel string `json:"decision_model"`
}

func (c *Client) FetchSetup() (Setup, error) {
	var s Setup
	err := c.do(http.MethodGet, "/setup", nil, &s)
	return s, err
}

// ApplySetup 落盘（POST /setup）。core 校验不过回 error。
func (c *Client) ApplySetup(s Setup) error {
	r, err := c.postJSON("/setup", s)
	if err == nil && r.Error != "" {
		err = fmt.Errorf("%s", r.Error)
	}
	return err
}

// FetchModels 按 s 里的 protocol / base_url / api_key 拉端点的模型清单（POST /setup/models）。
func (c *Client) FetchModels(s Setup) ([]string, error) {
	r, err := c.postJSON("/setup/models", s)
	if err != nil {
		return nil, err
	}
	if r.Error != "" {
		return nil, fmt.Errorf("%s", r.Error)
	}
	var list []string
	err = json.Unmarshal(r.Data, &list)
	return list, err
}

// CloseGroup 正常退出前通知 core（POST /group/close）
func (c *Client) CloseGroup() error {
	_, err := c.postJSON("/group/close", map[string]string{"client_id": c.clientID})
	return err
}

// SubscribeEvents 连上 /events，帧持续写进 ch；连接断开时关闭 ch 返回。阻塞调用。
func (c *Client) SubscribeEvents(ch chan<- Frame) error {
	defer close(ch)
	ws, _, err := websocket.DefaultDialer.Dial("ws://"+c.addr+"/events?client_id="+c.clientID, nil)
	if err != nil {
		return fmt.Errorf("订阅事件流失败: %w", err)
	}
	defer ws.Close()
	for {
		var f Frame
		if err := ws.ReadJSON(&f); err != nil {
			return err
		}
		ch <- f
	}
}
