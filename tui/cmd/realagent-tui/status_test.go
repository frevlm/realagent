// 读秒状态行：计时循环单例、跨 turn 连续读秒、事件驱动的措辞切换。
package main

import (
	"strings"
	"testing"
	"time"

	"realagent/tui/internal/client"
)

func TestActivityTickLoopSingle(t *testing.T) {
	var a activity
	base := time.Unix(0, 0)
	if cmd := a.begin("思考中", base); cmd == nil {
		t.Fatal("首次 begin 应启动计时循环")
	}
	if cmd := a.begin("生成回复", base.Add(time.Second)); cmd != nil {
		t.Error("已在计时时 begin 不应再起一条循环")
	}
	if a.verb != "生成回复" {
		t.Errorf("verb = %q, want 生成回复（begin 只换措辞）", a.verb)
	}
	if !a.start.Equal(base) {
		t.Error("切换措辞不应重置读秒起点")
	}
}

func TestActivityStaleTickDies(t *testing.T) {
	var a activity
	base := time.Unix(0, 0)
	a.begin("思考中", base)
	stale := a.seq - 1
	if cmd := a.tick(tickMsg{seq: stale, t: base.Add(time.Second)}); cmd != nil {
		t.Error("过期 seq 的 tick 不应续命")
	}
	if cmd := a.tick(tickMsg{seq: a.seq, t: base.Add(time.Second)}); cmd == nil {
		t.Error("当前 seq 的 tick 应续命")
	}
	a.stop()
	if cmd := a.tick(tickMsg{seq: a.seq, t: base.Add(2 * time.Second)}); cmd != nil {
		t.Error("stop 后 tick 不应续命")
	}
}

func TestElapsedText(t *testing.T) {
	cases := []struct {
		d    time.Duration
		want string
	}{
		{500 * time.Millisecond, "0s"},
		{12 * time.Second, "12s"},
		{65 * time.Second, "1m05s"},
		{3725 * time.Second, "62m05s"},
	}
	for _, c := range cases {
		if got := elapsedText(c.d); got != c.want {
			t.Errorf("elapsedText(%v) = %q, want %q", c.d, got, c.want)
		}
	}
}

func TestStatusVerbFromEvents(t *testing.T) {
	m := testModel()
	feed(&m,
		client.Frame{Event: "turn_start"},
	)
	if !m.busy.active || m.busy.verb != "思考中" {
		t.Fatalf("turn_start 后 active=%v verb=%q, want true/思考中", m.busy.active, m.busy.verb)
	}
	feed(&m, client.Frame{Event: "message_update", Delta: "答"})
	if m.busy.verb != "生成回复" {
		t.Errorf("message_update 后 verb = %q, want 生成回复", m.busy.verb)
	}
	feed(&m, client.Frame{Event: "tool_execution_start", Name: "bash"})
	if m.busy.verb != "执行工具 bash" {
		t.Errorf("tool_execution_start 后 verb = %q, want 执行工具 bash", m.busy.verb)
	}
	feed(&m, client.Frame{Event: "permission_request", ID: "1", Tool: "bash"})
	if m.busy.verb != "等待你的审批" {
		t.Errorf("permission_request 后 verb = %q, want 等待你的审批", m.busy.verb)
	}
}

// turn_end 从来不收工（收工判定不通过就还有下一轮），agent_end 才停。
func TestStatusStopsOnAgentEnd(t *testing.T) {
	m := testModel()
	feed(&m,
		client.Frame{Event: "turn_start"},
		client.Frame{Event: "turn_end"},
	)
	if !m.busy.active {
		t.Error("工具轮 turn_end 不应停止读秒")
	}
	feed(&m, client.Frame{Event: "turn_end"})
	if !m.busy.active {
		t.Error("没过收工判定，turn_end 也不该停止读秒")
	}
	feed(&m, client.Frame{Event: "agent_end", Cost: 0.01})
	if m.busy.active {
		t.Error("agent_end 应停止读秒")
	}
}

func TestStatusRenderInView(t *testing.T) {
	m := testModel()
	m.width, m.height = 80, 24
	base := time.Unix(0, 0)
	m.busy.begin("思考中", base)
	m.busy.tick(tickMsg{seq: m.busy.seq, t: base.Add(12 * time.Second)})
	view := m.View().Content
	if !strings.Contains(view, "思考中…") {
		t.Error("状态行未出现在视图中")
	}
	if !strings.Contains(view, "12s") {
		t.Error("读秒未出现在视图中")
	}
	// 状态行必须紧贴输入框上方
	if strings.Index(view, "思考中…") > strings.LastIndex(view, "> ") {
		t.Error("状态行应在输入框上方")
	}
	m.busy.stop()
	if strings.Contains(m.View().Content, "思考中…") {
		t.Error("空闲时不应渲染状态行")
	}
}

// status_update 帧是本次 run 累计的绝对值：覆盖写，TUI 不累加。
func TestCostOverwrites(t *testing.T) {
	m := testModel()
	feed(&m,
		client.Frame{Event: "turn_start"},
		client.Frame{Event: "status_update", Cost: 0.0031},
	)
	if m.busy.cost != 0.0031 {
		t.Fatalf("cost = %v, want 0.0031", m.busy.cost)
	}
	feed(&m, client.Frame{Event: "status_update", Cost: 0.0123})
	if m.busy.cost != 0.0123 {
		t.Errorf("cost = %v, want 0.0123（覆盖写，不累加）", m.busy.cost)
	}
}

// 新一轮 run 从零开始：读秒与花费同一个重置点。
func TestCostResetOnNewRun(t *testing.T) {
	m := testModel()
	feed(&m,
		client.Frame{Event: "turn_start"},
		client.Frame{Event: "status_update", Cost: 0.0123},
		client.Frame{Event: "agent_end", Cost: 0.0123},
	)
	if m.busy.cost == 0 {
		t.Fatal("收工后花费应保留到下一次 begin（本轮结果仍可见）")
	}
	feed(&m, client.Frame{Event: "turn_start"})
	if m.busy.cost != 0 {
		t.Errorf("新一轮 run 花费应清零，实际 %v", m.busy.cost)
	}
}

func TestStatusRenderCost(t *testing.T) {
	var a activity
	base := time.Unix(0, 0)
	a.begin("生成回复", base)
	a.cost = 0.0123
	a.tick(tickMsg{seq: a.seq, t: base.Add(5 * time.Second)})

	full := a.render(80)
	if !strings.Contains(full, "$0.0123") || !strings.Contains(full, "esc 中断") {
		t.Errorf("宽终端应显示完整尾巴，实际 %q", full)
	}
	// 降级阶梯：先丢提示，再丢花费，最后只剩读秒
	if noHint := a.render(30); strings.Contains(noHint, "ctrl+c") || !strings.Contains(noHint, "$0.0123") {
		t.Errorf("width=30 应丢提示保花费，实际 %q", noHint)
	}
	if narrow := a.render(16); strings.Contains(narrow, "$") || !strings.Contains(narrow, "5s") {
		t.Errorf("width=16 应丢花费保读秒，实际 %q", narrow)
	}
	// 无花费数据（表里没这个模型 / 端点不报用量）：整段隐藏，绝不显示 $0
	a.cost = 0
	if s := a.render(80); strings.Contains(s, "$") {
		t.Errorf("无花费数据时不应渲染花费段，实际 %q", s)
	}
}
