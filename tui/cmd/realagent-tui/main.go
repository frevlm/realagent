// realagent-tui — 终端客户端（Bubble Tea，alternate screen，ADR-0020）
//
// POST /message 提交消息，回复与审批都走 /events 推送流。屏幕：历史进 viewport（可滚），
// 底下是审批框 + 子面板 + 斜杠菜单 + 读秒行 + 输入框 + 状态栏。
// 只留当前 agent 的行流；切 agent 就丢掉、从 GET /session 重新读。
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/viewport"
	"github.com/charmbracelet/bubbletea"
	"realagent/tui/internal/client"
)

// ==================== 事件订阅（推送流 → tea.Msg） ====================

type eventsReady struct{ ch <-chan client.Event }
type eventMsg client.Event
type eventsDone struct{}

// commandsMsg 携带 GET /commands 的拉取结果（斜杠菜单数据源）
type commandsMsg struct {
	cmds []client.Command
	err  error
}

// 启动事件订阅：Init 返回此 cmd，goroutine 持续读推送流
func subscribeCmd(c *client.Client) tea.Cmd {
	return func() tea.Msg {
		ch := make(chan client.Event)
		go c.SubscribeEvents(ch)
		return eventsReady{ch: ch}
	}
}

// 等待下一条事件（Bubble Tea 循环）
func waitEventCmd(ch <-chan client.Event) tea.Cmd {
	return func() tea.Msg {
		ev, ok := <-ch
		if !ok {
			return eventsDone{}
		}
		return eventMsg(ev)
	}
}

// 拉取斜杠命令列表（失败就没有菜单）。plugin 命令跟着 agent 的工作目录走。
func fetchCommandsCmd(c *client.Client, agentID int) tea.Cmd {
	return func() tea.Msg {
		cmds, err := c.FetchCommands(agentID)
		return commandsMsg{cmds: cmds, err: err}
	}
}

// ==================== Bubble Tea 模型 ====================

// pendingApproval 是挂起的审批请求（ADR-0005）
type pendingApproval struct {
	id     string // permission_request / approval-response 关联 ID
	tool   string
	params string // 工具参数（紧凑 JSON，仅展示）
}

// 斜杠菜单最多同时显示的条目数（超出按高亮项开窗）
const menuMaxRows = 8

type model struct {
	client *client.Client
	// 当前 agent 的行流（原始文本，不含 ANSI）。m.open 时末行仍在增长。
	// 它是盘上会话的副本，切 agent 时丢掉重读。
	lines     []line
	open      bool // 末行是否还在流式增长
	vp        viewport.Model
	eventsCh  <-chan client.Event
	ed        editor // 输入行编辑器
	width     int
	height    int
	approval  *pendingApproval // 非 nil = 审批模态（忽略除 y/n 外按键）
	commands  []client.Command // 斜杠命令列表（GET /commands，启动时拉取）
	menuSel   int              // 斜杠菜单高亮项（menuMatches 索引）
	menuHid   bool             // esc 收起菜单（下次编辑输入即复原）
	panel     *panel           // 非 nil = 子面板模态（panel.go）：↑/↓ 选择，Enter 确认，Esc 取消
	panelWant string           // 期待用哪条命令的结果开面板（panelWantOf 算出，空 = 不开）
	awaiting  bool             // 已 POST /message 但推送流还没吐出任何内容
	busy      activity         // 读秒状态行（status.go）：模型在干什么 + 已耗时
	sl        statusline       // 状态栏（statusline.go）：model | directory | git
}

func initialModel(c *client.Client) model {
	m := model{client: c, sl: newStatusline()}
	m.vp = viewport.New(0, 0)
	m.emit("info", "连接 core (QUIC/HTTP3) — Enter 发送，Alt+Enter 换行，Esc 中断，Ctrl+C 退出")
	return m
}

// sendMsg 携带 POST /message 的兜底结果（正常回复走事件流）
type sendMsg struct {
	reply client.Reply
	err   error
}

