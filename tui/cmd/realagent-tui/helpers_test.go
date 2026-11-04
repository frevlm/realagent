// 测试共用的小工具：造按键、造帧，把对话与屏幕读成字符串好比对。
package main

import (
	"strings"

	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/ansi"

	"realagent/tui/internal/client"
)

// key 造一个没有字面的键按下：回车、方向键、Esc……
func key(code rune) tea.KeyPressMsg { return tea.KeyPressMsg{Code: code} }

// keyRunes 造一次打字，s 是打进去的字面。终端送来的单个字符以它自己为键码，多个字符是扩展键
func keyRunes(s string) tea.KeyPressMsg {
	code := tea.KeyExtended
	if r := []rune(s); len(r) == 1 {
		code = r[0]
	}
	return tea.KeyPressMsg{Code: code, Text: s}
}

// feed 依次处理帧（不经节拍器）
func feed(m *model, fs ...client.Frame) {
	for _, f := range fs {
		m.handleEvent(f)
	}
}

// texts 是对话里每一条的正文
func texts(m model) []string {
	var out []string
	for _, it := range m.tr.items {
		var ls []string
		for _, l := range it.lines {
			ls = append(ls, l.text)
		}
		out = append(out, strings.Join(ls, "\n"))
	}
	return out
}

// dump 是对话里每一条的「类别:正文」。工具调用写成「tool:名字:状态」，有输出时再接「|输出」
func dump(m model) []string {
	names := map[*kind]string{kindUser: "user", kindAssistant: "assistant", kindThinking: "thinking",
		kindTool: "tool", kindInfo: "info", kindError: "error", kindCompact: "compact"}
	var out []string
	for i, text := range texts(m) {
		it := m.tr.items[i]
		s := names[it.kind] + ":" + text
		if it.kind == kindTool {
			s = "tool:" + it.name + ":" + [...]string{"running", "ok", "failed"}[it.state]
			if text != "" {
				s += "|" + text
			}
		}
		out = append(out, s)
	}
	return out
}

// strip 去掉样式，只比内容
func strip(rows []string) []string {
	out := make([]string, len(rows))
	for i, r := range rows {
		out[i] = ansi.Strip(r)
	}
	return out
}

// screen 是这一帧的各行（去掉样式）
func screen(m model) []string {
	return strings.Split(ansi.Strip(m.View().Content), "\n")
}
