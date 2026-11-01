// thinking 事件（DeepSeek v4 reasoning）的流式累积 / 定型 / 渲染。
package main

import (
	"strings"
	"testing"

	"realagent/tui/internal/client"
)

func feedEvents(m *model, evs ...client.Event) {
	for _, ev := range evs {
		m.handleEvent(ev)
	}
}

// roleTexts 返回未提交行的「角色:文本」（断言顺序与归属）
func roleTexts(m model) []string {
	var out []string
	for _, l := range m.lines {
		out = append(out, l.role+":"+l.text)
	}
	return out
}

func TestThinkingStreaming(t *testing.T) {
	m := testModel()
	feedEvents(&m,
		client.Event{Type: "thinking_start", Payload: `{"signature":"sig-1"}`},
		client.Event{Type: "thinking_update", Payload: `{"delta":"先分析"}`},
		client.Event{Type: "thinking_update", Payload: `{"delta":"再动手"}`},
		client.Event{Type: "thinking_stop", Payload: `{}`},
		client.Event{Type: "message_update", Payload: `{"delta":"答案"}`},
	)
	want := []string{"info:💭 思考过程", "thinking:先分析再动手", "assistant:答案"}
	if got := roleTexts(m); !equal(got, want) {
		t.Errorf("行流 = %v, want %v", got, want)
	}
}

// 思考头只在真有内容时冒出来；空的 thinking_start 不留痕
func TestThinkingStartWithoutContent(t *testing.T) {
	m := testModel()
	feedEvents(&m, client.Event{Type: "thinking_start", Payload: `{}`})
	if len(m.lines) != 0 {
		t.Errorf("空 thinking_start 不应产生行，got %v", roleTexts(m))
	}
}

// 思考里的换行拆成独立行（每行独立折行，不会把整段当一行算）
func TestThinkingMultiline(t *testing.T) {
	m := testModel()
	feedEvents(&m,
		client.Event{Type: "thinking_update", Payload: `{"delta":"第一段\n第二"}`},
		client.Event{Type: "thinking_update", Payload: `{"delta":"段"}`},
	)
	want := []string{"info:💭 思考过程", "thinking:第一段", "thinking:第二段"}
	if got := roleTexts(m); !equal(got, want) {
		t.Errorf("行流 = %v, want %v", got, want)
	}
}

// 思考 → 正文 → 再思考：两个思考块各带一个头
func TestThinkingTwoBlocks(t *testing.T) {
	m := testModel()
	feedEvents(&m,
		client.Event{Type: "thinking_update", Payload: `{"delta":"想一"}`},
		client.Event{Type: "message_update", Payload: `{"delta":"说一"}`},
		client.Event{Type: "thinking_update", Payload: `{"delta":"想二"}`},
	)
	want := []string{"info:💭 思考过程", "thinking:想一", "assistant:说一", "info:💭 思考过程", "thinking:想二"}
	if got := roleTexts(m); !equal(got, want) {
		t.Errorf("行流 = %v, want %v", got, want)
	}
}

func TestRenderThinking(t *testing.T) {
	m := testModel()
	m.width, m.height = 80, 24
	feedEvents(&m,
		client.Event{Type: "thinking_update", Payload: `{"delta":"先想一下"}`},
		client.Event{Type: "message_update", Payload: `{"delta":"这是回答"}`},
	)
	view := m.View()
	thinkBody := strings.Index(view, "先想一下")
	answer := strings.Index(view, "这是回答")
	if thinkBody < 0 || answer < 0 {
		t.Fatalf("思考块与正文都应出现（think=%d answer=%d）", thinkBody, answer)
	}
	if thinkBody > answer {
		t.Errorf("思考内容应在正文之前（think=%d answer=%d）", thinkBody, answer)
	}

	// 无思考的消息不渲染思考头
	m2 := testModel()
	m2.width, m2.height = 80, 24
	feedEvents(&m2, client.Event{Type: "message_update", Payload: `{"delta":"无思考的回答"}`})
	if strings.Contains(m2.View(), "💭") {
		t.Error("无思考的消息不应渲染思考头")
	}
}

// 工具事件按到达顺序插进行流，不再被拼进模型正文
func TestToolLinesInterleave(t *testing.T) {
	m := testModel()
	feedEvents(&m,
		client.Event{Type: "message_update", Payload: `{"delta":"我来查一下"}`},
		client.Event{Type: "tool_execution_start", Payload: `{"name":"bash"}`},
		client.Event{Type: "tool_execution_end", Payload: `{"name":"bash","status":0}`},
		client.Event{Type: "message_update", Payload: `{"delta":"查完了"}`},
	)
	want := []string{"assistant:我来查一下", "tool:🔧 bash …", "tool:   ✓ bash", "assistant:查完了"}
	if got := roleTexts(m); !equal(got, want) {
		t.Errorf("行流 = %v, want %v", got, want)
	}
}

func TestToolFailureMark(t *testing.T) {
	m := testModel()
	feedEvents(&m, client.Event{Type: "tool_execution_end", Payload: `{"name":"bash","status":1}`})
	if got := lineTexts(m); len(got) != 1 || !strings.Contains(got[0], "✗") {
		t.Errorf("失败的工具应标 ✗，got %v", got)
	}
}

func equal(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
