// 子面板的单元测试：数据构造 / 开窗 / 按键状态机 / 与命令结果的衔接。
package main

import (
	"encoding/json"
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"

	"realagent/tui/internal/client"
)

func modelsJSON() json.RawMessage {
	data, _ := json.Marshal([]client.ModelInfo{
		{Name: "deepseek-chat", OwnedBy: "deepseek", Context: 131072},
		{Name: "deepseek-reasoner", OwnedBy: "deepseek", Context: 131072, Current: true},
	})
	return data
}

func sessionsList() []client.SessionInfo {
	return []client.SessionInfo{
		{ID: "s-002", Title: "改一个 bug", Messages: 12, State: "running"},
		{ID: "s-001", Title: "读代码", Messages: 4},
	}
}

func TestPanelWantOf(t *testing.T) {
	cases := []struct {
		input     string
		fromPanel bool
		want      string
	}{
		{"/model", false, "model"},
		{"/model deepseek-chat", false, ""}, // 带名 = 明确指令，选完即走
		{"/model deepseek-chat", true, ""},  // 面板里选中模型同样收工
		{"/statusline", false, "statusline"},
		{"/statusline icons nerd", false, ""},
		{"/statusline icons nerd", true, "statusline"},
		{"/new", false, ""},
		{"你好", false, ""},
	}
	for _, c := range cases {
		if got := panelWantOf(c.input, c.fromPanel); got != c.want {
			t.Errorf("panelWantOf(%q, %v) = %q, want %q", c.input, c.fromPanel, got, c.want)
		}
	}
}

func TestSplitCommand(t *testing.T) {
	cases := []struct{ in, cmd, args string }{
		{"/model", "/model", ""},
		{"/model  gpt ", "/model", "gpt"},
		{"/resume s-001", "/resume", "s-001"},
		{"", "", ""},
	}
	for _, c := range cases {
		cmd, args := splitCommand(c.in)
		if cmd != c.cmd || args != c.args {
			t.Errorf("splitCommand(%q) = (%q, %q), want (%q, %q)", c.in, cmd, args, c.cmd, c.args)
		}
	}
}

// 打开面板时高亮落在当前生效项上，确认项发的是完整命令
func TestModelPanel(t *testing.T) {
	p := modelPanel(modelsJSON())
	if p == nil {
		t.Fatal("modelPanel 返回 nil")
	}
	if len(p.items) != 2 {
		t.Fatalf("items = %d, want 2", len(p.items))
	}
	if p.sel != 1 {
		t.Errorf("sel = %d, want 1（current 项）", p.sel)
	}
	if p.items[1].submit != "/model deepseek-reasoner" {
		t.Errorf("submit = %q", p.items[1].submit)
	}
	if !strings.Contains(p.items[0].label, "128k") {
		t.Errorf("label 少了上下文窗口: %q", p.items[0].label)
	}
}

// 会话面板：高亮落在当前会话上，确认项发的是完整命令
func TestSessionPanel(t *testing.T) {
	p := sessionPanel(sessionsList(), "s-002")
	if p == nil {
		t.Fatal("sessionPanel 返回 nil")
	}
	if p.sel != 0 {
		t.Errorf("sel = %d, want 0（current 项）", p.sel)
	}
	if got := p.items[1].submit; got != "/resume s-001" {
		t.Errorf("submit = %q", got)
	}
	if !strings.Contains(p.items[0].label, "改一个 bug") || !strings.Contains(p.items[0].label, "正在跑") {
		t.Errorf("label 少了标题或状态: %q", p.items[0].label)
	}
}

// 无数据造不出面板：退回文本输出，不是新的失败点
func TestMakePanelEmpty(t *testing.T) {
	if p := makePanel("model", json.RawMessage(`[]`)); p != nil {
		t.Error("空清单不该开面板")
	}
	if p := makePanel("model", json.RawMessage(`{`)); p != nil {
		t.Error("坏载荷不该开面板")
	}
	if p := makePanel("plugins", nil); p != nil {
		t.Error("无面板的命令不该开面板")
	}
	if p := sessionPanel(nil, ""); p != nil {
		t.Error("没有对话不该开面板")
	}
}

func TestWindow(t *testing.T) {
	cases := []struct {
		n, sel, max, lo, hi int
	}{
		{3, 0, 8, 0, 3},     // 装得下就全给
		{20, 0, 8, 0, 8},    // 头部不越界
		{20, 10, 8, 6, 14},  // 高亮居中
		{20, 19, 8, 12, 20}, // 尾部不越界
	}
	for _, c := range cases {
		lo, hi := window(c.n, c.sel, c.max)
		if lo != c.lo || hi != c.hi {
			t.Errorf("window(%d,%d,%d) = (%d,%d), want (%d,%d)", c.n, c.sel, c.max, lo, hi, c.lo, c.hi)
		}
	}
}