func sendCmd(c *client.Client, input string) tea.Cmd {
	return func() tea.Msg {
		r, err := c.Send(input)
		return sendMsg{reply: r, err: err}
	}
}

// historyMsg 携带 GET /session 的回放帧
type historyMsg struct {
	agentID int
	frames  []client.Frame
	err     error
}

// fetchHistoryCmd 拉一个 agent 的历史，带回 agentID：回来时用户可能已经切走了
func fetchHistoryCmd(c *client.Client, agentID int) tea.Cmd {
	return func() tea.Msg {
		f, err := c.FetchSession(agentID)
		return historyMsg{agentID: agentID, frames: f, err: err}
	}
}

// agentsMsg 携带 GET /agents 的清单
type agentsMsg struct {
	list []client.AgentInfo
	err  error
}

func fetchAgentsCmd(c *client.Client) tea.Cmd {
	return func() tea.Msg {
		l, err := c.FetchAgents()
		return agentsMsg{list: l, err: err}
	}
}

// interruptMsg 携带 POST /interrupt 的结果
type interruptMsg struct {
	err error
}

func interruptCmd(c *client.Client) tea.Cmd {
	return func() tea.Msg {
		return interruptMsg{err: c.Interrupt()}
	}
}

// approvalResp 携带 POST /approval-response 的回传结果
type approvalResp struct {
	err error
}

func approvalCmd(c *client.Client, id string, allow bool) tea.Cmd {
	return func() tea.Msg {
		return approvalResp{err: c.RespondApproval(id, allow)}
	}
}

// ==================== Bubble Tea 接口 ====================

func (m model) Init() tea.Cmd {
	return tea.Batch(subscribeCmd(m.client), fetchCommandsCmd(m.client, m.client.AgentID()), fetchStatusCmd(m.client),
		fetchHistoryCmd(m.client, m.client.AgentID()))
}

// Update 先跑业务，再把行流铺进 viewport。别处只管往 m.lines 追加。
func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	next, cmd := m.update(msg)
	m = next
	return m.sync(), cmd
}

// sync 每帧全量重铺：折行是纯函数，增量维护要多一份会跟丢的状态。
// 原本贴着底就继续贴底；用户滚上去了就不拽回来。
func (m model) sync() model {
	width := m.viewWidth()
	h := m.height - len(m.chrome(width))
	if h < 1 {
		h = 1
	}
	atBottom := m.vp.AtBottom()
	m.vp.Width, m.vp.Height = width, h
	var rows []string
	for _, l := range m.lines {
		rows = append(rows, render(l, width)...)
	}
	// 不足一屏时在上方补空行：头几句贴着输入框
	if pad := h - len(rows); pad > 0 {
		rows = append(make([]string, pad), rows...)
	}
	m.vp.SetContent(strings.Join(rows, "\n"))
	if atBottom {
		m.vp.GotoBottom()
	}
	return m
}

// viewWidth 是折行用的宽度。尺寸还没到就按常规宽度画，下一帧校准
func (m model) viewWidth() int {
	if m.width <= 0 {
		return 80
	}
	return m.width
}

