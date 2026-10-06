// 渲染层：行模型 + 折行。
//
// 界面只有一条 line 流（原始文本 + 角色），数据里没有 ANSI，样式在渲染最后一刻
// 才套上，所以终端改宽时历史能跟着重排。每帧全量重绘（ADR-0020）。
package main

import (
	"strings"

	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"
)

// ==================== 样式 ====================

var (
	userStyle       = lipgloss.NewStyle().Foreground(lipgloss.Color("43")).Bold(true)
	assistantStyle  = lipgloss.NewStyle().Foreground(lipgloss.Color("214"))
	toolStyle       = lipgloss.NewStyle().Foreground(lipgloss.Color("240")).Italic(true)
	thinkingStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("245")).Italic(true)
	errorStyle      = lipgloss.NewStyle().Foreground(lipgloss.Color("196"))
	dimStyle        = lipgloss.NewStyle().Foreground(lipgloss.Color("240"))
	approvalStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("205")).Bold(true)
	spinnerStyle    = lipgloss.NewStyle().Foreground(lipgloss.Color("205"))
	statusStyle     = lipgloss.NewStyle().Foreground(lipgloss.Color("245"))
	menuStyle       = lipgloss.NewStyle().Foreground(lipgloss.Color("245"))
	menuSelStyle    = lipgloss.NewStyle().Foreground(lipgloss.Color("229")).Background(lipgloss.Color("238")).Bold(true)
	panelTitleStyle = lipgloss.NewStyle().Foreground(lipgloss.Color("205")).Bold(true)
	cursorStyle     = lipgloss.NewStyle().Reverse(true)
)

// ==================== 行模型 ====================

// line 是渲染的最小单位：一行原始文本 + 决定样式的角色。
type line struct {
	role string // user / assistant / thinking / tool / output / error / info
	text string
}

// styleOf 返回角色样式
func styleOf(role string) lipgloss.Style {
	switch role {
	case "user":
		return userStyle
	case "assistant":
		return assistantStyle
	case "error":
		return errorStyle
	case "tool":
		return toolStyle
	case "thinking":
		return thinkingStyle
	}
	return dimStyle // info / output（工具原始 stdout，压暗与工具行区分）/ 未知
}

// prefixOf 返回角色前缀。前缀只出现在首折行，续行用等宽空格挂起缩进对齐。
func prefixOf(role string) string {
	switch role {
	case "user":
		return "> "
	case "error":
		return "! "
	}
	return ""
}

// 折行后正文的最小可用宽度：终端窄到离谱时也别把字劈成一列
const minWrapWidth = 4

// wrapText 把一段原文按宽度折成渲染行：首行带 prefix，续行挂起缩进对齐。
// prefix 允许自带 ANSI（宽度按显示宽算），正文不着色。
func wrapText(prefix, text string, width int) []string {
	pw := ansi.StringWidth(prefix)
	w := width - pw
	if w < minWrapWidth {
		w = minWrapWidth
	}
	indent := strings.Repeat(" ", pw)
	rows := strings.Split(ansi.Wrap(text, w, ""), "\n")
	for i := range rows {
		if i == 0 {
			rows[i] = prefix + rows[i]
		} else {
			rows[i] = indent + rows[i]
		}
	}
	return rows
}

// fit 把一行按显示宽截断（宽度兜底，窄终端下也不会算出负数）。
// 用于菜单/审批框这种「一条就一行、宁截不折」的场合。
func fit(s string, width int) string {
	if width < minWrapWidth {
		width = minWrapWidth
	}
	return ansi.Truncate(s, width, "…")
}

// layout 把一行折成未着色的渲染行
func layout(l line, width int) []string {
	return wrapText(prefixOf(l.role), l.text, width)
}

// render 把一行折成着色后的渲染行
func render(l line, width int) []string {
	st := styleOf(l.role)
	rows := layout(l, width)
	out := make([]string, 0, len(rows))
	for _, r := range rows {
		out = append(out, st.Render(r))
	}
	return out
}

// ==================== 行流的追加 ====================

// emit 追加若干整行（自带 \n 则拆开）
func (m *model) emit(role, text string) {
	m.closeLine()
	for _, s := range strings.Split(text, "\n") {
		m.lines = append(m.lines, line{role: role, text: s})
	}
}

// stream 把流式增量续写到开着的行；遇 \n 起新行，角色变了先收尾再另起。
func (m *model) stream(role, delta string) {
	if !m.open || len(m.lines) == 0 || m.lines[len(m.lines)-1].role != role {
		m.closeLine() // 换角色时也丢掉上一条的空尾行（理由同 closeLine）
		m.lines = append(m.lines, line{role: role})
		m.open = true
	}
	parts := strings.Split(delta, "\n")
	m.lines[len(m.lines)-1].text += parts[0]
	for _, s := range parts[1:] {
		m.lines = append(m.lines, line{role: role, text: s})
	}
}

// closeLine 给开着的行收尾。空的开着的行只是"光标停在行首"（增量以 \n 结尾留下的），
// 收尾时丢掉；emit 出来的空行不受影响。
func (m *model) closeLine() {
	if m.open && len(m.lines) > 0 && m.lines[len(m.lines)-1].text == "" {
		m.lines = m.lines[:len(m.lines)-1]
	}
	m.open = false
}
