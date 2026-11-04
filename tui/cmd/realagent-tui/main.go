// realagent-tui — 终端客户端（Bubble Tea，alternate screen，ADR-0020）
//
// POST /message 提交消息，回复与审批都走 /events 推送流。屏幕布局见 view.go。
// 用户看见的只有对话，没有 agent（ADR-0029）：只留当前那段对话的条目，换一段就丢掉、
// 从 GET /session 重新读；它派生出去的子 agent 缩进成一行一条，挂在这段对话里。
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"time"

	tea "charm.land/bubbletea/v2"
	"realagent/tui/internal/client"
)

// ==================== 推送流与后台请求 → tea.Msg ====================

type eventMsg client.Frame
type eventsDone struct{}

// pump 把推送流喂给程序；断了就隔一秒用同一个 clientID 重连
func pump(c *client.Client, p *tea.Program) {
	for {
		ch := make(chan client.Frame)
		go c.SubscribeEvents(ch)
		for f := range ch {
			p.Send(eventMsg(f))
		}
		p.Send(eventsDone{})
		time.Sleep(time.Second)
	}
}

// commandsMsg 携带 GET /commands 的拉取结果（斜杠菜单数据源）
type commandsMsg []client.Command

// failMsg 是一次后台请求的失败原话，落进对话
type failMsg string

// try 跑一次只关心成败的请求：失败就带回 what + 原因
func try(what string, f func() error) tea.Cmd {
	return func() tea.Msg {
		if err := f(); err != nil {
			return failMsg(what + ": " + err.Error())
		}
		return nil
	}
}

// 拉取斜杠命令列表（失败就没有菜单）。plugin 命令跟着工作目录走。
func fetchCommandsCmd(c *client.Client) tea.Cmd {
	return func() tea.Msg {
		cmds, _ := c.FetchCommands()
		return commandsMsg(cmds)
	}
}

// ==================== Bubble Tea 模型 ====================

// pendingApproval 是挂起的审批请求（ADR-0005）
type pendingApproval struct {
	id     string // permission_request / approval-response 关联 ID
	tool   string
	params string // 工具参数（紧凑 JSON，仅展示）
	from   string // 不是当前对话本身在问时说明是谁：子任务 / 另一段对话
}

// 斜杠菜单最多同时显示的条目数（超出按高亮项开窗）
const menuMaxRows = 8

type model struct {
	client    *client.Client
	tr        transcript // 当前那段对话（transcript.go）
	scroll    scroll     // 历史区停在哪（render.go）
	expand    bool       // 工具输出全部展开（ctrl+o）
	pace      pacer      // 推送流的节拍器（pace.go）
	ed        editor     // 输入框（input.go）
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
	sl, err := newStatusline()
	m := model{client: c, sl: sl}
	m.emit(kindInfo, "Enter 发送，Alt+Enter 换行，Esc 中断，Ctrl+O 展开工具输出，Ctrl+C 退出")
	if err != nil {
		m.emit(kindError, err.Error())
	}
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

// historyMsg 携带 GET /session 的回放帧，连同是哪段对话的：回来时用户可能已经换走了
type historyMsg struct {
	sessionID string
	frames    []client.Frame
}

func fetchHistoryCmd(c *client.Client, sessionID string) tea.Cmd {
	return func() tea.Msg {
		f, err := c.FetchSession(sessionID)
		if err != nil {
			return failMsg("读历史失败: " + err.Error())
		}
		return historyMsg{sessionID, f}
	}
}

// sessionsMsg 携带 GET /sessions 的对话清单
type sessionsMsg []client.SessionInfo

func fetchSessionsCmd(c *client.Client) tea.Cmd {
	return func() tea.Msg {
		l, err := c.FetchSessions()
		if err != nil {
			return failMsg("取对话清单失败: " + err.Error())
		}
		return sessionsMsg(l)
	}
}

// ==================== Bubble Tea 接口 ====================

func (m model) Init() tea.Cmd {
	// 起步是一段新对话：还没说过话，没有历史可读
	return tea.Batch(fetchCommandsCmd(m.client), fetchStatusCmd(m.client), fetchGitCmd())
}

// Update 只跑业务，别处只管往对话里追加；屏幕在 View 里按需取。
func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	next, cmd := m.update(msg)
	watch := next.watchStatusline()
	return next, tea.Batch(cmd, watch)
}

