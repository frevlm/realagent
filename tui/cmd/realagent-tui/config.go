// /config：设置一览与修改。
//
// 可调键与可取值由 core 给（/config 回包的 items），改了写 settings.json、即刻生效。
// statusline 那一项是 TUI 自己的：值是一条 shell 命令（外部的可执行文件），
// 写进 ~/.realagent/statusline.json（格式见 statusline_cmd.go），空 = 用内置状态栏。
//
//	/config                      两层面板：先选键，再选值
//	/config <键>                 直接开那个键的取值面板
//	/config <键> <值>            直接改
//	/config statusline [命令]    换状态栏命令（不经 core）
package main

import (
	"encoding/json"
	"strings"
)

// configItem 是 core 回包里的一项可调配置
type configItem struct {
	Key         string            `json:"key"`
	Description string            `json:"description"`
	Value       json.RawMessage   `json:"value"`
	Choices     []json.RawMessage `json:"choices"`
}

// configItems 解 /config 的载荷；解不开返回 nil
func configItems(data json.RawMessage) []configItem {
	var d struct{ Items []configItem }
	if json.Unmarshal(data, &d) != nil {
		return nil
	}
	return d.Items
}

// valueText 是一个 JSON 值在命令行上的写法：字符串去引号，其余原样（core 按同一规则认）
func valueText(v json.RawMessage) string {
	var s string
	if json.Unmarshal(v, &s) == nil {
		return s
	}
	return string(v)
}

// configPanel 是第一层：每个键一项，Enter 进它的取值面板；statusline 那项 Enter 把命令填进输入框改
func configPanel(data json.RawMessage, statusline string) *panel {
	items := configItems(data)
	if items == nil {
		return nil
	}
	p := &panel{title: "设置（Enter 修改）"}
	for _, it := range items {
		p.items = append(p.items, panelItem{label: it.Key + "  " + valueText(it.Value) + "  · " + it.Description, submit: "/config " + it.Key})
	}
	p.items = append(p.items, panelItem{
		label:  "statusline  " + statuslineLabel(statusline) + "  · 状态栏命令（外部可执行文件，清空 = 内置）",
		submit: strings.TrimSpace("/config statusline "+statusline) + " ",
		edit:   true,
	})
	return p
}

// choicePanel 是第二层：一个键的全部可取值，● 是当前值
func choicePanel(data json.RawMessage) *panel {
	items := configItems(data)
	if len(items) != 1 {
		return nil
	}
	it := items[0]
	p := &panel{title: it.Key + " · " + it.Description}
	cur := valueText(it.Value)
	for _, c := range it.Choices {
		v := valueText(c)
		p.items = append(p.items, panelItem{label: v, mark: v == cur, submit: "/config " + it.Key + " " + v})
	}
	p.sel = p.markIndex()
	return p
}

// renderConfig 是不开面板时的文本：一键一行
func renderConfig(data json.RawMessage) string {
	items := configItems(data)
	if items == nil {
		return describeCommand("config")
	}
	lines := make([]string, len(items))
	for i, it := range items {
		lines[i] = "⚙ " + it.Key + " = " + valueText(it.Value)
	}
	return strings.Join(lines, "\n")
}

func statuslineLabel(command string) string {
	if command == "" {
		return "内置"
	}
	return command
}
