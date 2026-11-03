// 渲染层：条目 → 着色的渲染行，以及历史区的视窗。
//
// 样式在渲染最后一刻才套上。每个逻辑行缓存自己画出来的样子：收尾的行不再重画，
// 每帧真要重画的只有还在长的那一行，改宽时才整体重画——代价跟着增量走，不跟着历史长度走。
package main

import (
	"fmt"
	"strings"
	"unicode"

	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"
	"github.com/rivo/uniseg"
)

var (
	userStyle       = lipgloss.NewStyle().Foreground(lipgloss.Color("43")).Bold(true)
	assistantStyle  = lipgloss.NewStyle().Foreground(lipgloss.Color("214"))
	thinkingStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("245")).Italic(true)
	errorStyle      = lipgloss.NewStyle().Foreground(lipgloss.Color("196"))
	dimStyle        = lipgloss.NewStyle().Foreground(lipgloss.Color("240"))
	approvalStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("205")).Bold(true)
	spinnerStyle    = lipgloss.NewStyle().Foreground(lipgloss.Color("205"))
	statusStyle     = lipgloss.NewStyle().Foreground(lipgloss.Color("245"))
	menuStyle       = lipgloss.NewStyle().Foreground(lipgloss.Color("245"))
	menuSelStyle    = lipgloss.NewStyle().Foreground(lipgloss.Color("229")).Background(lipgloss.Color("238")).Bold(true)
	panelTitleStyle = lipgloss.NewStyle().Foreground(lipgloss.Color("205")).Bold(true)
)

// ==================== 一条怎么画 ====================

// toolTail 是工具输出收起时留下的末尾行数；ctrl+o 全部展开
const toolTail = 5

// line 是正文的一个逻辑行，连同它上次画出来的样子。text 只往后长，长度就是它的版本
type line struct {
	text  string
	rows  []string
	width int
	n     int
}

func (l *line) rendered(st lipgloss.Style, lead string, width int) []string {
	if l.rows == nil || l.width != width || l.n != len(l.text) {
		l.rows, l.width, l.n = paint(st, lead, l.text, width), width, len(l.text)
	}
	return l.rows
}

// rows 是这一条在 width 列下的渲染行（至少一行）。full = 工具输出全部展开
func (it *item) rows(width int, full bool) []string {
	k := it.kind
	var out []string
	if k.head != nil {
		out = paint(dimStyle, "", k.head(it), width)
	}
	body := it.lines
	if n := len(body); n > 1 && body[n-1].text == "" {
		body = body[:n-1] // 末尾的空行只是"下一行还没来"
	}
	if hidden := len(body) - toolTail; k == kindTool && !full && hidden > 0 {
		note := fmt.Sprintf("… 前面还有 %d 行（ctrl+o 展开）", hidden)
		out, body = append(out, paint(dimStyle, k.prefix, note, width)...), body[hidden:]
	}
	lead, indent := k.prefix, strings.Repeat(" ", len(k.prefix))
	for i := range body {
		out = append(out, body[i].rendered(k.style, lead, width)...)
		lead = indent
	}
	return out
}

// 折行后正文的最小可用宽度：终端窄到离谱时也别把字劈成一列
const minWrapWidth = 4

// paint 把一行原文折进 width 列并着色：首行带 lead，续行挂起缩进到 lead 之后
func paint(st lipgloss.Style, lead, text string, width int) []string {
	indent := strings.Repeat(" ", ansi.StringWidth(lead))
	rows := wrap(printable(text), max(width-len(indent), minWrapWidth))
	for i, r := range rows {
		rows[i] = st.Render(lead + r)
		lead = indent
	}
	return rows
}

// wrap 按 Unicode 断行规则（UAX #14）把一行纯文本折进 width 列：中文字与字之间可断，
// 英文按词断，句中标点不落到行首；一段比整行还宽才从字素之间劈开。行尾的空格不留。
func wrap(s string, width int) []string {
	var rows []string
	row, col := "", 0
	for state := -1; s != ""; {
		var seg string
		seg, s, _, state = uniseg.FirstLineSegmentInString(s, state)
		if col > 0 && col+ansi.StringWidth(strings.TrimRight(seg, " ")) > width {
			rows, row, col = append(rows, strings.TrimRight(row, " ")), "", 0
		}
		for g := -1; seg != ""; {
			var c string
			c, seg, _, g = uniseg.FirstGraphemeClusterInString(seg, g)
			if col > 0 && col+ansi.StringWidth(c) > width && c != " " {
				rows, row, col = append(rows, strings.TrimRight(row, " ")), "", 0
			}
			row, col = row+c, col+ansi.StringWidth(c)
		}
	}
	return append(rows, strings.TrimRight(row, " "))
}