func (m model) update(msg tea.Msg) (model, tea.Cmd) {
	switch v := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = v.Width
		m.height = v.Height
		return m, nil

	case eventsReady:
		m.eventsCh = v.ch
		return m, waitEventCmd(m.eventsCh)

	case eventsDone:
		m.busy.stop()
		return m, subscribeCmd(m.client) // 断线重连

	case tickMsg:
		return m, m.busy.tick(v)

	case commandsMsg:
		if v.err == nil {
			m.commands = v.cmds
		}
		return m, nil

	case statusMsg:
		m.sl.model = v.model
		return m, nil

	case gitMsg:
		m.sl.branch, m.sl.gitMark = v.branch, v.mark
		return m, nil

	case eventMsg:
		cmd := m.handleEvent(client.Event(v))
		return m, tea.Batch(cmd, waitEventCmd(m.eventsCh))

	case tea.KeyMsg:
		return m.handleKey(v)

	case sendMsg:
		// 普通消息回 processing，回复走推送流；这里只处理失败与斜杠命令结果
		if !m.awaiting {
			return m, nil
		}
		switch {
		case v.err != nil:
			m.emit("error", "发送失败: "+v.err.Error())
			m.awaiting = false
			m.busy.stop()
		case v.reply.Error != "":
			m.emit("error", v.reply.Error)
			m.awaiting = false
			m.busy.stop()
		case v.reply.Ok:
			m.awaiting = false
			m.busy.stop() // 命令不启动 agent turn，收到结果即收工
			// 无参的 /model /resume 是要选一个：做成子面板，造不出才退回文本
			if m.panelWant == v.reply.Command {
				if p := makePanel(v.reply.Command, v.reply.Data, m.client.AgentID()); p != nil {
					m.panel = p
					return m, nil
				}
			}
			text := describeCommand(v.reply.Command)
			switch v.reply.Command {
			case "model":
				text = renderModels(v.reply.Data)
			case "new", "resume":
				text = renderSessions(v.reply.Command, v.reply.Data, m.client.AgentID())
			case "plugins":
				text = renderPlugins(v.reply.Data)
			}
			m.emit("info", text)
		}
		return m, nil

	case historyMsg:
		// 用户已经切走了：丢掉
		if v.agentID != m.client.AgentID() {
			return m, nil
		}
		if v.err != nil {
			m.emit("error", "读历史失败: "+v.err.Error())
			return m, nil
		}
		// 回放帧与实时帧同形，走同一个渲染器
		for _, f := range v.frames {
			m.handleEvent(client.Event{Type: f.Type, Payload: string(f.Data)})
		}
		m.closeLine()
		m.busy.stop() // 别让历史里的 turn_start 点着读秒行
		return m, nil

	case agentsMsg:
		if v.err != nil {
			m.emit("error", "取 agent 清单失败: "+v.err.Error())
			return m, nil
		}
		m.panel = agentPanel(v.list, m.client.AgentID())
		return m, nil

	case interruptMsg:
		if v.err != nil {
			m.emit("error", "中断请求失败: "+v.err.Error())
		}
		return m, nil

	case approvalResp:
		if v.err != nil {
			m.emit("error", "审批回传失败: "+v.err.Error())
		}
		return m, nil
	}
	return m, nil
}

// ==================== 按键 ====================

// handleKey 处理按键。审批模态下只响应 y/n（a/d）与 ctrl+c，其余忽略。
// 子面板模态下只响应 ↑/↓、Enter、Esc 与 ctrl+c。
// 斜杠菜单（输入以 '/' 开头）时：↑/↓ 移动高亮、Tab 补全、Enter 执行、Esc 收起。
func (m model) handleKey(v tea.KeyMsg) (model, tea.Cmd) {
	if m.approval != nil {
		switch v.String() {
		case "ctrl+c":
			return m, tea.Quit
		case "y", "a":
			return m.decideApproval(true)
		case "n", "d", "esc":
			return m.decideApproval(false)
		}
		return m, nil
	}

	if m.panel != nil {
		return m.panelKey(v.String())
	}

	// 粘贴：整块原样插入（含换行），不当按键解释
	if v.Paste {
		m.ed.insert(string(v.Runes))
		m.menuHid = false
		return m, nil
	}

	switch v.String() {
	case "ctrl+c":
		return m, tea.Quit

	case "esc":
		switch {
		case len(m.menuMatches()) > 0:
			m.menuHid = true // 只收菜单，不清输入
		case m.busy.active:
			return m, interruptCmd(m.client)
		}
		return m, nil

	case "enter":
		if matches := m.menuMatches(); len(matches) > 0 {
			m.ed.set("/" + matches[m.menuIndex(matches)].Name) // 菜单执行 = 补全 + 发送
		}
		return m.submitInput(false)

	case "alt+enter", "ctrl+j":
		m.ed.insert("\n")

	case "tab":
		return m.menuNav("tab")

	case "up", "down":
		// 菜单开着归菜单，否则滚动。不开 mouse mode（会关掉终端的选中复制），
		// 终端的 alternate scroll 会把滚轮转成方向键送到这里
		if len(m.menuMatches()) > 0 {
			return m.menuNav(v.String())
		}
		if v.String() == "up" {
			m.vp.LineUp(3)
		} else {
			m.vp.LineDown(3)
		}
		return m, nil

	case "pgup":
		m.vp.ViewUp()
		return m, nil

	case "pgdown":
		m.vp.ViewDown()
		return m, nil

	case "backspace":
		m.ed.backspace()
	case "delete":
		m.ed.del()
	case "left", "ctrl+b":
		m.ed.left()
	case "right", "ctrl+f":
		m.ed.right()
	case "home", "ctrl+a":
		m.ed.home()
	case "end", "ctrl+e":
		m.ed.end()
	case "ctrl+k":
		m.ed.killToEnd()
	case "ctrl+u":
		m.ed.killToStart()
	case "ctrl+w":
		m.ed.killWord()

	default:
		if v.Type == tea.KeyRunes && len(v.Runes) > 0 {
			m.ed.insert(string(v.Runes))
		} else if v.Type == tea.KeySpace {
			m.ed.insert(" ")
		}
	}
	m.menuHid = false // 任何编辑都让菜单复原
	return m, nil
}

