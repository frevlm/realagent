// realagent-mcp：让别的 agent 通过 MCP 把活儿派给 realagent（ADR-0031）。
// 它是 core 的第三个客户端：一个 clientID、一条 WebSocket。停没停、说了什么只问 core。
package main

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"syscall"
	"time"

	"github.com/modelcontextprotocol/go-sdk/jsonrpc"
	"github.com/modelcontextprotocol/go-sdk/mcp"

	"realagent/tui/internal/client"
)

const instructions = "realagent 是一个可委派活儿的 agent。send 开始一件活（同一轮里连发多个就是并行），" +
	"wait 等列出的全部停下再返回（超时不是错，没停的回 running）。给 send 传已有的 session_id 是接着说。" +
	"结果若以 <channel source=\"realagent\" session_id=...> 事件送到，就不必再 wait 它。"

type bridge struct {
	base *client.Client
	mu   sync.Mutex
	dir  map[string]string // 开过的对话 → 它的 workdir
	errs map[string]string // 对话 → 顶层 agent 最近一次端点报错的原文（盘上没有这一份）
	tick chan struct{}     // 每收到一个 agent_end 就关掉换新，叫醒在 wait 的
	conn mcp.Connection    // 推 channel 通知用
}

type sendIn struct {
	Workdir   string `json:"workdir" jsonschema:"工作目录，绝对路径"`
	Prompt    string `json:"prompt" jsonschema:"交给 realagent 的任务"`
	SessionID string `json:"session_id,omitempty" jsonschema:"省略则开新对话；给已有的则接着说"`
}
type sendOut struct {
	SessionID string `json:"session_id"`
}
type waitIn struct {
	SessionIDs []string `json:"session_ids"`
	TimeoutS   *float64 `json:"timeout_s,omitempty" jsonschema:"最多等多少秒，默认 50；0 表示只查状态"`
}
type item struct {
	SessionID string `json:"session_id"`
	State     string `json:"state"` // running | done
	Text      string `json:"text"`
}
type waitOut struct {
	Results []item `json:"results"`
}
type cancelIn struct {
	SessionID string `json:"session_id"`
}

func (b *bridge) send(_ context.Context, _ *mcp.CallToolRequest, in sendIn) (*mcp.CallToolResult, sendOut, error) {
	if !filepath.IsAbs(in.Workdir) {
		return nil, sendOut{}, errors.New("workdir 必须是绝对路径")
	}
	cl := b.base.With(in.Workdir, in.SessionID)
	b.mu.Lock()
	b.dir[cl.SessionID()] = in.Workdir
	delete(b.errs, cl.SessionID())
	b.mu.Unlock()
	r, err := cl.Send(in.Prompt)
	if err == nil && r.Error != "" {
		err = errors.New(r.Error)
	}
	return nil, sendOut{cl.SessionID()}, err
}

func (b *bridge) wait(ctx context.Context, _ *mcp.CallToolRequest, in waitIn) (*mcp.CallToolResult, waitOut, error) {
	d := 50.0
	if in.TimeoutS != nil {
		d = *in.TimeoutS
	}
	timeout := time.After(time.Duration(d * float64(time.Second)))
	for {
		b.mu.Lock()
		tick := b.tick // 先拿信号再问 core：问完到等之间来的 agent_end 不会漏
		b.mu.Unlock()
		out, busy := b.status(in.SessionIDs)
		if !busy {
			return nil, out, nil
		}
		select {
		case <-tick:
		case <-timeout:
			return nil, out, nil
		case <-ctx.Done():
			return nil, waitOut{}, ctx.Err()
		}
	}
}

func (b *bridge) status(ids []string) (out waitOut, busy bool) {
	for _, id := range ids {
		it := b.report(id)
		busy = busy || it.State == "running"
		out.Results = append(out.Results, it)
	}
	return out, busy
}

// report 问 core：还在跑就是 running；停了就取最后一个 turn 的正文，端点报错时取报错原文
func (b *bridge) report(id string) item {
	b.mu.Lock()
	dir, failure := b.dir[id], b.errs[id]
	b.mu.Unlock()
	cl := b.base.With(dir, id)
	l, _ := cl.FetchSessions()
	for _, s := range l {
		if s.ID == id && s.State == "running" {
			return item{id, "running", ""}
		}
	}
	if failure != "" {
		return item{id, "done", failure}
	}
	frames, _ := cl.FetchSession(id)
	return item{id, "done", finalText(frames)}
}

