// 画面：每帧恰好铺满终端、每行不超宽，光标停在输入处，浮层不挤历史区，历史区按用户停的位置滚。
package main

import (
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/charmbracelet/x/ansi"
)

// sized 造一个 w×h 的终端，历史区里有 n 条
func sized(w, h, n int) model {
	m := testModel()
	m.width, m.height = w, h
	for i := range n {
		m.emit(kindAssistant, fmt.Sprintf("第%d行 %s", i, strings.Repeat("长", i%40)))
	}
	return m
}

// 什么状态、什么尺寸都恰好 h 行、每行不超过 w 列，光标要么不画、要么在屏内
func TestFrameFillsScreen(t *testing.T) {
	states := map[string]func(m *model){
		"空":     func(m *model) { m.tr = transcript{} },
		"长历史":   func(m *model) {},
		"斜杠菜单":  func(m *model) { m.ed.set("/") },
		"子面板":   func(m *model) { m.panel = sessionPanel(sessionsList(), "") },
		"审批":    func(m *model) { m.approval = &pendingApproval{id: "a", tool: "bash"} },
		"四十行粘贴": func(m *model) { m.ed.insert(strings.Repeat("粘贴进来的一行\n", 40)) },
		"读秒+状态栏+多行输入": func(m *model) {
			m.busy.active, m.busy.verb = true, "思考中"
			m.sl = testStatusline() // 两行、能折行降级的那种
			m.ed.insert("一\n二\n三")
		},
	}
	for name, setup := range states {
		for _, size := range [][2]int{{80, 30}, {40, 12}, {12, 5}, {20, 3}, {2, 6}} {
			m := sized(size[0], size[1], 60)
			setup(&m)
			rows := screen(m)
			if len(rows) != m.height {
				t.Errorf("%s %dx%d：%d 行", name, m.width, m.height, len(rows))
			}
			for i, r := range rows {
				if w := ansi.StringWidth(r); w > m.width {
					t.Errorf("%s %dx%d：第 %d 行宽 %d：%q", name, m.width, m.height, i, w, r)
				}
			}
			if c := m.View().Cursor; c != nil && (c.X >= m.width || c.Y < 0 || c.Y >= m.height) {
				t.Errorf("%s %dx%d：光标 %+v 在屏外", name, m.width, m.height, c)
			}
		}
	}
}

// 光标停在输入处：按显示宽数列，中文一个字两列；模态开着时不画
func TestCursorAtCaret(t *testing.T) {
	m := sized(40, 10, 0)
	m.ed.insert("你好世界")
	m.ed.left()
	m.ed.left()
	c := m.View().Cursor
	y := m.height - 1 - len(m.sl.render(m.width)) // 输入框在状态栏上面
	if c == nil || c.X != len(prompt)+4 || c.Y != y || screen(m)[c.Y] != "> 你好世界" {
		t.Fatalf("光标 = %+v，want (%d, %d) 在 \"> 你好世界\" 那行", c, len(prompt)+4, y)
	}
	m.approval = &pendingApproval{id: "a", tool: "bash"}
	if c := m.View().Cursor; c != nil {
		t.Errorf("审批开着，光标 = %+v", c)
	}
}

// 输入框有上限：四十行的粘贴挤不掉历史区；矮到挤在一起时让的是输入框的行数，光标所在行仍在屏上
func TestEditorYields(t *testing.T) {
	m := sized(40, 30, 1)
	m.ed.insert(strings.Repeat("粘贴进来的一行\n", 40))
	if !strings.Contains(strings.Join(screen(m), "\n"), "第0行") {
		t.Errorf("历史被挤掉了：\n%s", strings.Join(screen(m), "\n"))
	}

	m = sized(40, 3, 0)
	m.busy.active, m.busy.verb = true, "思考中"
	m.sl = statusline{cwd: "proj", tier: "unicode"}
	m.ed.insert("一\n二\n三")
	m.ed.home()
	if c := m.View().Cursor; c == nil || c.Y != 1 || screen(m)[1] != "> 一" {
		t.Errorf("光标 = %+v，屏幕 %q", c, screen(m))
	}
}

