// 首启引导：settings.json 里 setup_done 不为 true（或 `realagent-tui setup`）时，进主界面前先走一遍。
//
// 一页一项：protocol → base_url → api_key → model → small_model，现值预填。
// 模型两页先拉端点的模型清单（POST /setup/models）给人选，选「手动输入…」或拉不到就手填。
// 写盘归 core（POST /setup），它的内存同步更新，走完直接进主界面。Esc 回上一页，第一页 Esc 跳过。
package main

import (
	"fmt"
	"slices"
	"strings"

	"github.com/charmbracelet/bubbletea"
	"realagent/tui/internal/client"
)

var protocols = []string{"anthropic-messages", "openai-chat", "openai-responses"}

const (
	stepProtocol = iota
	stepBaseURL
	stepAPIKey
	stepModel
	stepSmallModel
)

var setupSteps = [...]struct{ name, hint string }{
	{"protocol", "端点说哪种协议：Anthropic 原厂及其兼容端点选 anthropic-messages"},
	{"base_url", "anthropic-messages 不带 /v1（https://api.anthropic.com）；openai-* 带到 /v1（https://api.openai.com/v1）"},
	{"api_key", "端点的 API key"},
	{"model", "对话用的主模型"},
	{"small_model", "收工判定用的小模型；手动输入留空 = 不做收工判定"},
}

const manualItem = "手动输入…"

type setupModel struct {
	client  *client.Client
	step    int
	eds     [len(setupSteps)]editor // 每页一个；协议页存协议名
	models  []string                // 拉到的模型清单
	loading bool
	manual  bool   // 模型页在手填
	sel     int    // 列表页高亮
	note    string // 拉清单失败 / 保存失败的原话
}

type setupSaved struct{ err error }

type modelsFetched struct {
	list []string
	err  error
}

func newSetupModel(c *client.Client, s client.Setup) setupModel {
	m := setupModel{client: c}
	for i, v := range []string{s.Protocol, s.BaseURL, s.APIKey, s.Model, s.SmallModel} {
		m.eds[i].set(v)
	}
	return m.enter(stepProtocol)
}

func (m setupModel) values() client.Setup {
	v := func(i int) string { return strings.TrimSpace(m.eds[i].value()) }
	return client.Setup{Protocol: v(0), BaseURL: v(1), APIKey: v(2), Model: v(3), SmallModel: v(4)}
}

// choices 是列表页的可选项；文本页返回 nil
func (m setupModel) choices() []string {
	switch {
	case m.step == stepProtocol:
		return protocols
	case m.step >= stepModel && !m.manual && !m.loading:
		return append(slices.Clone(m.models), manualItem)
	}
	return nil
}

// enter 进入某一页：高亮落在现值上；模型页现值不在清单里、或清单是空的，就直接手填
func (m setupModel) enter(step int) setupModel {
	m.step, m.manual = step, false
	cur := strings.TrimSpace(m.eds[step].value())
	i := slices.Index(m.choices(), cur)
	m.sel = max(i, 0)
	m.manual = step >= stepModel && !m.loading && i < 0 && (cur != "" || len(m.models) == 0)
	return m
}

func (m setupModel) Init() tea.Cmd { return nil }

func (m setupModel) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch v := msg.(type) {
	case modelsFetched:
		m.loading, m.models, m.note = false, v.list, ""
		if v.err != nil {
			m.note = "拉不到模型清单（" + v.err.Error() + "），请手动输入"
		}
		return m.enter(m.step), nil
	case setupSaved:
		if v.err != nil {
			m.note = v.err.Error()
			return m, nil
		}
		return m, tea.Quit
	case tea.KeyMsg:
		return m.key(v)
	}
	return m, nil
}

func (m setupModel) key(v tea.KeyMsg) (setupModel, tea.Cmd) {
	switch v.String() {
	case "ctrl+c":
		return m, tea.Quit
	case "esc":
		if m.step == stepProtocol {
			return m, tea.Quit
		}
		m.loading, m.note = false, ""
		return m.enter(m.step - 1), nil
	case "enter":
		if m.loading {
			return m, nil
		}
		return m.confirm()
	}
	if items := m.choices(); items != nil {
		switch v.String() {
		case "up":
			m.sel = wrapIndex(m.sel-1, len(items))
		case "down":
			m.sel = wrapIndex(m.sel+1, len(items))
		}
		return m, nil
	}
	ed := &m.eds[m.step]
	switch {
	case v.String() == "backspace":
		ed.backspace()
	case v.String() == "ctrl+u":
		ed.clear()
	case v.Type == tea.KeyRunes:
		ed.insert(strings.TrimSpace(string(v.Runes))) // 粘贴进来的换行不要
	}
	return m, nil
}

// confirm 收下本页、去下一页；api_key 之后拉模型清单，最后一页保存
func (m setupModel) confirm() (setupModel, tea.Cmd) {
	if items := m.choices(); items != nil {
		if items[m.sel] == manualItem {
			m.manual = true
			return m, nil
		}
		m.eds[m.step].set(items[m.sel])
	}
	m.note = ""
	if (m.step == stepBaseURL || m.step == stepModel) && strings.TrimSpace(m.eds[m.step].value()) == "" {
		m.note = setupSteps[m.step].name + " 不能为空"
		return m, nil
	}
	c, s := m.client, m.values()
	switch m.step {
	case stepAPIKey:
		m.loading, m.models = true, nil
		return m.enter(stepModel), func() tea.Msg {
			list, err := c.FetchModels(s)
			return modelsFetched{list: list, err: err}
		}
	case stepSmallModel:
		return m, func() tea.Msg { return setupSaved{err: c.ApplySetup(s)} }
	}
	return m.enter(m.step + 1), nil
}

func (m setupModel) View() string {
	st := setupSteps[m.step]
	rows := []string{panelTitleStyle.Render(fmt.Sprintf("(%d/%d) %s", m.step+1, len(setupSteps), st.name))}
	switch items := m.choices(); {
	case m.loading:
		rows = append(rows, dimStyle.Render("  正在拉模型清单…"))
	case items != nil:
		lo, hi := window(len(items), m.sel, panelMaxRows)
		for i := lo; i < hi; i++ {
			if i == m.sel {
				rows = append(rows, menuSelStyle.Render("▸ "+items[i]))
			} else {
				rows = append(rows, menuStyle.Render("  "+items[i]))
			}
		}
	default:
		rows = append(rows, userStyle.Render("> ")+m.eds[m.step].display())
	}
	rows = append(rows, dimStyle.Render("  "+st.hint))
	if m.note != "" {
		rows = append(rows, errorStyle.Render("  ✗ "+m.note))
	}
	return strings.Join(append(rows, dimStyle.Render("  ↑/↓ 选择 · Enter 确认 · Esc 返回")), "\n") + "\n"
}

// runSetup 与主界面一样走 alternate screen
func runSetup(c *client.Client, s client.Setup) {
	if _, err := tea.NewProgram(newSetupModel(c, s), tea.WithAltScreen()).Run(); err != nil {
		fmt.Println("引导运行失败:", err)
	}
}
