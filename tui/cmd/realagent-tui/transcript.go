// 对话的数据：一条一条，不是一行一行。
//
// 协议本身按条目说话：工具调用有开始、实时输出、结束，靠 id 串起来（PROTOCOL.md tool_output）。
// 条目里只存原文，正文按逻辑行存；怎么按宽度画归 render.go。
package main

import (
	"strings"

	"charm.land/lipgloss/v2"
)

// kind 是一类条目的画法：正文的样式、首行前缀（续行同宽缩进），以及可有可无的头一行
type kind struct {
	style  lipgloss.Style
	prefix string
	head   func(it *item) string
}

var (
	kindUser      = &kind{style: userStyle, prefix: "> "}
	kindAssistant = &kind{style: assistantStyle}
	kindThinking  = &kind{style: thinkingStyle, head: func(*item) string { return "💭 思考过程" }}
	kindTool      = &kind{style: dimStyle, prefix: "   ", head: toolHead}
	kindInfo      = &kind{style: dimStyle} // 客户端自己的提示、子 agent 的动静
	kindError     = &kind{style: errorStyle, prefix: "! "}
)

// toolState 是一次工具调用走到哪了
type toolState int

const (
	toolRunning toolState = iota
	toolOK
	toolFailed
)

// toolHead 是工具调用的头一行：在跑 / 成功 / 失败
func toolHead(it *item) string {
	return [...]string{"🔧 " + it.name + " …", "✓ " + it.name, "✗ " + it.name}[it.state]
}

// item 是对话里的一条。正文只往后长：流式增量、工具输出都是续写末行
type item struct {
	kind  *kind
	lines []line // 正文的逻辑行
	name  string // 工具名
	id    string // 工具调用 id
	state toolState
}

// write 续写正文：头一段接在末行后面，每个 \n 另起一行
func (it *item) write(s string) {
	for i, part := range strings.Split(s, "\n") {
		if i == 0 && len(it.lines) > 0 {
			it.lines[len(it.lines)-1].text += part
		} else {
			it.lines = append(it.lines, line{text: part})
		}
	}
}

// transcript 是当前那段对话的全部条目。它是盘上会话的副本，换对话时丢掉重读。
type transcript struct {
	items []item
	open  bool // 末条还在接增量
}

// push 追加一条；开着的那条随之收尾
func (t *transcript) push(it item) *item {
	t.items = append(t.items, it)
	t.open = false
	return &t.items[len(t.items)-1]
}

// stream 把增量续写到开着的末条；末条不是这一类就另起一条
func (t *transcript) stream(k *kind, delta string) {
	if n := len(t.items); !t.open || t.items[n-1].kind != k {
		t.push(item{kind: k})
		t.open = true
	}
	t.items[len(t.items)-1].write(delta)
}

// close 给开着的那条收尾
func (t *transcript) close() { t.open = false }

// call 是 id 那次工具调用；没见过就在末尾开一条。新的在后，倒着找
func (t *transcript) call(id string) *item {
	for i := len(t.items) - 1; i >= 0; i-- {
		if t.items[i].kind == kindTool && t.items[i].id == id {
			return &t.items[i]
		}
	}
	return t.push(item{kind: kindTool, id: id})
}
