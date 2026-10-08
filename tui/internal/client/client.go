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

	agentID  int    // 当前在跟哪个 agent 说话；动 agent 的端点都要指名
	clientID string // 本进程一个，不落盘
}

// Reply 是请求-响应端点的通用响应
type Reply struct {
	Status  string          `json:"status"`
	Error   string          `json:"error,omitempty"`
	Ok      bool            `json:"ok,omitempty"`
	Command string          `json:"command,omitempty"`
	Data    json.RawMessage `json:"data,omitempty"` // 斜杠命令结果载荷
	AgentID int             `json:"agent_id,omitempty"`
}

// Command 是一条斜杠命令（GET /commands）
type Command struct {
	Name         string `json:"name"` // 不带 '/'。plugin 来的是 "<plugin>:<名字>"
	Description  string `json:"description"`
	ArgumentHint string `json:"argument_hint"`
	Kind         string `json:"kind"` // "builtin" | "prompt"
}

// New 创建客户端。addr 形如 "127.0.0.1:12345"。
func New(addr string) *Client {
	var b [8]byte
	_, _ = rand.Read(b[:])
	return &Client{
		hc:       &http.Client{Timeout: 120 * time.Second},
		addr:     addr,
		clientID: hex.EncodeToString(b[:]),
	}
}

// CreateAgent 建一个 agent 并记住它。workdir 由客户端给：它知道用户站在哪，core 不知道。
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

func (c *Client) AgentID() int { return c.agentID }

// Attach 改连到另一个 agent。
func (c *Client) Attach(agentID int) { c.agentID = agentID }

// Frame 是一条回放帧，与推送流的帧同形（ADR-0020）
type Frame struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data"`
}

// FetchSession 取一个 agent 当前会话，回放成事件帧（GET /session）。
func (c *Client) FetchSession(agentID int) ([]Frame, error) {
	var f []Frame
	err := c.getJSON("/session", &f, map[string]any{"agent_id": agentID})
	return f, err
}

// SendTo 往指定 agent 发一条消息。
func (c *Client) SendTo(agentID int, message string) (Reply, error) {
	return c.postJSON("/message", map[string]any{"agent_id": agentID, "message": message})
}

// Send 往当前 agent 发。core 立即返回 {"status":"processing"}，回复走推送流。
func (c *Client) Send(message string) (Reply, error) { return c.SendTo(c.agentID, message) }

// Interrupt 中断当前 agent（POST /interrupt）
func (c *Client) Interrupt() error {
	_, err := c.postJSON("/interrupt", map[string]any{"agent_id": c.agentID})
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

// FetchCommands 拉取斜杠命令列表。带 agentID：plugin 命令跟着那个 agent 的工作目录走。
func (c *Client) FetchCommands(agentID int) ([]Command, error) {
	var cmds []Command
	err := c.getJSON("/commands", &cmds, map[string]any{"agent_id": agentID})
	return cmds, err
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

// SessionInfo 是会话清单的一条。OpenedBy 是打开着它的 agent（没人打开为 0）。
type SessionInfo struct {
	ID       string `json:"id"`
	Title    string `json:"title"`
	Messages int64  `json:"messages"`
	Mtime    int64  `json:"mtime"`
	OpenedBy int    `json:"opened_by"`
}

// AgentInfo 是 GET /agents 的一条
type AgentInfo struct {
	ID        int    `json:"id"`
	Workdir   string `json:"workdir"`
	State     string `json:"state"` // running | idle
	SessionID string `json:"session_id"`
	InEdges   []int  `json:"in_edges"`
	OutEdges  []int  `json:"out_edges"`
}

func (c *Client) FetchAgents() ([]AgentInfo, error) {
	var a []AgentInfo
	err := c.getJSON("/agents", &a)
	return a, err
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