// menuOpen 判断斜杠菜单是否激活（输入以 '/' 开头且未被 esc 收起）
func (m model) menuOpen() bool {
	return !m.menuHid && strings.HasPrefix(m.ed.value(), "/")
}

// localCmds 是不经 core 的斜杠命令：合进菜单，在 submitInput 里就地处理。
var localCmds = []client.Command{
	statuslineCmd, // statusline.go
	{Name: "agents", Description: "切到本组的另一个 agent（无参 = 列出来选）"},
	{Name: "quit", Description: "退出 TUI（core 继续在后台跑）"},
}

// menuMatches 返回当前输入前缀命中的命令（命令名不带 '/'，前缀匹配）。
// 输入含空格视为进入参数模式（v1 命令无参数），关闭菜单。
func (m model) menuMatches() []client.Command {
	if !m.menuOpen() {
		return nil
	}
	prefix := m.ed.value()[1:]
	if strings.ContainsAny(prefix, " \n") {
		return nil
	}
	all := make([]client.Command, 0, len(m.commands)+len(localCmds))
	all = append(all, m.commands...)
	all = append(all, localCmds...) // 本地命令，不经 core（见 submitInput）

	var out []client.Command
	for _, c := range all {
		if strings.HasPrefix(c.Name, prefix) {
			out = append(out, c)
		}
	}
	return out
}

// menuIndex 返回高亮项在 matches 中的索引（越界归 0）
func (m model) menuIndex(matches []client.Command) int {
	if len(matches) == 0 || m.menuSel < 0 || m.menuSel >= len(matches) {
		return 0
	}
	return m.menuSel
}

// menuNav 处理菜单方向键：↑/↓ 移动高亮（循环），Tab 补全当前高亮项到输入框
func (m model) menuNav(key string) (model, tea.Cmd) {
	matches := m.menuMatches()
	if len(matches) == 0 {
		return m, nil
	}
	switch key {
	case "up":
		m.menuSel = wrapIndex(m.menuIndex(matches)-1, len(matches))
	case "down":
		m.menuSel = wrapIndex(m.menuIndex(matches)+1, len(matches))
	case "tab":
		m.menuSel = m.menuIndex(matches)
		m.ed.set("/" + matches[m.menuSel].Name) // 补全：输入框写为完整命令
	}
	return m, nil
}

