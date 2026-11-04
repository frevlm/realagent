// 状态栏（输入框下方）：照抄本机 ccline 的 cometix 预设（Haleclipse/CCometixLine v2.0.1）。
// 两行，段、字形、配色、折行规则跟它一致：
//
//	model | directory | git | session_name | context
//	quota.5h | quota.7d | cache | timing.ttft | timing.tps | cc_version
//
// 每段有 full / compact / minimal 三档写法。一行放不下先在段与段之间折行，
// 一段自己就超宽再按优先级逐段降档，降过 minimal 就不画。
// 没数据的值填 "--"，段照画。配额、缓存倒计时、版本 realagent 没有数据源，恒为 "--"。
// 配色是 ccline 的 terminal 调色板（ANSI 16 色，跟着终端主题走）。
//
// 显示哪些段、字形用哪一档由 /statusline 命令配置（本文件末尾），纯客户端状态，进程重启即复原。
// 整条栏可以换成用户自己的命令（Claude Code 的 statusLine 接口，见 statusline_cmd.go）。
package main

import (
	"cmp"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"

	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"
	"realagent/tui/internal/client"
)

// ==================== 数据 ====================

// gitInfo 是一次 `git status --porcelain=v2 --branch` 的结果
type gitInfo struct {
	branch, oid                            string
	ahead, behind                          int
	staged, modified, untracked, conflicts int
}

func (g gitInfo) dirty() bool { return g.staged+g.modified+g.untracked+g.conflicts > 0 }

// liveStats 是 core 的 status_update 帧里喂状态栏的那几个键
type liveStats client.Live

// merge 收一帧：帧里带了的键覆盖，没带的保留旧值
func (l *liveStats) merge(f client.Live) {
	if f.ContextTokens != nil {
		l.ContextTokens = f.ContextTokens
	}
	if f.CacheHit != nil {
		l.CacheHit = f.CacheHit
	}
	if f.TTFTMs != nil {
		l.TTFTMs = f.TTFTMs
	}
	if f.TPS != nil {
		l.TPS = f.TPS
	}
}

type statusline struct {
	model   string
	effort  string // 主模型的思考量级；没配为空
	window  int64  // 模型上下文窗口（GET /statusline 的 context）；0 = 不知道
	cwd     string
	git     *gitInfo
	session string // 当前对话的标题：第一条 user 消息
	live    liveStats

	hidden [segCount]bool
	tier   string // "nerd" | "unicode" | "ascii"

	ext external // 用户自己的状态栏命令；没配就画内置的
}

func newStatusline() (statusline, error) {
	sl := statusline{tier: pickTier()}
	sl.cwd, _ = os.Getwd()
	var err error
	sl.ext.command, err = loadStatuslineCommand()
	return sl, err
}

// pickTier 按 REALAGENT_ICONS 选初始字形档，默认 unicode（不要求装 Nerd Font）
func pickTier() string {
	switch t := os.Getenv("REALAGENT_ICONS"); t {
	case "nerd", "ascii":
		return t
	}
	return "unicode"
}

// forget 丢掉跟着对话走的数据（换对话、开新对话时）
func (sl *statusline) forget() {
	sl.session = ""
	sl.live = liveStats{}
}

// noteUserMessage 记下对话标题：第一条 user 消息（与 core 的 title_of 同源）
func (sl *statusline) noteUserMessage(text string) {
	if sl.session == "" {
		sl.session = strings.Join(strings.Fields(text), " ")
	}
}

// ==================== 拉取 ====================

// fetchStatusCmd 拿状态栏的初值。GET /statusline 与 statusline 帧同一份载荷，回来就当一帧处理
func fetchStatusCmd(c *client.Client) tea.Cmd {
	return func() tea.Msg {
		s, _ := c.FetchStatusline()
		return eventMsg{Event: "statusline", Model: s.Model, Effort: s.Effort, Context: s.Context}
	}
}

// gitMsg 是一次 git 状态刷新；nil = 不在仓库里或 git 不可用
type gitMsg struct{ info *gitInfo }

