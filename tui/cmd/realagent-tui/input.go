// 输入行编辑器：rune 光标 + 常用行编辑键 + 多行输入。
//
// 一个 []rune 加一个下标，所有编辑都是切片拼接。按 rune 而不是 byte，中文不会被劈开。
package main

import (
	"strings"
	"unicode"
)

// editor 是输入框的全部状态：文本 + 光标位置（runes 下标，取值 0..len）
type editor struct {
	runes []rune
	pos   int
}

func (e *editor) value() string { return string(e.runes) }

func (e *editor) empty() bool { return len(e.runes) == 0 }

// set 覆盖全文，光标置于末尾（斜杠命令补全走这条）
func (e *editor) set(s string) {
	e.runes = []rune(s)
	e.pos = len(e.runes)
}

func (e *editor) clear() { e.set("") }

// insert 在光标处插入文本（粘贴的 \r 归一为 \n）
func (e *editor) insert(s string) {
	r := []rune(strings.ReplaceAll(strings.ReplaceAll(s, "\r\n", "\n"), "\r", "\n"))
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

// display 返回带光标的显示文本：光标处字符反显，行尾则反显一个空格。
// 光标落在换行符上时反显空格再补换行——否则反显块会吞掉换行。
func (e *editor) display() string {
	if e.pos >= len(e.runes) {
		return string(e.runes) + cursorStyle.Render(" ")
	}
	head, cur, tail := string(e.runes[:e.pos]), e.runes[e.pos], string(e.runes[e.pos+1:])
	if cur == '\n' {
		return head + cursorStyle.Render(" ") + "\n" + tail
	}
	return head + cursorStyle.Render(string(cur)) + tail
}

// view 渲染输入框：首行 "> " 提示符着色，续行挂起缩进对齐
func (e *editor) view(width int) []string {
	rows := wrapText("> ", e.display(), width)
	rows[0] = userStyle.Render("> ") + strings.TrimPrefix(rows[0], "> ")
	return rows
}
