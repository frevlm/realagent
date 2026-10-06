// 子面板：斜杠命令的第二层选择。
//
// 面板 = 一列可选项 + 一个高亮下标，每项自带确认时要发的整条命令（submit）。
// 确认就是把 submit 写进输入框走 submitInput，与手打同一条路。
// 数据来自 /model /resume 回包里本来就有的 data 载荷。
package main

import (
	"encoding/json"
	"fmt"
	"strings"

	"realagent/tui/internal/client"
)

// 面板最多同时显示的条目数（超出按高亮项开窗，同斜杠菜单）
const panelMaxRows = 10

// panelItem 是面板里的一项
type panelItem struct {
	label  string // 展示文本（不含标记与光标）
	mark   bool   // 当前生效项（● 标记，打开时高亮落在这里）
	submit string // 确认时提交的整条输入，如 "/model gpt-5"
}

// panel 是打开着的子面板。approval 是模态，panel 也是：开着时按键只喂它。
type panel struct {
	title string
	items []panelItem
	sel   int
}

// makePanel 按命令名与结果载荷造面板；造不出返回 nil，调用方退回文本输出。
func makePanel(command string, data json.RawMessage, agentID int) *panel {
	switch command {
	case "model":
		return modelPanel(data)
	case "resume":
		return sessionPanel(data, agentID)
	}
	return nil
}

// modelPanel 把 /model 的模型清单做成选择面板：Enter = 切主模型
func modelPanel(data json.RawMessage) *panel {
	var list []client.ModelInfo
	if err := json.Unmarshal(data, &list); err != nil || len(list) == 0 {
		return nil
	}
	p := &panel{title: "选择主模型"}
	for _, mi := range list {
		text := mi.Name
		if mi.OwnedBy != "" {
			text += " [" + mi.OwnedBy + "]"
		}
		if c := humanContext(mi.Context); c != "" {
			text += "  " + c
		}
		p.items = append(p.items, panelItem{label: text, mark: mi.Current, submit: "/model " + mi.Name})
	}
	p.sel = p.markIndex()
	return p
}

// sessionPanel 把 /resume 的会话清单做成选择面板：Enter = 恢复那个会话。
// 清单已按最近写入倒序（core 侧排好），所以第一项就是"上一个会话"。
func sessionPanel(data json.RawMessage, agentID int) *panel {
	var list []client.SessionInfo
	if err := json.Unmarshal(data, &list); err != nil || len(list) == 0 {
		return nil
	}
	p := &panel{title: "恢复会话"}
	for _, s := range list {
		title := s.Title
		if title == "" {
			title = "（空会话）"
		}
		p.items = append(p.items, panelItem{
			label:  fmt.Sprintf("%s  %s  %d 条", s.ID, title, s.Messages),
			mark:   s.OpenedBy == agentID,
			submit: "/resume " + s.ID,
		})
	}
	p.sel = p.markIndex()
	return p
}

// agentPanel 把 agent 清单做成选择面板：Enter = 切过去看它（ADR-0020）。
func agentPanel(list []client.AgentInfo, cur int) *panel {
	if len(list) == 0 {
		return nil
	}
	p := &panel{title: "切到哪个 agent"}
	for _, a := range list {
		label := fmt.Sprintf("%d  %s  %s", a.ID, a.State, a.Workdir)
		if n := len(a.InEdges) + len(a.OutEdges); n > 0 {
			label += fmt.Sprintf("  (入 %d 出 %d)", len(a.InEdges), len(a.OutEdges))
		}
		p.items = append(p.items, panelItem{
			label:  label,
			mark:   a.ID == cur,
			submit: fmt.Sprintf("/agents %d", a.ID),
		})
	}
	p.sel = p.markIndex()
	return p
}

// markIndex 返回当前生效项的下标（没有则 0）：打开面板时光标就落在那儿
func (p *panel) markIndex() int {
	for i, it := range p.items {
		if it.mark {
			return i
		}
	}
	return 0
}

// panelWantOf 判断一条输入的结果该不该开面板。fromPanel = 这条输入是面板里
// 确认出来的（启停类操作改完接着操作，面板不该自己跑掉）。
//
//	/model      列清单 → 开面板选；/model <name> 是明确指令，选完即走
//	/resume     列会话 → 开面板选
//	/statusline 纯本地，列表与切换回的是同一份清单 → 面板里连续操作不用重打命令
func panelWantOf(input string, fromPanel bool) string {
	cmd, args := splitCommand(input)
	switch cmd {
	case "/model":
		if args == "" {
			return "model"
		}
	case "/resume":
		if args == "" {
			return "resume"
		}
	case "/statusline":
		if args == "" || fromPanel {
			return "statusline"
		}
	}
	return ""
}

// splitCommand 按首个空白把输入切成命令名与参数（参数已去掉首尾空白）
func splitCommand(input string) (string, string) {
	input = strings.TrimSpace(input)
	if i := strings.IndexAny(input, " \n\t"); i >= 0 {
		return input[:i], strings.TrimSpace(input[i+1:])
	}
	return input, ""
}

// window 按高亮项在 n 条里开一个最多 max 行的窗口，返回 [lo, hi)
func window(n, sel, max int) (int, int) {
	if n <= max {
		return 0, n
	}
	lo := sel - max/2
	if lo < 0 {
		lo = 0
	}
	if lo > n-max {
		lo = n - max
	}
	return lo, lo + max
}

// wrapIndex 把下标绕回 [0, n)（面板与菜单的上下键都循环）
func wrapIndex(i, n int) int {
	if n <= 0 {
		return 0
	}
	return ((i % n) + n) % n
}

// renderPanel 渲染子面板（输入行上方）：标题 + 可选项 + 键位提示
func renderPanel(p *panel, width int) []string {
	out := []string{panelTitleStyle.Render(fit(p.title, width))}
	lo, hi := window(len(p.items), p.sel, panelMaxRows)
	for i := lo; i < hi; i++ {
		mark := "  "
		if p.items[i].mark {
			mark = "● "
		}
		text := fit(mark+p.items[i].label, width-2)
		if i == p.sel {
			out = append(out, menuSelStyle.Render("▸ "+text))
		} else {
			out = append(out, menuStyle.Render("  "+text))
		}
	}
	return append(out, dimStyle.Render("  ↑/↓ 选择 · Enter 确认 · Esc 取消"))
}
