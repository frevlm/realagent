// Package client 封装 core 的端点：请求走 HTTP，推送走 WebSocket（PROTOCOL.md）
package client

import (
	"bytes"
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

// Frame 是一条回放帧，与推送流的帧同形（ADR-0020）
type Frame struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data"`
}

// FetchSession 取一段对话，回放成事件帧（GET /session）。读的是盘上那份。
func (c *Client) FetchSession(sessionID string) ([]Frame, error) {
	var f []Frame
	err := c.getJSON("/session", &f, map[string]any{"workdir": c.workdir, "session_id": sessionID})
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
	err := c.getJSON("/commands", &cmds, map[string]any{"workdir": c.workdir})
	return cmds, err
}

// FetchSessions 拉这个目录下的对话清单（GET /sessions），最近的在前。
func (c *Client) FetchSessions() ([]SessionInfo, error) {
	var l []SessionInfo
	err := c.getJSON("/sessions", &l, c.conv(nil))
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

func (c *Client) getJSON(path string, out any, body ...any) error {
	var b any
	if len(body) > 0 {
		b = body[0]
	}
	return c.do(http.MethodGet, path, b, out)
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
	OwnedBy string `json:"owned_by"`
	Context int64  `json:"context"`
}

func (c *Client) FetchStatusline() (Statusline, error) {
	var s Statusline
	err := c.getJSON("/statusline", &s)
	return s, err
}

// Setup 是首启引导的那几项：GET /setup 回整棵配置树，取这几个；POST /setup 原样写回。
type Setup struct {
	Done       bool   `json:"setup_done,omitempty"`
	Protocol   string `json:"protocol"`
	BaseURL    string `json:"base_url"`
	APIKey     string `json:"api_key"`
	Model      string `json:"model"`
	SmallModel string `json:"small_model"`
}

func (c *Client) FetchSetup() (Setup, error) {
	var s Setup
	err := c.getJSON("/setup", &s)
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

// Event 是推送流中的一条事件
type Event struct {
	Type    string
	Payload string // JSON
}

// SubscribeEvents 连上 /events，事件持续写进 ch；连接断开时关闭 ch 返回。阻塞调用。
func (c *Client) SubscribeEvents(ch chan<- Event) error {
	defer close(ch)
	ws, _, err := websocket.DefaultDialer.Dial("ws://"+c.addr+"/events?client_id="+c.clientID, nil)
	if err != nil {
		return fmt.Errorf("订阅事件流失败: %w", err)
	}
	defer ws.Close()
	for {
		var f struct {
			Event string          `json:"event"`
			Data  json.RawMessage `json:"data"`
		}
		if err := ws.ReadJSON(&f); err != nil {
			return err
		}
		ch <- Event{Type: f.Event, Payload: string(f.Data)}
	}
}