// finalText 取最后一个 turn 的正文（回放里一个 message_update 就是一整块）
func finalText(frames []client.Frame) string {
	text := ""
	for _, f := range frames {
		var d struct {
			Delta string `json:"delta"`
		}
		switch f.Type {
		case "turn_start":
			text = ""
		case "message_update":
			_ = json.Unmarshal(f.Data, &d)
			text += d.Delta
		}
	}
	return text
}

func (b *bridge) cancel(_ context.Context, _ *mcp.CallToolRequest, in cancelIn) (*mcp.CallToolResult, struct{}, error) {
	return nil, struct{}{}, b.base.With("", in.SessionID).Interrupt()
}

func (b *bridge) handle(ev client.Event) {
	var d struct {
		Root      string `json:"root"`
		SessionID string `json:"session_id"`
		ID        string `json:"id"`
		Error     string `json:"error"`
	}
	_ = json.Unmarshal([]byte(ev.Payload), &d)
	b.mu.Lock()
	_, ours := b.dir[d.Root]
	switch {
	case ev.Type == "permission_request": // 先全放行；以后改成转给调用方问
		go b.base.RespondApproval(d.ID, true)
	case ev.Type == "turn_end" && d.Error != "" && d.SessionID == d.Root:
		b.errs[d.Root] = d.Error
	case ev.Type == "agent_end":
		close(b.tick)
		b.tick = make(chan struct{})
	}
	b.mu.Unlock()
	if ev.Type == "agent_end" && ours {
		if it := b.report(d.Root); it.State == "done" {
			b.push(it)
		}
	}
}

// push 推一条 Claude Code channels 通知。SDK 不发自定义方法，直接写连接；不认得的客户端忽略
func (b *bridge) push(it item) {
	b.mu.Lock()
	conn := b.conn
	b.mu.Unlock()
	if conn == nil {
		return
	}
	params, _ := json.Marshal(map[string]any{"content": it.Text, "meta": map[string]string{"session_id": it.SessionID}})
	_ = conn.Write(context.Background(), &jsonrpc.Request{Method: "notifications/claude/channel", Params: params})
}

// Connect 让 bridge 自己当传输层的外壳，记下连上的那条连接
type tap struct {
	mcp.Transport
	b *bridge
}

func (t tap) Connect(ctx context.Context) (mcp.Connection, error) {
	conn, err := t.Transport.Connect(ctx)
	t.b.mu.Lock()
	t.b.conn = conn
	t.b.mu.Unlock()
	return conn, err
}

// pump 把推送流喂给 handle；断了 1 秒后用同一个 clientID 重连
func (b *bridge) pump(ctx context.Context) {
	for ctx.Err() == nil {
		ch := make(chan client.Event, 64)
		go b.base.SubscribeEvents(ch)
		for ev := range ch {
			b.handle(ev)
		}
		time.Sleep(time.Second)
	}
}

func (b *bridge) server() *mcp.Server {
	s := mcp.NewServer(&mcp.Implementation{Name: "realagent", Version: "0.1.0"}, &mcp.ServerOptions{
		Instructions: instructions,
		Capabilities: &mcp.ServerCapabilities{Experimental: map[string]any{"claude/channel": map[string]any{}}},
	})
	mcp.AddTool(s, &mcp.Tool{Name: "send", Description: "把任务交给 realagent，立即返回 session_id。"}, b.send)
	mcp.AddTool(s, &mcp.Tool{Name: "wait", Description: "等这些对话全部停下，或超时；返回各自的状态与最后的回复。"}, b.wait)
	mcp.AddTool(s, &mcp.Tool{Name: "cancel", Description: "停掉一段对话和它派生的全部子任务。"}, b.cancel)
	return s
}

// startCore 连不上 core 就在后台拉起 <本程序目录>/core/realagent-core，最多等 5 秒
func startCore(c *client.Client) {
	if _, err := c.FetchSetup(); err == nil {
		return
	}
	exe, _ := os.Executable()
	home, _ := os.UserHomeDir()
	log, _ := os.OpenFile(filepath.Join(home, ".realagent", "core.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	cmd := exec.Command(filepath.Join(filepath.Dir(exe), "core", "realagent-core"))
	cmd.Stdout, cmd.Stderr = log, log
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if cmd.Start() != nil {
		return
	}
	for i := 0; i < 50; i++ {
		if _, err := c.FetchSetup(); err == nil {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func newBridge(c *client.Client) *bridge {
	return &bridge{base: c, dir: map[string]string{}, errs: map[string]string{}, tick: make(chan struct{})}
}

func main() {
	b := newBridge(client.New("127.0.0.1:12345", ""))
	startCore(b.base)
	ctx := context.Background()
	go b.pump(ctx)
	_ = b.server().Run(ctx, tap{&mcp.StdioTransport{}, b})
	_ = b.base.CloseGroup()
}