// panelKey：↑/↓（Tab/Shift+Tab）移动，Enter 确认（替用户打全命令再走 submitInput），Esc 取消。
func (m model) panelKey(key string) (model, tea.Cmd) {
	p := m.panel
	switch key {
	case "ctrl+c":
		return m, tea.Quit
	case "esc":
		m.panel = nil
		m.panelWant = ""
	case "up", "shift+tab":
		p.sel = wrapIndex(p.sel-1, len(p.items))
	case "down", "tab":
		p.sel = wrapIndex(p.sel+1, len(p.items))
	case "enter":
		item := p.items[p.sel]
		m.panel = nil
		m.ed.set(item.submit)
		return m.submitInput(true)
	}
	return m, nil
}

// submitInput 发送当前输入（普通消息或斜杠命令），清空输入框。
// fromPanel = 这条输入是子面板确认出来的（决定结果回来后面板要不要接着开，见 panelWantOf）。
func (m model) submitInput(fromPanel bool) (model, tea.Cmd) {
	input := strings.TrimSpace(m.ed.value())
	if input == "" {
		return m, nil
	}
	m.ed.clear()
	m.menuSel = 0
	m.panelWant = panelWantOf(input, fromPanel)
	// 只回显斜杠命令：普通消息由 core 的 message_start 帧画
	if strings.HasPrefix(input, "/") {
		m.emit("user", input)
	}

	// 以下三条是纯客户端命令
	if cmd, _ := splitCommand(input); cmd == "/quit" {
		return m, tea.Quit
	}

	if cmd, rest := splitCommand(input); cmd == "/statusline" {
		if rest == "" {
			m.panel = m.sl.panel() // 无参 = 开面板选，面板本身就是配置一览
			return m, nil
		}
		var msg string
		m.sl, msg = m.sl.applyStatuslineCmd(rest)
		m.emit("info", msg)
		if m.panelWant == "statusline" {
			m.panel = m.sl.panel() // 面板里改的，改完还留在面板里接着改
		}
		return m, nil
	}

	// /agents：无参 = 开面板选，带 id = 直接切
	if cmd, rest := splitCommand(input); cmd == "/agents" {
		if rest == "" {
			return m, fetchAgentsCmd(m.client)
		}
		var id int
		_, _ = fmt.Sscanf(rest, "%d", &id)
		return m.attach(id)
	}

	m.awaiting = true
	// 读秒从按下 Enter 起算
	return m, tea.Batch(sendCmd(m.client, input), m.busy.begin("发送中", time.Now()))
}

// attach 切到另一个 agent：丢掉当前行流，从 GET /session 重读。
func (m model) attach(id int) (model, tea.Cmd) {
	if id == m.client.AgentID() {
		return m, nil
	}
	m.client.Attach(id)
	m.lines = nil
	m.open = false
	m.busy.stop()
	m.awaiting = false
	m.emit("info", fmt.Sprintf("⇄ 切到 agent %d", id))
	return m, fetchHistoryCmd(m.client, id)
}

// decideApproval 处理审批裁决：记录结果 → 退出审批模态 → 回传 core
func (m model) decideApproval(allow bool) (model, tea.Cmd) {
	p := m.approval
	if p == nil {
		return m, nil
	}
	verdict := "已拒绝"
	if allow {
		verdict = "已允许"
	}
	m.emit("info", fmt.Sprintf("🔐 %s工具 %s", verdict, p.tool))
	m.approval = nil
	return m, tea.Batch(approvalCmd(m.client, p.id, allow), m.busy.begin("执行工具", time.Now()))
}

// ==================== 事件 ====================

