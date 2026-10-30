package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/modelcontextprotocol/go-sdk/jsonrpc"
	"github.com/modelcontextprotocol/go-sdk/mcp"

	"realagent/tui/internal/client"
)

// fakeCore 是 core 的替身：记下 POST 的体，/sessions 对所有对话回同一个 state，/events 推测试给的帧
type fakeCore struct {
	mu     sync.Mutex
	state  string
	frames string
	posts  map[string]map[string]any // 路径 → 最后一次的体
	ws     *websocket.Conn
	pushed chan string // 调用方那头收到的 channel 通知
}

func (f *fakeCore) set(state, frames string) {
	f.mu.Lock()
	f.state, f.frames = state, frames
	f.mu.Unlock()
}

func (f *fakeCore) post(path string) map[string]any {
	for i := 0; i < 200; i++ {
		f.mu.Lock()
		p := f.posts[path]
		f.mu.Unlock()
		if p != nil {
			return p
		}
		time.Sleep(10 * time.Millisecond)
	}
	return nil
}

func (f *fakeCore) push(t *testing.T, typ, root, sid, rest string) {
	t.Helper()
	for i := 0; i < 200 && f.conn() == nil; i++ {
		time.Sleep(10 * time.Millisecond)
	}
	data := `{"root":"` + root + `","session_id":"` + sid + `"` + rest + `}`
	if err := f.conn().WriteJSON(map[string]any{"event": typ, "data": json.RawMessage(data)}); err != nil {
		t.Fatal(err)
	}
}

func (f *fakeCore) conn() *websocket.Conn {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.ws
}

func setup(t *testing.T) (*fakeCore, *mcp.ClientSession) {
	f := &fakeCore{state: "running", frames: "[]", posts: map[string]map[string]any{}, pushed: make(chan string, 8)}
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		var body map[string]any
		_ = json.NewDecoder(r.Body).Decode(&body)
		f.mu.Lock()
		defer f.mu.Unlock()
		switch r.URL.Path {
		case "/sessions":
			fmt.Fprintf(w, `[{"id":%q,"state":%q}]`, body["session_id"], f.state)
		case "/session":
			fmt.Fprint(w, f.frames)
		default:
			f.posts[r.URL.Path] = body
			fmt.Fprint(w, `{"status":"processing"}`)
		}
	})
	mux.HandleFunc("/events", func(w http.ResponseWriter, r *http.Request) {
		ws, _ := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		f.mu.Lock()
		f.ws = ws
		f.mu.Unlock()
		select {}
	})
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)

	b := newBridge(client.New(strings.TrimPrefix(srv.URL, "http://"), ""))
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	go b.pump(ctx)
	st, ct := mcp.NewInMemoryTransports()
	if _, err := b.server().Connect(ctx, tap{st, b}, nil); err != nil {
		t.Fatal(err)
	}
	cs, err := mcp.NewClient(&mcp.Implementation{Name: "test"}, nil).Connect(ctx, channelTap{ct, f.pushed}, nil)
	if err != nil {
		t.Fatal(err)
	}
	return f, cs
}

// channelTap 站在调用方那头，把 channel 通知截下来，别的原样交给 SDK 客户端
type channelTap struct {
	mcp.Transport
	got chan<- string
}

func (t channelTap) Connect(ctx context.Context) (mcp.Connection, error) {
	conn, err := t.Transport.Connect(ctx)
	return tapConn{conn, t.got}, err
}

type tapConn struct {
	mcp.Connection
	got chan<- string
}

func (c tapConn) Read(ctx context.Context) (jsonrpc.Message, error) {
	for {
		m, err := c.Connection.Read(ctx)
		if r, ok := m.(*jsonrpc.Request); ok && r.Method == "notifications/claude/channel" {
			c.got <- string(r.Params)
			continue
		}
		return m, err
	}
}

