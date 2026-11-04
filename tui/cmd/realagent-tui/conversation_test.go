// 对话的帧路由（ADR-0029）：每帧带 root 与 session_id，客户端只认对话，不认 agent。
package main

import (
	"reflect"
	"testing"

	"realagent/tui/internal/client"
)

func at(root, sid string, f client.Frame) client.Frame {
	f.Root, f.SessionID = root, sid
	return f
}

// 当前对话的画，别的对话的不画；子 agent 只留动静一行一条，正文不铺开
func TestFrameRouting(t *testing.T) {
	m := testModel()
	m.client.Use("s-1")
	m.handleEvent(at("s-1", "s-1", client.Frame{Event: "message_start", Text: "主线"}))
	m.handleEvent(at("s-2", "s-2", client.Frame{Event: "message_start", Text: "隔壁"}))
	m.handleEvent(at("s-1", "s-sub", client.Frame{Event: "message_start", Text: "去查日志\n细节"}))
	m.handleEvent(at("s-1", "s-sub", client.Frame{Event: "message_update", Delta: "子 agent 的正文"}))
	m.handleEvent(at("s-1", "s-sub", client.Frame{Event: "tool_execution_start", Name: "bash", ID: "c1"}))
	m.handleEvent(at("s-1", "s-sub", client.Frame{Event: "agent_end"}))
	want := []string{"主线", "  ↳ 子任务：去查日志", "  ↳   🔧 bash", "  ↳ 子任务收工"}
	if got := texts(m); !reflect.DeepEqual(got, want) {
		t.Errorf("条目 = %q, want %q", got, want)
	}
}

// 压缩：读秒说在压，压完摘要成一条；状态栏上压缩前的上下文大小作废
func TestCompactFrames(t *testing.T) {
	m := testModel()
	m.client.Use("s-1")
	tokens := int64(900)
	m.sl.live.ContextTokens = &tokens
	m.handleEvent(at("s-1", "s-1", client.Frame{Event: "compact_start"}))
	if m.busy.verb != "压缩上下文" {
		t.Errorf("读秒 = %q", m.busy.verb)
	}
	m.handleEvent(at("s-1", "s-1", client.Frame{Event: "compact", Text: "摘要正文"}))
	if got := dump(m); !reflect.DeepEqual(got, []string{"compact:摘要正文"}) {
		t.Errorf("条目 = %q", got)
	}
	if m.sl.live.ContextTokens != nil {
		t.Errorf("上下文大小 = %d，应等下一次请求再报", *m.sl.live.ContextTokens)
	}
}

// 子 agent 收工不是这段对话收工：读秒照走
func TestSubAgentEndKeepsBusy(t *testing.T) {
	m := testModel()
	m.client.Use("s-1")
	m.awaiting = true
	m.handleEvent(at("s-1", "s-sub", client.Frame{Event: "agent_end"}))
	if !m.awaiting {
		t.Error("子 agent 收工替整段对话收了工")
	}
}

// 审批不管来自哪段对话都要弹，并说明是谁在问
func TestApprovalFrom(t *testing.T) {
	m := testModel()
	m.client.Use("s-1")
	for _, c := range []struct{ root, sid, want string }{
		{"s-2", "s-2", "另一段对话"},
		{"s-1", "s-sub", "子任务"},
		{"s-1", "s-1", ""},
	} {
		m.handleEvent(at(c.root, c.sid, client.Frame{Event: "permission_request", ID: "a", Tool: "bash"}))
		if m.approval == nil || m.approval.from != c.want {
			t.Errorf("%s/%s: approval = %+v, want from %q", c.root, c.sid, m.approval, c.want)
		}
	}
}