// fit 把一行按显示宽截断（宽度兜底，窄终端下也不会算出负数）。
// 用于菜单/审批框这种「一条就一行、宁截不折」的场合。
func fit(s string, width int) string {
	return ansi.Truncate(s, max(width, minWrapWidth), "…")
}

// printable 把一行原文洗成终端照字面画的样子，折行算的宽才是上屏的宽。
// 原文来自模型与工具（进度条、彩色输出），终端会自己解释的东西都得在这里摊平：
// ANSI 序列去掉；\r 把光标拽回行首、后写的盖掉前写的，所以只留最后一个 \r 之后的（CRLF 的 \r 先剥掉）；
// \t 上屏占好几格、折行却按 0 宽算，换成空格；剩下的控制字符没有字面样子，丢掉。
func printable(s string) string {
	s = strings.TrimRight(ansi.Strip(s), "\r")
	s = strings.ReplaceAll(s[strings.LastIndexByte(s, '\r')+1:], "\t", "    ")
	return strings.Map(func(r rune) rune {
		if unicode.IsControl(r) {
			return -1
		}
		return r
	}, s)
}

// ==================== 历史区的视窗 ====================

// pos 是一个渲染行的位置：第 i 条的第 row 个渲染行
type pos struct{ i, row int }

func (a pos) before(b pos) bool { return a.i < b.i || a.i == b.i && a.row < b.row }

// scroll 是历史区停在哪。锚在条目上、不锚总行号：新内容只往后追加，锚不动，滚上去看的人
// 不会被拽走；也就不用算全量总行数——每帧只碰屏幕上那几条，代价与历史长度无关。
type scroll struct {
	up  bool // false = 贴着底，跟着新内容走
	top pos  // up 时屏幕第一行
}

// rowsOf 是第 i 条的渲染行
func (m *model) rowsOf(i int) []string { return m.tr.items[i].rows(m.width, m.expand) }

// tail 是贴底时屏幕第一行的位置，连同从它到底的渲染行（不足 h 行就是全部）
func (m *model) tail(h int) (pos, []string) {
	var chunks [][]string // 倒着收
	n := 0
	top := pos{len(m.tr.items), 0}
	for i := len(m.tr.items) - 1; i >= 0 && n < h; i-- {
		rows := m.rowsOf(i)
		take := min(h-n, len(rows))
		chunks = append(chunks, rows[len(rows)-take:])
		n += take
		top = pos{i, len(rows) - take}
	}
	out := make([]string, 0, n)
	for i := len(chunks) - 1; i >= 0; i-- {
		out = append(out, chunks[i]...)
	}
	return top, out
}

// history 是历史区这一帧的 h 行。不足一屏时在上方补空行：头几句贴着输入框
func (m *model) history(h int) []string {
	bottom, rows := m.tail(h)
	if top := m.scroll.top; m.scroll.up && top.before(bottom) {
		rows = rows[:0:0]
		for p := m.clamp(top); len(rows) < h; p = (pos{p.i + 1, 0}) {
			rows = append(rows, m.rowsOf(p.i)[p.row:]...)
		}
		rows = rows[:h]
	}
	return append(make([]string, h-len(rows)), rows...)
}

// clamp 把改宽、收起后可能越界的 row 收回这一条的最后一个渲染行
func (m *model) clamp(p pos) pos {
	if p.i < len(m.tr.items) {
		p.row = min(p.row, len(m.rowsOf(p.i))-1)
	}
	return p
}

// scrollBy 把视窗挪 n 个渲染行（负 = 往上）。挪到底就恢复跟随
func (m *model) scrollBy(n int) {
	bottom, _ := m.tail(m.histHeight())
	p := bottom
	if m.scroll.up && m.scroll.top.before(bottom) {
		p = m.clamp(m.scroll.top)
	}
	for n < 0 && (p.i > 0 || p.row > 0) {
		if p.row == 0 {
			p.i--
			p.row = len(m.rowsOf(p.i))
		}
		step := min(p.row, -n)
		p.row -= step
		n += step
	}
	for n > 0 && p.before(bottom) {
		left := len(m.rowsOf(p.i)) - p.row
		if n < left {
			p.row += n
			break
		}
		n -= left
		p = pos{p.i + 1, 0}
	}
	m.scroll = scroll{up: p.before(bottom), top: p}
}