func fetchGitCmd() tea.Cmd {
	return func() tea.Msg {
		out, err := exec.Command("git", "--no-optional-locks", "-c", "core.quotePath=false",
			"status", "--porcelain=v2", "--branch").Output()
		if err != nil {
			return gitMsg{}
		}
		g := parseGitStatus(string(out))
		return gitMsg{&g}
	}
}

func parseGitStatus(porcelain string) gitInfo {
	var g gitInfo
	for _, line := range strings.Split(porcelain, "\n") {
		kind, rest, _ := strings.Cut(line, " ")
		switch {
		case kind == "#":
			g.header(rest)
		case kind == "u":
			g.conflicts++
		case kind == "?":
			g.untracked++
		case (kind == "1" || kind == "2") && len(rest) >= 2:
			g.staged += b2i(rest[0] != '.')
			g.modified += b2i(rest[1] != '.')
		}
	}
	return g
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

func (g *gitInfo) header(h string) {
	key, val, _ := strings.Cut(h, " ")
	switch {
	case key == "branch.oid" && val != "(initial)":
		g.oid = val
	case key == "branch.head" && val != "(detached)":
		g.branch = val
	case key == "branch.ab":
		fmt.Sscanf(val, "+%d -%d", &g.ahead, &g.behind)
	}
}

// ==================== 段 ====================

const (
	segModel = iota
	segDir
	segGit
	segSession
	segContext
	segQuota5h
	segQuota7d
	segCache
	segTTFT
	segTPS
	segVersion
	segCount
)

type severity int

const (
	sevNormal severity = iota
	sevWarn
	sevCrit
)

// reading 是一段此刻的样子：三档文字，进度条比例，严重度
type reading struct {
	text  [3]string // full, compact, minimal
	ratio float64   // 只有带进度条的段用
	sev   severity
}

// segment 是一段的固定属性
type segment struct {
	name      string
	line      int
	icon      [2]string // nerd, unicode；ascii 档无图标
	color     string
	iconColor string // 空 = 同段色
	bar       bool   // full 档在文字前画进度条
	prio      int    // 1 最先降档，5 最后
	read      func(statusline) reading
}

var segments = [segCount]segment{
	segModel:   {name: "model", icon: [2]string{"", "◆"}, color: "14", prio: 5, read: readModel},
	segDir:     {name: "directory", icon: [2]string{"\U000f024b", "▣"}, color: "10", iconColor: "11", prio: 4, read: readDir},
	segGit:     {name: "git", icon: [2]string{"\U000f02a2", "⎇"}, color: "12", prio: 4, read: readGit},
	segSession: {name: "session_name", icon: [2]string{"", "#"}, color: "2", prio: 2, read: readSession},
	segContext: {name: "context", icon: [2]string{"", "◔"}, color: "13", prio: 5, bar: true, read: readContext},
	segQuota5h: {name: "quota.5h", line: 1, icon: [2]string{"\U000f0520", "◷"}, color: "6", prio: 4, read: readQuota},
	segQuota7d: {name: "quota.7d", line: 1, icon: [2]string{"\U000f0520", "◷"}, color: "6", prio: 3, read: readQuota},
	segCache:   {name: "cache", line: 1, icon: [2]string{"", "≈"}, color: "4", prio: 2, read: readCache},
	segTTFT:    {name: "timing.ttft", line: 1, icon: [2]string{"", "⧖"}, color: "5", prio: 3, read: readTTFT},
	segTPS:     {name: "timing.tps", line: 1, icon: [2]string{"\U000f04c5", "⇶"}, color: "5", prio: 3, read: readTPS},
	segVersion: {name: "cc_version", line: 1, icon: [2]string{"", "v"}, color: "8", prio: 1, read: readVersion},
}

// none 是没数据时填的值
const none = "--"

func or(v string) string { return cmp.Or(v, none) }

func same(s string) reading { return reading{text: [3]string{s, s, s}} }

func readModel(sl statusline) reading {
	family, _, _ := strings.Cut(strings.TrimSpace(sl.model), " ")
	name := or(sl.model) + " · " + or(sl.effort)
	return reading{text: [3]string{name, name, or(family)}}
}

func readDir(sl statusline) reading {
	if sl.cwd == "" {
		return same(none)
	}
	parts := strings.FieldsFunc(sl.cwd, func(r rune) bool { return r == '/' || r == '\\' })
	path := "/" // ccline 的 parent 风格：末两级
	if len(parts) > 0 {
		path = strings.Join(parts[max(0, len(parts)-2):], "/")
	}
	return reading{text: [3]string{path, path, filepath.Base(sl.cwd)}}
}

func readGit(sl statusline) reading {
	g := sl.git
	if g == nil || (g.branch == "" && g.oid == "") {
		return same(none)
	}
	branch := g.branch
	if branch == "" {
		branch = "@" + g.oid[:min(7, len(g.oid))]
	}
	ascii := sl.tier == "ascii"
	status, dirty, sev := pick(ascii, "ok", "✓"), "", sevNormal
	if g.dirty() {
		status, dirty = pick(ascii, "*", "●"), "*"
	}
	if g.conflicts > 0 {
		status, sev = pick(ascii, "!", "⚠"), sevCrit
	}
	full := branch + " " + status
	if g.ahead > 0 {
		full += fmt.Sprintf(" %s%d", pick(ascii, "+", "↑"), g.ahead)
	}
	if g.behind > 0 {
		full += fmt.Sprintf(" %s%d", pick(ascii, "-", "↓"), g.behind)
	}
	return reading{text: [3]string{full, branch + dirty, branch}, sev: sev}
}

func pick(ascii bool, a, u string) string {
	if ascii {
		return a
	}
	return u
}

func readSession(sl statusline) reading {
	return same(or(ansi.Truncate(sl.session, 30, "…")))
}

// readContext：窗口不知道就没有百分比，进度条全空；token 数照报
func readContext(sl statusline) reading {
	pct, tokens, r := none, none, reading{}
	if t := sl.live.ContextTokens; t != nil {
		tokens = fmtTokens(*t)
		if sl.window > 0 {
			r.ratio = float64(*t) / float64(sl.window)
			pct = percent(r.ratio)
		}
	}
	switch {
	case r.ratio >= 0.8:
		r.sev = sevCrit
	case r.ratio >= 0.5:
		r.sev = sevWarn
	}
	r.text = [3]string{pct + "% · " + tokens, pct + "%", pct + "%"}
	return r
}

func readQuota(statusline) reading {
	return reading{text: [3]string{none + "% · " + none, none + "%", none + "%"}}
}

// readCache：缓存什么时候过期 realagent 不知道，倒计时恒为 --
func readCache(sl statusline) reading {
	hit := none
	if h := sl.live.CacheHit; h != nil {
		hit = percent(*h)
	}
	return reading{text: [3]string{none + " · " + hit + "% hit", none + " · " + hit + "%", none}}
}

func readTTFT(sl statusline) reading {
	if sl.live.TTFTMs == nil {
		return same(none)
	}
	return same(fmt.Sprintf("%.1fs", float64(*sl.live.TTFTMs)/1000))
}

func readTPS(sl statusline) reading {
	tps := none
	if t := sl.live.TPS; t != nil && *t > 0 {
		tps = fmt.Sprintf("%.0f", *t)
	}
	return reading{text: [3]string{tps + " tok/s", tps + "/s", tps}}
}

func readVersion(statusline) reading { return same(none) }

// fmtTokens: 842, 45.2k, 124k, 1.3M（与 ccline metrics/fmt.rs 一致）
func fmtTokens(n int64) string {
	trim := func(s string) string { return strings.TrimSuffix(s, ".0") }
	switch {
	case n < 1_000:
		return fmt.Sprint(n)
	case n < 100_000:
		return trim(fmt.Sprintf("%.1f", float64(n)/1e3)) + "k"
	case n < 1_000_000:
		return fmt.Sprintf("%.0fk", math.Round(float64(n)/1e3))
	}
	return trim(fmt.Sprintf("%.1f", float64(n)/1e6)) + "M"
}

// percent: 整数百分比，非零时至少 1
func percent(ratio float64) string {
	p := math.Round(ratio * 100)
	if p == 0 && ratio > 0 {
		return "1"
	}
	return fmt.Sprint(int64(p))
}

// ==================== 渲染 ====================

type role int

const (
	roleText  role = iota // 段色，告警时换成严重度色
	roleSep               // " · "：白
	roleIcon              // 图标：段色或单独指定的图标色
	roleMuted             // 进度条空格与边框：灰
)

type piece struct {
	text string
	role role
}

// slItem 是一段在三档下画出来的样子
type slItem struct {
	variants  [3][]piece
	color     string
	iconColor string
	sev       severity
	prio      int
}

func pieceColor(it slItem, p piece) string {
	switch {
	case p.role == roleSep:
		return "7"
	case p.role == roleMuted:
		return "8"
	case p.role == roleIcon:
		return it.iconColor
	case it.sev == sevWarn:
		return "3"
	case it.sev == sevCrit:
		return "1"
	}
	return it.color
}

// item 把一段画成三档；被隐藏返回 nil
func (sl statusline) item(i int) *slItem {
	if sl.hidden[i] {
		return nil
	}
	s := segments[i]
	r := s.read(sl)
	it := slItem{color: s.color, iconColor: cmp.Or(s.iconColor, s.color), sev: r.sev, prio: s.prio}
	icon := ""
	if sl.tier != "ascii" {
		icon = s.icon[b2i(sl.tier == "unicode")]
	}
	for lv, text := range r.text {
		var ps []piece
		// minimal 档不带图标；full 档带进度条
		if icon != "" && lv < 2 {
			ps = append(ps, piece{icon, roleIcon}, piece{" ", roleText})
		}
		if s.bar && lv == 0 {
			ps = append(append(ps, bar(r.ratio, sl.tier)...), piece{" ", roleText})
		}
		for j, part := range strings.Split(text, " · ") {
			if j > 0 {
				ps = append(ps, piece{" · ", roleSep})
			}
			ps = append(ps, piece{part, roleText})
		}
		it.variants[lv] = ps
	}
	return &it
}

// bar 是 10 格的 shapes 进度条（▰▱，ascii 档 [==--]），只画满格
func bar(ratio float64, tier string) []piece {
	full, empty, open, close := "▰", "▱", "", ""
	if tier == "ascii" {
		full, empty, open, close = "=", "-", "[", "]"
	}
	n := int(min(max(ratio, 0), 1) * 10)
	return []piece{{open, roleMuted}, {strings.Repeat(full, n), roleText},
		{strings.Repeat(empty, 10-n), roleMuted}, {close, roleMuted}}
}

const slSep = " | "

// width 是一行在各段取 lv 档时的宽；lv > 2 的段不画
func width(row []slItem, lv []int) int {
	w, n := 0, 0
	for i, it := range row {
		if lv[i] > 2 {
			continue
		}
		for _, p := range it.variants[lv[i]] {
			w += ansi.StringWidth(p.text)
		}
		n++
	}
	return w + max(n-1, 0)*len(slSep)
}

// layout 把一行的段排成若干终端行：按 full 档宽在段间折行，
// 一行还放不下就挑优先级最低的（同级取靠后的）降一档，直到放下
func layout(items []slItem, w int) []string {
	var rows [][]slItem
	for _, it := range items {
		last := len(rows) - 1
		if last >= 0 && width(append(slices.Clone(rows[last]), it), make([]int, len(rows[last])+1)) <= w {
			rows[last] = append(rows[last], it)
			continue
		}
		rows = append(rows, []slItem{it})
	}
	var out []string
	for _, row := range rows {
		lv := make([]int, len(row))
		for width(row, lv) > w {
			pick := -1
			for i, it := range row {
				if lv[i] <= 2 && (pick < 0 || it.prio <= row[pick].prio) {
					pick = i
				}
			}
			if pick < 0 {
				break
			}
			lv[pick]++
		}
		out = append(out, paintRow(row, lv))
	}
	return out
}

func paintRow(row []slItem, lv []int) string {
	var parts []string
	for i, it := range row {
		if lv[i] > 2 {
			continue
		}
		var b strings.Builder
		for _, p := range it.variants[lv[i]] {
			b.WriteString(lipgloss.NewStyle().Foreground(lipgloss.Color(pieceColor(it, p))).Render(p.text))
		}
		parts = append(parts, b.String())
	}
	return strings.Join(parts, colored("7", slSep))
}

func colored(c, s string) string {
	return lipgloss.NewStyle().Foreground(lipgloss.Color(c)).Render(s)
}

// render 渲染状态栏：配了命令就是命令的输出，否则两行各自排版
func (sl statusline) render(w int) []string {
	if sl.ext.command != "" {
		return sl.ext.render(w)
	}
	var out []string
	for line := range 2 {
		var items []slItem
		for i, s := range segments {
			if it := sl.item(i); it != nil && s.line == line {
				items = append(items, *it)
			}
		}
		out = append(out, layout(items, max(w, 1))...)
	}
	return out
}

// ==================== /statusline 命令（纯本地，不经 core） ====================

// statuslineCmd 是本地命令的注册项（合入斜杠菜单，见 main.go menuMatches）
var statuslineCmd = client.Command{
	Name:        "statusline",
	Description: "配置状态栏：icons nerd|unicode|ascii，enable|disable <段>",
}

var tiers = []string{"nerd", "unicode", "ascii"}

func segmentNames() string {
	names := make([]string, segCount)
	for i, s := range segments {
		names[i] = s.name
	}
	return strings.Join(names, "|")
}

func statuslineUsage() string {
	return "用法: /statusline [icons " + strings.Join(tiers, "|") + "] [enable|disable " + segmentNames() + "]"
}

// applyStatuslineCmd 解析 /statusline 的参数（不含命令名本身），返回更新后的状态与提示文本。
// 参数不合法一律回退到原值 + 用法提示，绝不半改。无参数的 /statusline 开面板（见 submitInput）
func (sl statusline) applyStatuslineCmd(rest string) (statusline, string) {
	args := strings.Fields(rest)
	if len(args) != 2 {
		return sl, statuslineUsage()
	}
	verb, arg := args[0], args[1]
	switch verb {
	case "icons":
		if !slices.Contains(tiers, arg) {
			return sl, "未知字形档: " + arg + "（" + strings.Join(tiers, "|") + "）\n" + statuslineUsage()
		}
		sl.tier = arg
		return sl, "✅ 字形切换为 " + arg

	case "enable", "disable":
		i := slices.IndexFunc(segments[:], func(s segment) bool { return s.name == arg })
		if i < 0 {
			return sl, "未知段: " + arg + "（" + segmentNames() + "）\n" + statuslineUsage()
		}
		sl.hidden[i] = verb == "disable"
		if sl.hidden[i] {
			return sl, "✅ 隐藏 " + arg
		}
		return sl, "✅ 显示 " + arg
	}
	return sl, statuslineUsage()
}

// panel 把状态栏配置做成子面板（/statusline 无参数时打开）：
// 每项确认后走的是和手打完全一样的 /statusline 子命令，本地生效不走网络。
func (sl statusline) panel() *panel {
	p := &panel{title: "状态栏配置（Enter 切换）"}
	for i, s := range segments {
		verb, state := "enable", "隐藏"
		if !sl.hidden[i] {
			verb, state = "disable", "显示"
		}
		p.items = append(p.items, panelItem{label: s.name + "  " + state, mark: !sl.hidden[i], submit: "/statusline " + verb + " " + s.name})
	}
	for _, t := range tiers {
		p.items = append(p.items, panelItem{label: "icons  " + t, mark: sl.tier == t, submit: "/statusline icons " + t})
	}
	return p
}