func (m model) update(msg tea.Msg) (model, tea.Cmd) {
	switch v := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = v.Width
		m.height = v.Height
		return m, nil

	case eventsDone: // 断线：pump 在重连
		m.busy.stop()
		return m, nil

	case tickMsg:
		return m, m.busy.tick(v)

	case commandsMsg:
		m.commands = v
		return m, nil

	case gitMsg:
		m.sl.git = v.info
		return m, nil

	case extOutput:
		if v.gen == m.sl.ext.gen {
			m.sl.ext.lines = v.lines
		}
		return m, nil

	case failMsg:
		m.emit(kindError, string(v))
		return m, nil

	case eventMsg:
		return m, m.enqueue(client.Frame(v), time.Now())

	case paceMsg:
		return m, m.onPace(time.Time(v))

	case tea.KeyPressMsg:
		return m.handleKey(v)

	case tea.PasteMsg:
		// 粘贴整块原样插入（含换行），不当按键解释；模态开着时不收
		if m.approval == nil && m.panel == nil {
			m.ed.insert(v.Content)
			m.menuHid = false
		}
		return m, nil

	case sendMsg:
		// 普通消息回 processing，回复走推送流；这里只收失败与斜杠命令的结果，收到即收工
		if !m.awaiting || v.err == nil && v.reply.Error == "" && !v.reply.Ok {
			return m, nil
		}
		m.awaiting = false
		m.busy.stop()
		switch {
		case v.err != nil:
			m.emit(kindError, "发送失败: "+v.err.Error())
		case v.reply.Error != "":
			m.emit(kindError, v.reply.Error)
		default:
			// 无参的 /model 是要选一个：做成子面板，造不出才退回文本
			if m.panelWant == v.reply.Command {
				if p := modelPanel(v.reply.Data); p != nil {
					m.panel = p
					return m, nil
				}
			}
			text := describeCommand(v.reply.Command)
			switch v.reply.Command {
			case "model":
				text = renderModels(v.reply.Data)
			case "plugins":
				text = renderPlugins(v.reply.Data)
			}
			m.emit(kindInfo, text)
		}
		return m, nil

	case historyMsg:
		// 用户已经换走了：丢掉
		if v.sessionID != m.client.SessionID() {
			return m, nil
		}
		// 回放帧与实时帧同形，走同一段代码
		for _, f := range v.frames {
			m.handleEvent(f)
		}
		m.tr.close()
		m.busy.stop() // 别让历史里的 turn_start 点着读秒行
		return m, nil

	case sessionsMsg:
		if m.panel = sessionPanel(v, m.client.SessionID()); m.panel == nil {
			m.emit(kindInfo, "📄 这个目录下还没有对话")
		}
		return m, nil
	}
	return m, nil
}

// ==================== 按键 ====================

