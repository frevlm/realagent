// 对话的帧路由（ADR-0029）：每帧带 root 与 session_id，客户端只认对话，不认 agent。
package main

import (
	"reflect"
	"testing"

	"realagent/tui/internal/client"
)

func frame(typ, root, sid, rest string) client.Event {
	return client.Event{Type: typ, Payload: `{"root":"` + root + `","session_id":"` + sid + `",` + rest + `}`}
}

// 当前对话的画，别的对话的不画；子 agent 只留动静一行一条，正文不铺开
func TestFrameRouting(t *testing.T) {
	m := testModel()
	m.client.Use("s-1")
	m.handleEvent(frame("message_start", "s-1", "s-1", `"text":"主线"`))
	m.handleEvent(frame("message_start", "s-2", "s-2", `"text":"隔壁"`))
	m.handleEvent(frame("message_start", "s-1", "s-sub", `"text":"去查日志\n细节"`))
	m.handleEvent(frame("message_update", "s-1", "s-sub", `"delta":"子 agent 的正文"`))
	m.handleEvent(frame("tool_execution_start", "s-1", "s-sub", `"name":"bash","id":"c1"`))
	m.handleEvent(frame("agent_end", "s-1", "s-sub", `"cost":0`))
	want := []string{"主线", "  ↳ 子任务：去查日志", "  ↳   🔧 bash", "  ↳ 子任务收工"}
	if got := lineTexts(m); !reflect.DeepEqual(got, want) {
		t.Errorf("行流 = %q, want %q", got, want)
	}
}

// 子 agent 收工不是这段对话收工：读秒照走
func TestSubAgentEndKeepsBusy(t *testing.T) {
	m := testModel()
	m.client.Use("s-1")
	m.awaiting = true
	m.handleEvent(frame("agent_end", "s-1", "s-sub", `"cost":0`))
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
		m.handleEvent(frame("permission_request", c.root, c.sid, `"id":"a","tool":"bash"`))
		if m.approval == nil || m.approval.from != c.want {
			t.Errorf("%s/%s: approval = %+v, want from %q", c.root, c.sid, m.approval, c.want)
		}
	}
}