// handleEvent 处理推送流事件，返回需要执行的 tea.Cmd（读秒计时循环的启动）
func (m *model) handleEvent(ev client.Event) tea.Cmd {
	// core 全推，每帧带 agent_id，这里只留当前 agent 的。审批例外：不管在看谁都要弹。
	// 没有 agent_id 的帧（statusline）是进程级的。
	var who struct {
		AgentID int `json:"agent_id"`
	}
	jsonUnmarshal(ev.Payload, &who)
	if ev.Type != "permission_request" && who.AgentID != 0 && who.AgentID != m.client.AgentID() {
		return nil
	}

	switch ev.Type {
	case "turn_start":
		return m.busy.begin("思考中", time.Now())

	case "message_start":
		// 收件箱三种来源都是 user；用户自己打的那条也从这里画
		var d struct {
			Text string `json:"text"`
		}
		jsonUnmarshal(ev.Payload, &d)
		if d.Text != "" {
			m.emit("user", d.Text)
		}

	case "message_update":
		m.awaiting = false
		m.stream("assistant", deltaOf(ev.Payload))
		return m.busy.begin("生成回复", time.Now())

	case "thinking_start":
		return m.busy.begin("思考中", time.Now())

	case "thinking_update":
		m.awaiting = false
		// 思考块头部只在真有内容时冒出来（空的 thinking_start 不留痕）
		if !m.open || len(m.lines) == 0 || m.lines[len(m.lines)-1].role != "thinking" {
			m.emit("info", "💭 思考过程")
		}
		m.stream("thinking", deltaOf(ev.Payload))
		return m.busy.begin("思考中", time.Now())

	case "thinking_stop":
		m.closeLine()

	case "statusline":
		// 与 GET /statusline 同一份载荷，覆盖写
		var d client.Statusline
		jsonUnmarshal(ev.Payload, &d)
		m.sl.model = d.Model

	case "status_update":
		// 本次 run 累计的绝对值，覆盖写
		jsonUnmarshal(ev.Payload, &m.busy.cost)

	case "tool_execution_start":
		var d struct {
			Name string `json:"name"`
		}
		jsonUnmarshal(ev.Payload, &d)
		m.awaiting = false
		m.emit("tool", "🔧 "+d.Name+" …")
		return m.busy.begin(toolVerb(d.Name), time.Now())

	case "tool_output":
		// 工具边跑边推的输出。走 stream（续写开着的行）：末行可能没有换行符
		var d struct {
			Text string `json:"text"`
		}
		jsonUnmarshal(ev.Payload, &d)
		m.awaiting = false
		m.stream("output", d.Text)

	case "tool_execution_end":
		var d struct {
			Name   string `json:"name"`
			Status int    `json:"status"`
		}
		jsonUnmarshal(ev.Payload, &d)
		mark := "✓"
		if d.Status != 0 {
			mark = "✗"
		}
		m.emit("tool", "   "+mark+" "+d.Name)
		return m.busy.begin("思考中", time.Now()) // 工具完事，等下一轮 LLM

	case "permission_request":
		// agent 阻塞等裁决，进审批模态
		var d struct {
			ID     string          `json:"id"`
			Tool   string          `json:"tool"`
			Params json.RawMessage `json:"params"`
		}
		jsonUnmarshal(ev.Payload, &d)
		m.approval = &pendingApproval{id: d.ID, tool: d.Tool, params: string(d.Params)}
		return m.busy.begin("等待你的审批", time.Now())

	case "interrupted":
		m.emit("info", "已中断")
		m.awaiting = false
		m.busy.stop()
		m.approval = nil

	case "turn_end":
		// turn 结束不是收工（还要过收工判定），读秒只认 agent_end
		m.closeLine()
		var d struct {
			Error string `json:"error"`
		}
		jsonUnmarshal(ev.Payload, &d)
		// core 报的失败落到对话流里
		if d.Error != "" {
			m.emit("error", "✗ "+d.Error)
		}

	case "agent_end":
		// 唯一的收工信号
		m.closeLine()
		m.awaiting = false
		m.busy.stop()
		return fetchGitCmd // agent 可能改了文件，重取 git 标记

	case "message_end":
		m.closeLine()
	}
	return nil
}

// deltaOf 取事件载荷里的 delta 字段（message_update / thinking_update 同构）
func deltaOf(payload string) string {
	var d struct {
		Delta string `json:"delta"`
	}
	jsonUnmarshal(payload, &d)
	return d.Delta
}

func jsonUnmarshal(s string, v any) {
	_ = json.Unmarshal([]byte(s), v)
}

