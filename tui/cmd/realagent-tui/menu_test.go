// 斜杠命令菜单的单元测试：过滤 / 补全 / 按键状态机 / 命令结果渲染。
package main

import (
	"reflect"
	"strings"
	"testing"

	"github.com/charmbracelet/bubbletea"

	"realagent/tui/internal/client"
)

var testCmds = []client.Command{
	{Name: "model", Description: "查看模型清单"},
	{Name: "plugins", Description: "查看装了哪些 plugin"},
}

func testModel() model {
	// 带一个没连过的 client：渲染路径要问它当前是哪段对话。
	// New 不连接，构造是纯本地的
	return model{commands: testCmds, client: client.New("127.0.0.1:1", "/tmp")}
}

// typed 造一个输入了 s 的模型（光标在末尾）
func typed(s string) model {
	m := testModel()
	m.ed.set(s)
	return m
}

// lineTexts 返回行流的全部文本 —— altscreen 之后 m.lines 就是本次会话的全部输出，
// 没有「已提交／未提交」这条边界了（ADR-0020）。
func lineTexts(m model) []string {
	var out []string
	for _, l := range m.lines {
		out = append(out, l.text)
	}
	return out
}

func key(k tea.KeyType) tea.KeyMsg { return tea.KeyMsg{Type: k} }

func keyRunes(s string) tea.KeyMsg {
	return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(s)}
}

func TestMenuOpen(t *testing.T) {
	cases := []struct {
		input string
		want  bool
	}{
		{"", false},
		{"hello", false},
		{"/", true},
		{"/ne", true},
		{"/new 1", true}, // menuOpen 只看前缀；含空格由 menuMatches 关闭
	}
	for _, c := range cases {
		if got := typed(c.input).menuOpen(); got != c.want {
			t.Errorf("menuOpen(%q) = %v, want %v", c.input, got, c.want)
		}
	}
}

func TestMenuMatches(t *testing.T) {
	cases := []struct {
		input string
		want  []string // 命中的命令名（nil 表示无）
	}{
		{"", nil},
		{"hello", nil},
		{"/", []string{"model", "plugins", "statusline", "new", "resume", "quit"}},
		{"/n", []string{"new"}},
		{"/m", []string{"model"}},
		{"/re", []string{"resume"}},
		{"/q", []string{"quit"}}, // 本地命令与远端命令在菜单里没有区别
		{"/xyz", nil},
		{"/new x", nil}, // 参数模式关闭菜单
		{"/new ", nil},
		{"/new\n", nil}, // 多行输入同样不是菜单
	}
	for _, c := range cases {
		got := typed(c.input).menuMatches()
		if c.want == nil {
			if len(got) != 0 {
				t.Errorf("menuMatches(%q) = %v, want none", c.input, names(got))
			}
			continue
		}
		if len(got) != len(c.want) {
			t.Errorf("menuMatches(%q) = %v, want %v", c.input, names(got), c.want)
			continue
		}
		for i, w := range c.want {
			if got[i].Name != w {
				t.Errorf("menuMatches(%q)[%d] = %q, want %q", c.input, i, got[i].Name, w)
			}
		}
	}
}

func names(cmds []client.Command) []string {
	var out []string
	for _, c := range cmds {
		out = append(out, c.Name)
	}
	return out
}

// 远端命令列表为空（core 未启动/拉取失败）时，本地命令仍应可见——
// 它们不经 core，不该被 core 的状态拖累。
func TestMenuMatchesEmptyList(t *testing.T) {
	m := model{}
	m.ed.set("/")
	got := names(m.menuMatches())
	want := names(localCmds)
	if !reflect.DeepEqual(got, want) {
		t.Errorf("空远端命令列表应只剩本地命令 %v，got %v", want, got)
	}
}

func TestMenuIndexClamp(t *testing.T) {
	m := testModel()
	m.menuSel = 5
	if got := m.menuIndex(testCmds); got != 0 {
		t.Errorf("越界 menuSel 应归 0，got %d", got)
	}
	m.menuSel = 1
	if got := m.menuIndex(testCmds); got != 1 {
		t.Errorf("有效 menuSel 应保留，got %d", got)
	}
	if got := m.menuIndex(nil); got != 0 {
		t.Errorf("空匹配应归 0，got %d", got)
	}
}

func TestEnterExecutesHighlighted(t *testing.T) {
	m, cmd := typed("/re").handleKey(key(tea.KeyEnter)) // 命中 resume，高亮它
	if cmd == nil {
		t.Fatal("enter 应返回发送命令")
	}
	if v := m.ed.value(); v != "" {
		t.Errorf("发送后输入应清空，got %q", v)
	}
	if got := lineTexts(m); len(got) != 1 || got[0] != "/resume" {
		t.Errorf("应发送 /resume，got %v", got)
	}
}

// 普通消息**不在本地回显**：core 收下它就发一帧 message_start 带正文回来，
// 本地再画一遍就是画两遍（ADR-0019 §5 / ADR-0020）
func TestEnterNormalMessage(t *testing.T) {
	m, cmd := typed("hello world").handleKey(key(tea.KeyEnter))
	if cmd == nil {
		t.Fatal("enter 应返回发送命令")
	}
	if got := lineTexts(m); len(got) != 0 {
		t.Errorf("普通输入不该本地回显，got %v", got)
	}
	if v := m.ed.value(); v != "" {
		t.Errorf("发送后输入应清空，got %q", v)
	}
}

