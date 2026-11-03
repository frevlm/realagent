// 输入框编辑器：rune 光标 + 常用行编辑键 + 多行输入。
//
// 一个 []rune 加一个下标，所有编辑都是切片拼接。按 rune 而不是 byte，中文不会被劈开。
package main

import (
	"strings"
	"unicode"

	"github.com/charmbracelet/x/ansi"
)

// editor 是输入框的全部状态：文本 + 光标位置（runes 下标，取值 0..len）
type editor struct {
	runes []rune
	pos   int
}

func (e *editor) value() string { return string(e.runes) }

// set 覆盖全文，光标置于末尾（斜杠命令补全走这条）
func (e *editor) set(s string) {
	e.runes = []rune(s)
	e.pos = len(e.runes)
}

func (e *editor) clear() { e.set("") }

// insert 在光标处插入文本。换行归一成 \n；其余控制字符（粘贴带进来的 ESC、BEL……）不是输入，丢掉
func (e *editor) insert(s string) {
	s = strings.ReplaceAll(strings.ReplaceAll(s, "\r\n", "\n"), "\r", "\n")
	r := []rune(strings.Map(func(r rune) rune {
		if unicode.IsControl(r) && r != '\n' && r != '\t' {
			return -1
		}
		return r
	}, s))
	e.runes = append(e.runes[:e.pos], append(r, e.runes[e.pos:]...)...)
	e.pos += len(r)
}

// backspace 删除光标前一个字符
func (e *editor) backspace() {
	if e.pos == 0 {
		return
	}
	e.runes = append(e.runes[:e.pos-1], e.runes[e.pos:]...)
	e.pos--
}

// del 删除光标处字符
func (e *editor) del() {
	if e.pos >= len(e.runes) {
		return
	}
	e.runes = append(e.runes[:e.pos], e.runes[e.pos+1:]...)
}

func (e *editor) left() {
	if e.pos > 0 {
		e.pos--
	}
}

func (e *editor) right() {
	if e.pos < len(e.runes) {
		e.pos++
	}
}

func (e *editor) home() { e.pos = 0 }

func (e *editor) end() { e.pos = len(e.runes) }

// killToEnd 删到行尾（ctrl+k）
func (e *editor) killToEnd() { e.runes = e.runes[:e.pos] }

// killToStart 删到行首（ctrl+u）
func (e *editor) killToStart() {
	e.runes = e.runes[e.pos:]
	e.pos = 0
}

// killWord 删除光标前一个词（ctrl+w）：先吃空白，再吃非空白
func (e *editor) killWord() {
	i := e.pos
	for i > 0 && unicode.IsSpace(e.runes[i-1]) {
		i--
	}
	for i > 0 && !unicode.IsSpace(e.runes[i-1]) {
		i--
	}
	e.runes = append(e.runes[:i], e.runes[e.pos:]...)
	e.pos = i
}

// 输入框首行的提示符；续行缩进同宽，与首行正文对齐
const prompt = "> "

// view 把输入框排进 width 列、最多 maxRows 行，连同光标在框内的位置（列, 行）。
// 按字符折行而不是按词：光标落在哪一格是一路数出来的，不用去猜折行器在哪断。
// \t 画成空格：终端按制表位跳格，宽度没法照字面算。行数超过 maxRows 时开一个窗，光标所在行总在窗内。
func (e *editor) view(width, maxRows int) (rows []string, cx, cy int) {
	avail, indent := max(width-len(prompt), 1), strings.Repeat(" ", len(prompt))
	row, col := userStyle.Render(prompt), 0
	for i := 0; i <= len(e.runes); i++ { // i == len(runes) 是末尾那格
		eol := i == len(e.runes) || e.runes[i] == '\n'
		s := ""
		if !eol {
			s = strings.ReplaceAll(string(e.runes[i]), "\t", "    ")
		}
		w, need := ansi.StringWidth(s), ansi.StringWidth(s)
		if i == e.pos {
			need = max(w, 1) // 光标停在哪，哪里就得占得住一格
		}
		if col > 0 && col+need > avail {
			rows, row, col = append(rows, row), indent, 0
		}
		if i == e.pos {
			cx, cy = col+len(prompt), len(rows)
		}
		if row, col = row+s, col+w; eol {
			rows, row, col = append(rows, row), indent, 0
		}
	}
	lo := max(0, cy-maxRows+1)
	return rows[lo:min(len(rows), lo+maxRows)], cx, cy - lo
}