func TestWrapIndex(t *testing.T) {
	cases := []struct{ i, n, want int }{{-1, 3, 2}, {3, 3, 0}, {1, 3, 1}, {0, 0, 0}}
	for _, c := range cases {
		if got := wrapIndex(c.i, c.n); got != c.want {
			t.Errorf("wrapIndex(%d,%d) = %d, want %d", c.i, c.n, got, c.want)
		}
	}
}

// ↑/↓ 循环，Esc 关面板
func TestPanelNav(t *testing.T) {
	m := testModel()
	m.panel = modelPanel(modelsJSON())
	m, _ = m.panelKey("down")
	if m.panel.sel != 0 {
		t.Errorf("down 未从末项绕回首项: sel = %d", m.panel.sel)
	}
	m, _ = m.panelKey("up")
	if m.panel.sel != 1 {
		t.Errorf("up 未绕回末项: sel = %d", m.panel.sel)
	}
	m, _ = m.panelKey("esc")
	if m.panel != nil || m.panelWant != "" {
		t.Error("esc 应关闭面板并清掉重开意图")
	}
}

// Enter 确认 = 把 submit 当成用户输入发出去（复用 submitInput，没有第二条路）
func TestPanelEnterSubmits(t *testing.T) {
	m := testModel()
	m.panel = sessionPanel(sessionsList(), "")
	m.panel.sel = 1 // s-001
	m, cmd := m.panelKey("enter")
	if m.panel != nil {
		t.Error("确认后面板应关闭")
	}
	if cmd == nil {
		t.Error("确认应去拉那段对话的历史")
	}
	if m.client.SessionID() != "s-001" {
		t.Errorf("当前对话 = %q, want s-001", m.client.SessionID())
	}
	if m.panelWant != "" {
		t.Errorf("panelWant = %q, want 空（换完即走）", m.panelWant)
	}
}

// 命令结果回来时按 panelWant 开面板，且不再往 scrollback 打那坨清单
func TestReplyOpensPanel(t *testing.T) {
	m := testModel()
	m.awaiting = true
	m.panelWant = "model"
	m, _ = m.update(sendMsg{reply: client.Reply{Ok: true, Command: "model", Data: modelsJSON()}})
	if m.panel == nil {
		t.Fatal("结果未开面板")
	}
	if len(lineTexts(m)) != 0 {
		t.Errorf("开了面板还打文本清单: %v", lineTexts(m))
	}
	if m.awaiting || m.busy.active {
		t.Error("命令结果到手就该收工")
	}
}

// 没打算开面板（如手打 /model <name>）时行为不变：照旧渲染文本
func TestReplyWithoutPanelWant(t *testing.T) {
	m := testModel()
	m.awaiting = true
	m, _ = m.update(sendMsg{reply: client.Reply{Ok: true, Command: "model", Data: modelsJSON()}})
	if m.panel != nil {
		t.Error("没要面板就别弹")
	}
	if len(lineTexts(m)) == 0 {
		t.Error("应渲染模型清单文本")
	}
}

// /statusline 无参开面板；面板里改完仍留在面板
func TestStatuslinePanel(t *testing.T) {
	m := testModel()
	m.ed.set("/statusline")
	m, _ = m.submitInput(false)
	if m.panel == nil {
		t.Fatal("/statusline 未开面板")
	}
	sel := len(m.panel.items) - 1 // icons nerd
	m.panel.sel = sel
	m, _ = m.panelKey("enter")
	if m.sl.iconSet != "nerd" {
		t.Errorf("iconSet = %q, want nerd", m.sl.iconSet)
	}
	if m.panel == nil {
		t.Fatal("面板里改完应留在面板")
	}
	if !m.panel.items[sel].mark {
		t.Error("重开的面板应标记新生效项")
	}
}

// 面板是模态：普通按键不进输入框
func TestPanelIsModal(t *testing.T) {
	m := testModel()
	m.panel = modelPanel(modelsJSON())
	m, _ = m.handleKey(keyRunes("x"))
	if !m.ed.empty() {
		t.Errorf("面板开着时按键不该写进输入框: %q", m.ed.value())
	}
	if _, cmd := m.handleKey(key(tea.KeyCtrlC)); cmd == nil {
		t.Error("ctrl+c 仍应能退出")
	}
}
