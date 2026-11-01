// 行模型的单元测试：折行 / 挂起缩进 / 整屏布局。
package main

import (
	"fmt"
	"strings"
	"testing"

	"github.com/charmbracelet/x/ansi"
)

// strip 去掉样式，只比内容（着色与否不该影响布局断言）
func strip(rows []string) []string {
	out := make([]string, len(rows))
	for i, r := range rows {
		out[i] = ansi.Strip(r)
	}
	return out
}

func TestWrapTextWidth(t *testing.T) {
	rows := wrapText("", strings.Repeat("中", 20), 10)
	if len(rows) < 4 {
		t.Fatalf("20 个中文（宽 40）在 10 列应折成 ≥4 行，got %d: %v", len(rows), rows)
	}
	for _, r := range rows {
		if w := ansi.StringWidth(r); w > 10 {
			t.Errorf("折行后宽度 %d 超出 10：%q", w, r)
		}
	}
}

// 前缀只出现在首行，续行用等宽空格挂起缩进——续行也不能撑破宽度
func TestWrapTextHangingIndent(t *testing.T) {
	rows := wrapText("> ", strings.Repeat("ab ", 20), 20)
	if len(rows) < 2 {
		t.Fatalf("应折成多行，got %v", rows)
	}
	if !strings.HasPrefix(rows[0], "> ") {
		t.Errorf("首行应带前缀，got %q", rows[0])
	}
	for i, r := range rows[1:] {
		if !strings.HasPrefix(r, "  ") {
			t.Errorf("续行 %d 应挂起缩进，got %q", i, r)
		}
		if w := ansi.StringWidth(r); w > 20 {
			t.Errorf("续行 %d 宽度 %d 超出 20：%q", i, w, r)
		}
	}
}

// 极窄终端不能算出负宽度而 panic
func TestWrapTextTinyWidth(t *testing.T) {
	for _, w := range []int{0, 1, 2, 3} {
		if rows := wrapText("> ", "中文abc", w); len(rows) == 0 {
			t.Errorf("width=%d 不应产出空结果", w)
		}
	}
}

// altscreen 下整屏恰好占满终端高度：viewport 的高度就是「屏幕减去底下那块」，
// 多一行少一行都是花屏（多了 Bubble Tea 自己截，光标就算错）
func TestViewFillsScreenExactly(t *testing.T) {
	m := testModel()
	m.width, m.height = 40, 10
	for i := range 50 {
		m.emit("assistant", strings.Repeat("长", 30)+string(rune('a'+i%26)))
	}
	if rows := strings.Split(m.View(), "\n"); len(rows) != m.height {
		t.Errorf("整屏应正好 %d 行，got %d", m.height, len(rows))
	}
}

// 历史多到几百屏，屏上也只铺一屏：贴着底，看得见最新的，看不见最早的（ADR-0020）
func TestHistoryShowsOneScreen(t *testing.T) {
	m := testModel()
	m.width, m.height = 40, 10
	m.emit("assistant", "最早的一行")
	for range 500 {
		m.emit("assistant", "中间")
	}
	m.emit("assistant", "最新的一行")
	screen := ansi.Strip(m.View())
	if !strings.Contains(screen, "最新的一行") || strings.Contains(screen, "最早的一行") {
		t.Errorf("屏上应只有最后一屏\n%s", screen)
	}
}

// 新内容来了跟到底；用户自己滚上去了就别把他拽回来，滚回底就接着跟
func TestScrollStaysWhereUserPutIt(t *testing.T) {
	m := testModel()
	m.width, m.height = 40, 10
	for i := range 50 {
		m.emit("assistant", fmt.Sprintf("第%d行", i))
	}
	w := m.viewWidth()
	m.scrollBy(-5, w, m.histHeight(w))
	before := m.View()
	m.emit("assistant", "新来的一行")
	if m.View() != before {
		t.Error("用户滚上去之后，新内容不该挪动屏幕")
	}
	m.scrollBy(100, w, m.histHeight(w))
	if !strings.Contains(m.View(), "新来的一行") {
		t.Error("滚回底部应恢复跟随，看得见新内容")
	}
}

// 滚到最顶就停在第一行，不越界
func TestScrollStopsAtTop(t *testing.T) {
	m := testModel()
	m.width, m.height = 40, 10
	m.emit("assistant", "第一行")
	for range 50 {
		m.emit("assistant", "后面")
	}
	w := m.viewWidth()
	m.scrollBy(-1000, w, m.histHeight(w))
	rows := strings.Split(ansi.Strip(m.View()), "\n")
	if !strings.Contains(rows[0], "第一行") {
		t.Errorf("滚到顶屏幕第一行应是最早的那行，got %q", rows[0])
	}
	if len(rows) != m.height {
		t.Errorf("整屏应正好 %d 行，got %d", m.height, len(rows))
	}
}

func TestRenderApprovalTruncates(t *testing.T) {
	p := &pendingApproval{tool: "bash", params: `{"cmd":"` + strings.Repeat("中", 100) + `"}`}
	rows := renderApproval(p, 40)
	if len(rows) != 2 {
		t.Fatalf("审批框应为 2 行，got %d", len(rows))
	}
	for _, r := range rows {
		if w := ansi.StringWidth(r); w > 40 {
			t.Errorf("审批框宽度 %d 超出 40：%q", w, ansi.Strip(r))
		}
	}
	if !strings.Contains(ansi.Strip(rows[0]), "中") {
		t.Error("按显示宽截断不应把中文切碎")
	}
}

// 历史与输入栏之间空一行。它归排版，不归历史——历史里塞空行等于把记录撑稀
func TestViewBlankLineBeforeInput(t *testing.T) {
	m := testModel()
	m.width, m.height = 40, 12
	m.emit("assistant", "回答")
	rows := strings.Split(m.View(), "\n")
	in := len(rows) - 1
	if ansi.Strip(rows[in-1]) != "" {
		t.Errorf("输入栏上方应是空行，got %q", ansi.Strip(rows[in-1]))
	}
	if !strings.Contains(ansi.Strip(rows[in-2]), "回答") {
		t.Errorf("空行上方应是模型输出，got %q", ansi.Strip(rows[in-2]))
	}
	if got := lineTexts(m); len(got) != 1 || got[0] != "回答" {
		t.Errorf("行流里只该有输出本身，got %v", got)
	}
}