// core 的 message_start 带正文回来，那一行才出现——多行按 \n 拆开
func TestUserLineComesFromCore(t *testing.T) {
	m := testModel()
	m.handleEvent(client.Event{Type: "message_start", Payload: `{"role":"user","text":"第一行\n第二行"}`})
	got := lineTexts(m)
	if len(got) != 2 || got[0] != "第一行" || got[1] != "第二行" {
		t.Errorf("用户行应由 message_start 带来并按行拆开，got %v", got)
	}
}

// 别的 agent 发来的消息与完成通知走的是同一条路（收件箱里都是 user 消息）
func TestPeerMessageRendered(t *testing.T) {
	m := testModel()
	m.handleEvent(client.Event{Type: "message_start", Payload: `{"role":"user","text":"[a2 已完成] 干完了"}`})
	if got := lineTexts(m); len(got) != 1 || got[0] != "[a2 已完成] 干完了" {
		t.Errorf("完成通知应原样出现在行流里，got %v", got)
	}
}

func TestTabCompletes(t *testing.T) {
	m, _ := typed("/").handleKey(key(tea.KeyTab))
	if v := m.ed.value(); v != "/model" {
		t.Errorf("Tab 应补全为首项 /model，got %q", v)
	}
}

func TestUpDownWrap(t *testing.T) {
	m := typed("/") // testCmds 两条 + 全部本地命令
	last := len(m.menuMatches()) - 1
	// down: 0 → 1 → ... → 末项，再 down 回绕到 0
	for i := 1; i <= last; i++ {
		m, _ = m.handleKey(key(tea.KeyDown))
		if m.menuSel != i {
			t.Fatalf("down 应移到 %d，got %d", i, m.menuSel)
		}
	}
	m, _ = m.handleKey(key(tea.KeyDown))
	if m.menuSel != 0 {
		t.Errorf("down 应回绕到 0，got %d", m.menuSel)
	}
	// up 回绕：0 → 末项
	m, _ = m.handleKey(key(tea.KeyUp))
	if m.menuSel != last {
		t.Errorf("up 应回绕到末项 %d，got %d", last, m.menuSel)
	}
}

// esc 只收菜单，不没收用户敲的字；再编辑则菜单复原
func TestEscHidesMenuKeepsInput(t *testing.T) {
	m, _ := typed("/re").handleKey(key(tea.KeyEsc))
	if v := m.ed.value(); v != "/re" {
		t.Errorf("esc 不应清空输入，got %q", v)
	}
	if len(m.menuMatches()) != 0 {
		t.Error("esc 后菜单应收起")
	}
	m, _ = m.handleKey(keyRunes("s"))
	if len(m.menuMatches()) == 0 {
		t.Error("继续输入应让菜单复原")
	}
}

func TestSpaceClosesMenu(t *testing.T) {
	m, _ := typed("/new").handleKey(key(tea.KeySpace))
	if v := m.ed.value(); v != "/new " {
		t.Fatalf("空格应进入输入，got %q", v)
	}
	if got := m.menuMatches(); len(got) != 0 {
		t.Errorf("含空格应关闭菜单，got %v", names(got))
	}
}

// 菜单条目多于窗口时按高亮开窗，且行数不超上限
func TestMenuWindow(t *testing.T) {
	var many []client.Command
	for i := range 20 {
		many = append(many, client.Command{Name: string(rune('a' + i))})
	}
	rows := renderMenu(many, 15, 40)
	if len(rows) != menuMaxRows {
		t.Fatalf("菜单行数 = %d, want %d", len(rows), menuMaxRows)
	}
	if !strings.Contains(strings.Join(rows, "\n"), "▸") {
		t.Error("开窗后应仍能看到高亮项")
	}
}

func TestDescribeCommand(t *testing.T) {
	if got := describeCommand("unknown"); !strings.Contains(got, "/unknown") {
		t.Errorf("describeCommand = %q, want contains /unknown", got)
	}
}

// sendMsg 兜底：斜杠命令结果（ok:true）应渲染为 info 行
func TestCommandResultRendered(t *testing.T) {
	m := testModel()
	m.awaiting = true
	nm, _ := m.Update(sendMsg{reply: client.Reply{Ok: true, Command: "new"}})
	m = nm.(model)
	if len(m.lines) != 1 {
		t.Fatalf("命令结果应产生 1 行，got %d", len(m.lines))
	}
	if m.lines[0].role != "info" || !strings.Contains(m.lines[0].text, "/new") {
		t.Errorf("命令结果应为 info 渲染，got %+v", m.lines[0])
	}
	if m.awaiting {
		t.Error("命令结果落地后不应再等 POST 兜底")
	}
}

func TestCommandErrorRendered(t *testing.T) {
	m := testModel()
	m.awaiting = true
	nm, _ := m.Update(sendMsg{reply: client.Reply{Error: "unknown command"}})
	m = nm.(model)
	if len(m.lines) != 1 || m.lines[0].role != "error" {
		t.Fatalf("命令错误应渲染为 error，got %+v", m.lines)
	}
}

// 事件流已经吐出内容后，POST 的兜底结果不得再插一脚
func TestSendFallbackIgnoredAfterStream(t *testing.T) {
	m := testModel()
	m.awaiting = true
	m.handleEvent(client.Event{Type: "message_update", Payload: `{"delta":"已经在答了"}`})
	nm, _ := m.Update(sendMsg{reply: client.Reply{Ok: true, Command: "new"}})
	m = nm.(model)
	if got := lineTexts(m); len(got) != 1 || got[0] != "已经在答了" {
		t.Errorf("兜底不应覆盖流式内容，got %v", got)
	}
}