// handleKey 处理按键。审批模态下只响应 y/n（a/d）与 ctrl+c，其余忽略。
// 子面板模态下只响应 ↑/↓、Enter、Esc 与 ctrl+c。
// 斜杠菜单（输入以 '/' 开头）时：↑/↓ 移动高亮、Tab 补全、Enter 执行、Esc 收起。
func (m model) handleKey(v tea.KeyPressMsg) (model, tea.Cmd) {
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

	switch v.String() {
	case "ctrl+c":
		return m, tea.Quit

	case "esc":
		switch {
		case len(m.menuMatches()) > 0:
			m.menuHid = true // 只收菜单，不清输入
		case m.busy.active:
			return m, tea.Batch(m.flush(), try("中断请求失败", m.client.Interrupt))
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

	case "ctrl+o":
		m.expand = !m.expand
		return m, nil

	case "up", "down":
		// 菜单开着归菜单，否则滚动。不开 mouse mode（会关掉终端的选中复制），
		// 终端的 alternate scroll 会把滚轮转成方向键送到这里
		if len(m.menuMatches()) > 0 {
			return m.menuNav(v.String())
		}
		n := 3
		if v.String() == "up" {
			n = -3
		}
		m.scrollBy(n)
		return m, nil

	case "pgup":
		m.scrollBy(-m.histHeight())
		return m, nil

	case "pgdown":
		m.scrollBy(m.histHeight())
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
		m.ed.insert(v.Text) // 没有字面的组合键 Text 为空，插入即无事
	}
	m.menuHid = false // 任何编辑都让菜单复原
	return m, nil
}

// localCmds 是不经 core 的斜杠命令：合进菜单，在 submitInput 里就地处理。
var localCmds = []client.Command{
	statuslineCmd, // statusline.go
	{Name: "new", Description: "开一段新对话（当前这段留在盘上）"},
	{Name: "resume", Description: "换到另一段对话（无参 = 列出来选）"},
	{Name: "quit", Description: "退出 TUI（core 继续在后台跑）"},
}

// menuMatches 返回当前输入前缀命中的命令（命令名不带 '/'，前缀匹配）。
// 菜单只在输入以 '/' 开头、没被 esc 收起时开；输入含空白视为进入参数，关闭菜单。
func (m model) menuMatches() []client.Command {
	prefix, ok := strings.CutPrefix(m.ed.value(), "/")
	if m.menuHid || !ok || strings.ContainsAny(prefix, " \n") {
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
		if item.edit {
			return m, nil
		}
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
		m.emit(kindUser, input)
	}

	// 以下几条是纯客户端命令。换对话不需要 core 做任何事：下一条消息带哪个 session_id 而已
	switch cmd, rest := splitCommand(input); {
	case cmd == "/quit":
		return m, tea.Quit
	case cmd == "/statusline" && rest == "":
		m.panel = m.sl.panel() // 无参 = 开面板选，面板本身就是配置一览
		return m, nil
	case cmd == "/statusline":
		var msg string
		m.sl, msg = m.sl.applyStatuslineCmd(rest)
		m.emit(kindInfo, msg)
		if m.panelWant == "statusline" {
			m.panel = m.sl.panel() // 面板里改的，改完还留在面板里接着改
		}
		return m, nil
	case cmd == "/new":
		m.client.Fresh()
		m.reset()
		m.emit(kindInfo, "✨ 新对话")
		return m, nil
	case cmd == "/resume" && rest == "":
		return m, fetchSessionsCmd(m.client)
	case cmd == "/resume":
		return m.switchTo(rest)
	}

	m.awaiting = true
	// 读秒从按下 Enter 起算
	return m, tea.Batch(sendCmd(m.client, input), m.busy.begin("发送中", time.Now()))
}

// switchTo 换一段对话：丢掉当前条目，从 GET /session 重读。
// 原来那段在 core 里照跑，回来时从盘上读得到。
func (m model) switchTo(sessionID string) (model, tea.Cmd) {
	if sessionID == m.client.SessionID() {
		return m, nil
	}
	m.client.Use(sessionID)
	m.reset()
	return m, fetchHistoryCmd(m.client, sessionID)
}

// reset 丢掉当前对话的条目与读秒
func (m *model) reset() {
	m.tr = transcript{}
	m.scroll = scroll{}
	m.sl.forget()
	m.busy.stop()
	m.awaiting = false
}

// decideApproval 处理审批裁决：记录结果 → 退出审批模态 → 回传 core
func (m model) decideApproval(allow bool) (model, tea.Cmd) {
	p := m.approval
	verdict := "已拒绝"
	if allow {
		verdict = "已允许"
	}
	m.emit(kindInfo, fmt.Sprintf("🔐 %s工具 %s", verdict, p.tool))
	m.approval = nil
	respond := func() error { return m.client.RespondApproval(p.id, allow) }
	return m, tea.Batch(try("审批回传失败", respond), m.busy.begin("执行工具", time.Now()))
}

// ==================== 事件 ====================

// emit 往对话里追加一条完整的
func (m *model) emit(k *kind, text string) { m.tr.push(item{kind: k}).write(text) }

// handleEvent 处理一帧，返回需要执行的 tea.Cmd（读秒计时循环的启动）
func (m *model) handleEvent(f client.Frame) tea.Cmd {
	// 每帧带 root（哪段对话）与 session_id（谁说的）。只留当前这段的；审批例外，不管在看哪段都要弹。
	// 没有 root 的帧是进程级的（statusline）或回放出来的（已经按对话取过了）。
	cur := m.client.SessionID()
	switch {
	case f.Event == "permission_request" || f.Root == "":
	case f.Root != cur:
		return nil
	case f.SessionID != f.Root:
		m.subEvent(f)
		return nil
	}

	switch f.Event {
	case "turn_start":
		return m.busy.begin("思考中", time.Now())

	case "message_start":
		// 收件箱三种来源都是 user；用户自己打的那条也从这里画
		if f.Text != "" {
			m.emit(kindUser, f.Text)
			m.sl.noteUserMessage(f.Text)
		}

	case "message_update":
		m.awaiting = false
		m.tr.stream(kindAssistant, f.Delta)
		return m.busy.begin("生成回复", time.Now())

	case "thinking_start":
		return m.busy.begin("思考中", time.Now())

	case "thinking_update":
		m.awaiting = false
		m.tr.stream(kindThinking, f.Delta)
		return m.busy.begin("思考中", time.Now())

	case "message_end", "thinking_stop":
		m.tr.close()

	case "statusline":
		// 与 GET /statusline 同一份载荷，覆盖写
		m.sl.model, m.sl.window = f.Model, f.Context

	case "status_update":
		// 本次 run 累计的绝对值，覆盖写。算不出钱时帧里不带 cost，解出来的 0 就是"没数据"，不覆盖
		if f.Cost != 0 {
			m.busy.cost = f.Cost
		}
		m.sl.live.merge(f.Live)

	case "tool_execution_start":
		m.awaiting = false
		m.tr.call(f.ID).name = f.Name
		return m.busy.begin(toolVerb(f.Name), time.Now())

	case "tool_output":
		// 工具边跑边推的输出，挂到它那次调用下面。一帧不一定是一整行，续写即可
		m.awaiting = false
		m.tr.call(f.CallID).write(f.Text)

	case "tool_execution_end":
		c := m.tr.call(f.ID)
		c.name, c.state = f.Name, toolOK
		if f.Status != 0 {
			c.state = toolFailed
		}
		return m.busy.begin("思考中", time.Now()) // 工具完事，等下一轮 LLM

	case "permission_request":
		// agent 阻塞等裁决，进审批模态
		m.approval = &pendingApproval{id: f.ID, tool: f.Tool, params: string(f.Params)}
		switch {
		case f.Root != "" && f.Root != cur:
			m.approval.from = "另一段对话"
		case f.SessionID != f.Root:
			m.approval.from = "子任务"
		}
		return m.busy.begin("等待你的审批", time.Now())

	case "interrupted":
		m.emit(kindInfo, "已中断")
		m.awaiting = false
		m.busy.stop()
		m.approval = nil

	case "turn_end":
		// turn 结束不是收工（还要过收工判定），读秒只认 agent_end
		m.tr.close()
		// core 报的失败落到对话里
		if f.Error != "" {
			m.emit(kindError, "✗ "+f.Error)
		}

	case "agent_end":
		// 唯一的收工信号
		m.tr.close()
		m.awaiting = false
		m.busy.stop()
		return fetchGitCmd() // 这一趟可能改了文件、提交了：刷新 git 段
	}
	return nil
}

// subEvent 画子 agent 的动静：一行一条，缩进挂在父对话里。正文与思考不铺开——
// 两段文字交错着长，对话就乱了；它干完了什么，作为 spawn 的结果回到父对话。
// 读秒行与 awaiting 只归父对话管：子 agent 收工不是这段对话收工。
func (m *model) subEvent(f client.Frame) {
	switch f.Event {
	case "message_start":
		task, _, _ := strings.Cut(f.Text, "\n")
		m.emit(kindInfo, "  ↳ 子任务："+task)
	case "tool_execution_start":
		m.emit(kindInfo, "  ↳   🔧 "+f.Name)
	case "interrupted":
		m.emit(kindInfo, "  ↳ 子任务已中断")
	case "agent_end":
		m.emit(kindInfo, "  ↳ 子任务收工")
	}
}

// mine 是这帧是否出自当前对话本身（不是子任务，也不是别段对话）
func (m *model) mine(f client.Frame) bool {
	return f.Root == m.client.SessionID() && f.SessionID == f.Root
}

// ==================== 渲染 ====================

// renderMenu 渲染斜杠命令菜单：/命令名  说明
func renderMenu(cmds []client.Command, sel, width int) []string {
	labels := make([]string, len(cmds))
	for i, c := range cmds {
		labels[i] = strings.TrimSpace("/" + c.Name + "  " + c.Description)
	}
	return renderList(labels, sel, menuMaxRows, width)
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

// renderModels 把 /model 结果（[]ModelInfo JSON）渲染为多行 info 文本。
// 每行：标记 名称 [供应商] 上下文；● 是当前主模型。切换用 /model <name>。
func renderModels(data json.RawMessage) string {
	var list []client.ModelInfo
	if err := json.Unmarshal(data, &list); err != nil {
		return describeCommand("model")
	}
	if len(list) == 0 {
		return "✅ /model: 端点没报任何模型（手动指定：/model <模型名>）"
	}
	var out []string
	for _, mi := range list {
		mark := "  "
		if mi.Current {
			mark = "● "
		}
		out = append(out, mark+modelLabel(mi))
	}
	return strings.Join(out, "\n")
}

// modelLabel 是一个模型的一行说明：名称 [供应商] 上下文
func modelLabel(mi client.ModelInfo) string {
	text := mi.Name
	if mi.OwnedBy != "" {
		text += " [" + mi.OwnedBy + "]"
	}
	if c := humanContext(mi.Context); c != "" {
		text += "  " + c
	}
	return text
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
	from := ""
	if p.from != "" {
		from = "（" + p.from + "）"
	}
	head := fit("🔐 权限请求"+from+": "+desc, width) // 按显示宽截，不切碎中文
	return []string{
		approvalStyle.Render(head),
		approvalStyle.Render("   [y] 允许    [n] 拒绝"),
	}
}

// 用法：realagent-tui [setup] [addr]。带 setup = 不管引导过没有，先重走一遍引导
func main() {
	args := os.Args[1:]
	force := len(args) > 0 && args[0] == "setup"
	if force {
		args = args[1:]
	}
	addr := "127.0.0.1:12345"
	if len(args) > 0 {
		addr = args[0]
	}
	// workdir 由客户端给：它知道用户站在哪，core 不猜
	wd, err := os.Getwd()
	if err != nil {
		fmt.Fprintln(os.Stderr, "取不到当前目录:", err)
		os.Exit(1)
	}
	c := client.New(addr, wd)
	// 退出时通知 core（ADR-0021）
	defer c.CloseGroup()

	// 首启引导（setup.go）：settings.json 里 setup_done 不为 true 就先走一遍
	s, err := c.FetchSetup()
	if err != nil {
		fmt.Fprintln(os.Stderr, "连不上 core:", err)
		os.Exit(1)
	}
	if force || !s.Done {
		runSetup(c, s)
	}

	// alternate screen 由 View 声明（view.go）
	p := tea.NewProgram(initialModel(c))
	go pump(c, p)
	if _, err = p.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "TUI 运行失败:", err)
		os.Exit(1)
	}
}