// 斜杠菜单盖在历史区底部：菜单之上的历史一行不动，之下是空行、菜单、输入框
func TestOverlayKeepsHistory(t *testing.T) {
	m := sized(40, 20, 40)
	before := screen(m)
	m.ed.set("/")
	after := screen(m)
	menu := strip(m.overlays())
	keep := m.histHeight() - len(menu)
	if !reflect.DeepEqual(before[:keep], after[:keep]) {
		t.Errorf("菜单之上的历史动了：\nbefore %q\nafter  %q", before[:keep], after[:keep])
	}
	want := append(append(append([]string{""}, menu...), "> /"), plain(m.sl.render(m.width))...)
	if got := after[keep:]; !reflect.DeepEqual(got, want) {
		t.Errorf("历史之下应是空行、菜单、输入框、状态栏，got %q", got)
	}
}

// 贴底时跟着新内容走；用户滚上去了新内容不挪屏幕，滚回底接着跟；滚到顶停在第一行
func TestScroll(t *testing.T) {
	m := sized(40, 10, 50)
	if s := strings.Join(screen(m), "\n"); !strings.Contains(s, "第49行") || strings.Contains(s, "第0行") {
		t.Errorf("贴底时应只看见最后一屏：\n%s", s)
	}
	m.scrollBy(-5)
	before := screen(m)
	m.emit(kindAssistant, "新来的一行")
	if !reflect.DeepEqual(screen(m), before) {
		t.Error("用户滚上去之后，新内容不该挪动屏幕")
	}
	m.scrollBy(1000)
	if !strings.Contains(strings.Join(screen(m), "\n"), "新来的一行") {
		t.Error("滚回底部应恢复跟随")
	}
	m.scrollBy(-1000)
	if rows := screen(m); !strings.HasPrefix(rows[0], "第0行") {
		t.Errorf("滚到顶屏幕第一行应是最早那条，got %q", rows[0])
	}
}

// 输入框排版：首行带提示符，续行同宽缩进，光标位置一路数出来；行数超了开窗跟着光标
func TestEditorView(t *testing.T) {
	cases := []struct {
		name          string
		text          string
		pos, width, n int // 光标的 rune 下标（-1 = 末尾）、宽、最多几行
		rows          []string
		cx, cy        int
	}{
		{"一行", "ab", -1, 20, 9, []string{"> ab"}, 4, 0},
		{"满行后光标换到下一行", "abcdefghij", -1, 12, 9, []string{"> abcdefghij", "  "}, 2, 1},
		{"宽字放不下就整个换行", "中文字", -1, 7, 9, []string{"> 中文", "  字"}, 4, 1},
		{"光标在换行符上", "上\n下", 1, 20, 9, []string{"> 上", "  下"}, 4, 0},
		{"tab 画成空格", "\tx", -1, 20, 9, []string{">     x"}, 7, 0},
		{"窗停在光标所在的末尾", "1\n2\n3\n4", -1, 20, 2, []string{"  3", "  4"}, 3, 1},
		{"窗跟着光标回到开头", "1\n2\n3\n4", 0, 20, 2, []string{"> 1", "  2"}, 2, 0},
	}
	for _, c := range cases {
		var e editor
		e.insert(c.text)
		if c.pos >= 0 {
			e.pos = c.pos
		}
		rows, cx, cy := e.view(c.width, c.n)
		if got := strip(rows); !reflect.DeepEqual(got, c.rows) || cx != c.cx || cy != c.cy {
			t.Errorf("%s：rows=%q cursor=(%d,%d)，want rows=%q cursor=(%d,%d)", c.name, got, cx, cy, c.rows, c.cx, c.cy)
		}
	}
}