func call[T any](t *testing.T, cs *mcp.ClientSession, tool string, args map[string]any) T {
	t.Helper()
	res, err := cs.CallTool(context.Background(), &mcp.CallToolParams{Name: tool, Arguments: args})
	if err != nil {
		t.Fatal(err)
	}
	var v T
	data, _ := json.Marshal(res.StructuredContent)
	_ = json.Unmarshal(data, &v)
	return v
}

// workdir 是各平台都算绝对路径的那种（Windows 上 /tmp/w 不是）
var workdir, _ = filepath.Abs("w")

func send(t *testing.T, cs *mcp.ClientSession) string {
	return call[sendOut](t, cs, "send", map[string]any{"workdir": workdir, "prompt": "干活"}).SessionID
}

func waitFor(t *testing.T, cs *mcp.ClientSession, id string, timeout float64) string {
	got := call[waitOut](t, cs, "wait", map[string]any{"session_ids": []string{id}, "timeout_s": timeout})
	return fmt.Sprint(got.Results)
}

func TestSendPostsMessage(t *testing.T) {
	f, cs := setup(t)
	id := send(t, cs)
	got := f.post("/message")
	if got["session_id"] != id || got["workdir"] != workdir || got["message"] != "干活" || len(id) != 16 {
		t.Fatalf("got %v id=%q", got, id)
	}
}

// 子 agent 收工时 core 仍说 running：不返回；core 说停了才返回最后一个 turn 的正文
func TestWaitReturnsWhenConversationStops(t *testing.T) {
	f, cs := setup(t)
	id := send(t, cs)
	done := make(chan string, 1)
	go func() { done <- waitFor(t, cs, id, 10) }()
	f.push(t, "agent_end", id, "sub", "")
	select {
	case got := <-done:
		t.Fatalf("对话还在跑就返回了：%s", got)
	case <-time.After(200 * time.Millisecond):
	}
	f.set("", `[{"type":"turn_start","data":{}},{"type":"message_update","data":{"delta":"先查"}},
		{"type":"turn_start","data":{}},{"type":"message_update","data":{"delta":"改好了"}}]`)
	f.push(t, "agent_end", id, id, "")
	if got, want := <-done, fmt.Sprint([]item{{id, "done", "改好了"}}); got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
}

func TestWaitTimeoutReportsRunning(t *testing.T) {
	_, cs := setup(t)
	id := send(t, cs)
	if got, want := waitFor(t, cs, id, 0), fmt.Sprint([]item{{id, "running", ""}}); got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
}

// 端点报错只在帧里、不在盘上：原文交给调用方，它才能判断该重试还是换路
func TestEndpointErrorBecomesText(t *testing.T) {
	f, cs := setup(t)
	id := send(t, cs)
	done := make(chan string, 1)
	go func() { done <- waitFor(t, cs, id, 5) }()
	time.Sleep(100 * time.Millisecond) // wait 先问到 running，之后只被 agent_end 叫醒；帧按序处理，那时报错已记下
	f.push(t, "turn_end", id, id, `,"error":"端点返回 HTTP 502"`)
	f.set("", "[]")
	f.push(t, "agent_end", id, id, "")
	if got, want := <-done, fmt.Sprint([]item{{id, "done", "端点返回 HTTP 502"}}); got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
}

func TestPermissionRequestIsApproved(t *testing.T) {
	f, cs := setup(t)
	id := send(t, cs)
	f.push(t, "permission_request", id, id, `,"id":"p1"`)
	if got := fmt.Sprint(f.post("/approval-response")); got != fmt.Sprint(map[string]any{"id": "p1", "allow": true}) {
		t.Fatalf("got %s", got)
	}
}

func TestStopPushesChannelEvent(t *testing.T) {
	f, cs := setup(t)
	id := send(t, cs)
	f.set("", `[{"type":"turn_start","data":{}},{"type":"message_update","data":{"delta":"改好了"}}]`)
	f.push(t, "agent_end", id, id, "")
	select {
	case got := <-f.pushed:
		if want := `{"content":"改好了","meta":{"session_id":"` + id + `"}}`; got != want {
			t.Fatalf("got %s, want %s", got, want)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("停下后没有推 channel 通知")
	}
}