// ==================== 渲染 ====================

// chrome 是屏幕底下那块。viewport 高度 = 屏幕减去它，所以高度只在这里算。
func (m model) chrome(width int) []string {
	// 历史与交互区之间留一行空白
	rows := []string{""}
	if m.approval != nil {
		rows = append(rows, renderApproval(m.approval, width)...)
	}
	if m.panel != nil {
		rows = append(rows, renderPanel(m.panel, width)...)
	}
	if matches := m.menuMatches(); len(matches) > 0 {
		rows = append(rows, renderMenu(matches, m.menuIndex(matches), width)...)
	}
	if status := m.busy.render(width); status != "" {
		rows = append(rows, status)
	}
	rows = append(rows, m.ed.view(width)...)
	if status := m.sl.render(); status != "" {
		rows = append(rows, status)
	}
	return rows
}

// View 画整个屏幕：历史（viewport）+ 底下那块。
func (m model) View() string {
	width := m.viewWidth()
	return m.vp.View() + "\n" + strings.Join(m.chrome(width), "\n")
}

// renderMenu 渲染斜杠命令菜单（输入行上方）：▸ 高亮当前项，条目多时按高亮开窗
func renderMenu(cmds []client.Command, sel, width int) []string {
	lo, hi := window(len(cmds), sel, menuMaxRows)
	var out []string
	for i := lo; i < hi; i++ {
		text := "/" + cmds[i].Name
		if cmds[i].Description != "" {
			text += "  " + cmds[i].Description
		}
		text = fit(text, width-2) // 菜单一条一行，宽了就截
		if i == sel {
			out = append(out, menuSelStyle.Render("▸ "+text))
		} else {
			out = append(out, menuStyle.Render("  "+text))
		}
	}
	return out
}

// describeCommand 是没有专门渲染器（或载荷解不开）时的通用结果行
func describeCommand(name string) string {
	return "✅ 命令已执行: /" + name
}

// renderPlugins 把 /plugins 渲染成多行 info 文本，末尾列出建 agent 时的错误。
func renderPlugins(data json.RawMessage) string {
	var p struct {
		Plugins []struct {
			Name        string `json:"name"`
			Description string `json:"description"`
			Version     string `json:"version"`
			Root        string `json:"root"`
			Implicit    bool   `json:"implicit"`
			Skills      int    `json:"skills"`
			Commands    int    `json:"commands"`
			Agents      int    `json:"agents"`
			MCPServers  int    `json:"mcp_servers"`
			Hooks       int    `json:"hooks"`
		} `json:"plugins"`
		Errors []string `json:"errors"`
	}
	if err := json.Unmarshal(data, &p); err != nil {
		return describeCommand("plugins")
	}
	var b strings.Builder
	if len(p.Plugins) == 0 {
		b.WriteString("一个 plugin 都没装。装法：git clone <repo> ~/.realagent/plugins/<name>")
	}
	for _, x := range p.Plugins {
		name := x.Name
		if x.Implicit {
			name += "（本地）"
		}
		if x.Version != "" {
			name += " v" + x.Version
		}
		b.WriteString("📦 " + name + "\n")
		// 只列非零的
		var parts []string
		for _, kv := range []struct {
			n int
			s string
		}{{x.Skills, "skill"}, {x.Commands, "命令"}, {x.Agents, "agent 定义"},
			{x.MCPServers, "MCP server"}, {x.Hooks, "hook"}} {
			if kv.n > 0 {
				parts = append(parts, fmt.Sprintf("%s %d", kv.s, kv.n))
			}
		}
		if len(parts) > 0 {
			b.WriteString("   " + strings.Join(parts, " · ") + "\n")
		}
		if x.Description != "" {
			b.WriteString("   " + x.Description + "\n")
		}
		b.WriteString("   " + x.Root + "\n")
	}
	for _, e := range p.Errors {
		b.WriteString("⚠️  " + e + "\n")
	}
	return strings.TrimRight(b.String(), "\n")
}

