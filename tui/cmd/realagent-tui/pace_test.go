// 节拍器：成团到的增量匀速吐，其余事件不插队；以及折行宽与上屏宽一致。
package main

import (
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/charmbracelet/x/ansi"
	"realagent/tui/internal/client"
)

// live 造一帧当前对话本身的实时事件
func live(m *model, typ, body string) client.Event {
	id := m.client.SessionID()
	return client.Event{Type: typ, Payload: fmt.Sprintf(`{"root":%q,"session_id":%q,%s}`, id, id, body)}
}

// 一团 90 个字同时到：按 paceLag 匀开，过 1/9 的时间露出 10 个字，过满 paceLag 全部露出
func TestPaceSpreadsBurst(t *testing.T) {
	m := testModel()
	text := strings.Repeat("字", 90)
	t0 := time.Now()
	m.enqueue(live(&m, "message_update", `"delta":"`+text+`"`), t0)

	m.onPace(t0.Add(paceLag / 9))
	if want := []string{"assistant:" + strings.Repeat("字", 10)}; !equal(roleTexts(m), want) {
		t.Errorf("1/9 处行流 = %v, want %v", roleTexts(m), want)
	}
	m.onPace(t0.Add(paceLag))
	if want := []string{"assistant:" + text}; !equal(roleTexts(m), want) {
		t.Errorf("满 paceLag 行流 = %v, want %v", roleTexts(m), want)
	}
}

// 正文吐到一半来了工具帧：工具行排在整段正文之后
func TestPaceKeepsOrder(t *testing.T) {
	m := testModel()
	t0 := time.Now()
	m.enqueue(live(&m, "message_update", `"delta":"我来查一下"`), t0)
	m.enqueue(live(&m, "tool_execution_start", `"name":"bash"`), t0)
	m.onPace(t0.Add(paceLag / 5))
	if want := []string{"assistant:我"}; !equal(roleTexts(m), want) {
		t.Errorf("吐了一个字时行流 = %v, want %v", roleTexts(m), want)
	}
	m.onPace(t0.Add(paceLag))
	if want := []string{"assistant:我来查一下", "tool:🔧 bash …"}; !equal(roleTexts(m), want) {
		t.Errorf("行流 = %v, want %v", roleTexts(m), want)
	}
}

// Esc：队里收到的立刻全部上屏
func TestPaceFlush(t *testing.T) {
	m := testModel()
	m.enqueue(live(&m, "message_update", `"delta":"已经收到的全部"`), time.Now())
	m.flush()
	if want := []string{"assistant:已经收到的全部"}; !equal(roleTexts(m), want) {
		t.Errorf("行流 = %v, want %v", roleTexts(m), want)
	}
}

// 带 tab 的行折出来的每一行，上屏宽都不超过屏宽
func TestTabsFitWidth(t *testing.T) {
	m := testModel()
	m.emit("output", "\t\t"+strings.Repeat("a", 38))
	for _, r := range m.lines[0].rendered(40) {
		if w := ansi.StringWidth(r); w > 40 {
			t.Errorf("行宽 %d 超过 40：%q", w, ansi.Strip(r))
		}
	}
}

// \r 照终端的样子：后写的盖掉前写的；CRLF 的 \r 不吞掉整行
func TestCarriageReturn(t *testing.T) {
	for in, want := range map[string]string{
		"进度 10%\r进度 100%": "进度 100%",
		"一行\r":            "一行",
	} {
		if got := layout(line{role: "output", text: in}, 40); !equal(got, []string{want}) {
			t.Errorf("layout(%q) = %q, want %q", in, got, want)
		}
	}
}
