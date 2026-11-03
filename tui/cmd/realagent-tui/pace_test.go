// 节拍器：成团到的增量匀速吐，其余事件不插队。
package main

import (
	"reflect"
	"strings"
	"testing"
	"time"

	"realagent/tui/internal/client"
)

// live 造一帧当前对话本身的实时事件
func live(m *model, f client.Frame) client.Frame {
	f.Root, f.SessionID = m.client.SessionID(), m.client.SessionID()
	return f
}

// 一团 90 个字同时到：按 paceLag 匀开，过 1/9 的时间露出 10 个字，过满 paceLag 全部露出
func TestPaceSpreadsBurst(t *testing.T) {
	m := testModel()
	text := strings.Repeat("字", 90)
	t0 := time.Now()
	m.enqueue(live(&m, client.Frame{Event: "message_update", Delta: text}), t0)

	m.onPace(t0.Add(paceLag / 9))
	if want := []string{"assistant:" + strings.Repeat("字", 10)}; !reflect.DeepEqual(dump(m), want) {
		t.Errorf("1/9 处对话 = %v, want %v", dump(m), want)
	}
	m.onPace(t0.Add(paceLag))
	if want := []string{"assistant:" + text}; !reflect.DeepEqual(dump(m), want) {
		t.Errorf("满 paceLag 对话 = %v, want %v", dump(m), want)
	}
}

// 正文吐到一半来了工具帧：工具条目排在整段正文之后
func TestPaceKeepsOrder(t *testing.T) {
	m := testModel()
	t0 := time.Now()
	m.enqueue(live(&m, client.Frame{Event: "message_update", Delta: "我来查一下"}), t0)
	m.enqueue(live(&m, client.Frame{Event: "tool_execution_start", Name: "bash"}), t0)
	m.onPace(t0.Add(paceLag / 5))
	if want := []string{"assistant:我"}; !reflect.DeepEqual(dump(m), want) {
		t.Errorf("吐了一个字时对话 = %v, want %v", dump(m), want)
	}
	m.onPace(t0.Add(paceLag))
	if want := []string{"assistant:我来查一下", "tool:bash:running"}; !reflect.DeepEqual(dump(m), want) {
		t.Errorf("对话 = %v, want %v", dump(m), want)
	}
}

// Esc：队里收到的立刻全部上屏
func TestPaceFlush(t *testing.T) {
	m := testModel()
	m.enqueue(live(&m, client.Frame{Event: "message_update", Delta: "已经收到的全部"}), time.Now())
	m.flush()
	if want := []string{"assistant:已经收到的全部"}; !reflect.DeepEqual(dump(m), want) {
		t.Errorf("对话 = %v, want %v", dump(m), want)
	}
}