// renderSessions 把 /new /resume 的会话清单渲染为多行 info 文本，当前会话打 ▸。
func renderSessions(command string, data json.RawMessage, agentID int) string {
	var list []client.SessionInfo
	if err := json.Unmarshal(data, &list); err != nil {
		return describeCommand(command)
	}
	// 「当前」= 被我这个 agent 打开着的那一条
	var cur client.SessionInfo
	for _, s := range list {
		if s.OpenedBy == agentID {
			cur = s
		}
	}
	if command == "new" {
		return "✅ 已新建会话 " + cur.ID + "（对话历史已清空，旧会话留在盘上）"
	}
	if len(list) == 0 {
		return "📄 还没有任何会话"
	}
	out := []string{fmt.Sprintf("📄 会话 %d 个（当前 %s，共 %d 条消息）", len(list), cur.ID, cur.Messages)}
	for _, s := range list {
		mark := "  "
		if s.OpenedBy == agentID {
			mark = "▸ "
		}
		title := s.Title
		if title == "" {
			title = "（空会话）"
		}
		out = append(out, fmt.Sprintf("%s%s  %s  %d 条", mark, s.ID, title, s.Messages))
	}
	return strings.Join(out, "\n")
}

// renderModels 把 /model 结果（[]ModelInfo JSON）渲染为多行 info 文本。
// 每行：标记 名称 [供应商] 上下文；● 是当前主模型。切换用 /model <name>。
func renderModels(data json.RawMessage) string {
	var list []client.ModelInfo
	if err := json.Unmarshal(data, &list); err != nil {
		return describeCommand("model")
	}
	if len(list) == 0 {
		return "✅ /model: 无模型清单（模型数据表是空的）"
	}
	var out []string
	for _, m := range list {
		mark := "  "
		if m.Current {
			mark = "● "
		}
		text := mark + m.Name
		if m.OwnedBy != "" {
			text += " [" + m.OwnedBy + "]"
		}
		if c := humanContext(m.Context); c != "" {
			text += "  " + c
		}
		out = append(out, text)
	}
	return strings.Join(out, "\n")
}

// humanContext 把上下文窗口折算成人话：1048576 → 1M，131072 → 128k；0（未知）返回空串
func humanContext(n int64) string {
	switch {
	case n <= 0:
		return ""
	case n%(1024*1024) == 0:
		return fmt.Sprintf("%dM", n/(1024*1024))
	case n%1024 == 0:
		return fmt.Sprintf("%dk", n/1024)
	default:
		return fmt.Sprintf("%d", n)
	}
}

// renderApproval 渲染审批对话框（模态，等待 y/n 裁决）
func renderApproval(p *pendingApproval, width int) []string {
	desc := p.tool
	if p.params != "" {
		desc += " " + p.params
	}
	head := fit("🔐 权限请求: "+desc, width) // 按显示宽截，不切碎中文
	return []string{
		approvalStyle.Render(head),
		approvalStyle.Render("   [y] 允许    [n] 拒绝"),
	}
}

func main() {
	addr := "127.0.0.1:12345"
	if len(os.Args) > 1 {
		addr = os.Args[1]
	}
	c := client.New(addr)
	// 退出时通知 core（ADR-0021）
	defer c.CloseGroup()

	// core 不自动建 agent：workdir 由客户端给，它知道用户站在哪
	wd, err := os.Getwd()
	if err != nil {
		fmt.Fprintln(os.Stderr, "取不到当前目录:", err)
		os.Exit(1)
	}
	if err := c.CreateAgent(wd); err != nil {
		fmt.Fprintln(os.Stderr, "连不上 core:", err)
		os.Exit(1)
	}

	// alternate screen 才能「换一个 agent 看」（ADR-0020）。不开鼠标模式：会关掉终端的选中复制
	p := tea.NewProgram(initialModel(c), tea.WithAltScreen())
	if _, err = p.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "TUI 运行失败:", err)
		os.Exit(1)
	}
}
