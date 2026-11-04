// 画面布局：每帧恰好铺满终端，光标停在输入处（ADR-0020）。
//
//	历史区    剩下多少行给多少；审批框、子面板、斜杠菜单是浮层，从它底部往上盖，不挤它
//	（空行）  历史与底下那块之间
//	读秒行    忙时才有
//	输入框    最多 editorRows 行，超出在框里滚
//	状态栏
//
// 每行最后按屏宽截一刀：哪里宽度算错，坏的只是那一行的行尾，不会把下面整屏挤歪。
package main

import (
	"strings"

	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/ansi"
)

// View 画整个屏幕。尺寸还没到（高 0）就什么都不画
func (m model) View() tea.View {
	dock, cx, cy := m.dock()
	rows := append(m.history(m.histHeight()), "")
	cover(rows, append([]string{""}, m.overlays()...))
	cy += len(rows)
	rows = append(rows, dock...)
	if cut := len(rows) - m.height; cut > 0 { // 屏幕比底下那块还矮：从上面截
		rows, cy = rows[cut:], cy-cut
	}
	for i := range rows {
		rows[i] = ansi.Truncate(rows[i], m.width, "")
	}

	v := tea.NewView(strings.Join(rows, "\n"))
	v.AltScreen = true // 不开鼠标模式：会关掉终端的选中复制
	// 模态开着时按键不进输入框，光标也就不停在那儿；窄到连提示符都放不下时输入处不在屏上
	if m.approval == nil && m.panel == nil && cy >= 0 && cx < m.width {
		v.Cursor = tea.NewCursor(cx, cy)
	}
	return v
}

// editorRows 是输入框最多占几行：屏高的三分之一，矮屏也给够三行
func editorRows(h int) int { return max(3, h/3) }

// dock 是屏幕底下那块：读秒行 + 输入框 + 状态栏，连同光标在块内的位置。
// 屏幕再矮，输入框也只让到读秒行与状态栏之外剩下的行数，至少一行：光标所在行总在屏上
func (m model) dock() (rows []string, cx, cy int) {
	var above []string
	if s := m.busy.render(m.width); s != "" {
		above = append(above, s)
	}
	// 状态栏让着输入框：屏幕矮到放不下时从下往上少画几行，输入框至少留一行
	below := m.sl.render(m.width)
	below = below[:min(len(below), max(0, m.height-len(above)-1))]
	ed, cx, cy := m.ed.view(m.width, max(1, min(editorRows(m.height), m.height-len(above)-len(below))))
	return append(append(above, ed...), below...), cx, cy + len(above)
}

// histHeight 是历史区的行数：屏幕减去底下那块，再减去中间那行空行
func (m model) histHeight() int {
	dock, _, _ := m.dock()
	return max(0, m.height-len(dock)-1)
}

// overlays 是盖在历史区底部的浮层，自上而下：审批框、子面板、斜杠菜单
func (m model) overlays() []string {
	var rows []string
	if m.approval != nil {
		rows = append(rows, renderApproval(m.approval, m.width)...)
	}
	if m.panel != nil {
		rows = append(rows, renderPanel(m.panel, m.width)...)
	}
	if matches := m.menuMatches(); len(matches) > 0 {
		rows = append(rows, renderMenu(matches, m.menuIndex(matches), m.width)...)
	}
	return rows
}

// cover 把 top 贴着 rows 的底往上盖；top 比 rows 高时只盖得下它最底下那截
func cover(rows, top []string) {
	copy(rows[max(0, len(rows)-len(top)):], top[max(0, len(top)-len(rows)):])
}
