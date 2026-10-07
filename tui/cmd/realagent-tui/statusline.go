// 状态栏（输入框下方，格式照本地 claude code statusline / ccline cometix 主题）：
// model | directory | git。模型由 core 推帧更新；目录启动拿一次；
// git 启动拿一次，每次 agent_end 再拿一次——干净/脏标记随 agent 改文件而变，
// 收工是文件改完的时刻，不需要常驻刷新循环。
//
// 显示什么、图标用 emoji 还是 nerd font 由 /statusline 命令配置（本文件末尾），
// 纯客户端状态——core 不认展示偏好，不走网络、不持久化，进程重启即复原默认值。
package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"realagent/tui/internal/client"
)

// 配色同 cometix：图标常规、文字加粗，分隔符白色（c16 = 7）
var (
	slSepStyle       = lipgloss.NewStyle().Foreground(lipgloss.Color("7"))
	slModelIconStyle = lipgloss.NewStyle().Foreground(lipgloss.Color("14"))
	slModelTextStyle = lipgloss.NewStyle().Foreground(lipgloss.Color("14")).Bold(true)
	slDirIconStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("11"))
	slDirTextStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("10")).Bold(true)
	slGitIconStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("12"))
	slGitTextStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("12")).Bold(true)
)

// statuslineIcons 一套图标（emoji 或 nerd font）
type statuslineIcons struct {
	model, dir, git string
}

var emojiIcons = statuslineIcons{model: "🤖", dir: "📁", git: "🌿"}
var nerdIcons = statuslineIcons{model: "", dir: "\U000f024b", git: "\U000f02a2"}

// pickIconSet 按 REALAGENT_ICONS 环境变量选初始图标集，默认 emoji（零配置可用）；
// /statusline icons 命令可在运行时改，iconSet 是唯一真相源，图标集只是它的派生值。
func pickIconSet() string {
	if os.Getenv("REALAGENT_ICONS") == "nerd" {
		return "nerd"
	}
	return "emoji"
}

// statusline 是状态栏的数据源 + 展示偏好
type statusline struct {
	model   string // GET /statusline 拿到的模型名；空 = 未知，段隐藏
	dir     string // 进程 cwd 的 basename
	branch  string // 当前 git 分支；非 git 仓库则空，段隐藏
	gitMark string // "✓" 干净 / "●" 有改动；git status 失败则空，不显示

	showModel, showDir, showGit bool
	iconSet                     string // "emoji" | "nerd"
}

func newStatusline() statusline {
	sl := statusline{showModel: true, showDir: true, showGit: true, iconSet: pickIconSet()}
	if wd, err := os.Getwd(); err == nil {
		sl.dir = filepath.Base(wd)
	}
	sl.branch, sl.gitMark = gitInfo()
	return sl
}

func (sl statusline) icons() statuslineIcons {
	if sl.iconSet == "nerd" {
		return nerdIcons
	}
	return emojiIcons
}

// gitInfo 取当前分支名与工作区标记；非仓库或命令失败返回空（PROTOCOL.md 一贯原则：无数据不伪造）
func gitInfo() (branch, mark string) {
	out, err := exec.Command("git", "rev-parse", "--abbrev-ref", "HEAD").Output()
	if err != nil {
		return "", ""
	}
	branch = strings.TrimSpace(string(out))
	if branch == "" || branch == "HEAD" {
		return "", ""
	}
	out, err = exec.Command("git", "status", "--porcelain").Output()
	if err != nil {
		return branch, ""
	}
	if len(bytes.TrimSpace(out)) == 0 {
		return branch, "✓"
	}
	return branch, "●"
}

// gitMsg 携带 agent_end 后重取的 git 信息
type gitMsg struct {
	branch, mark string
}

func fetchGitCmd() tea.Msg {
	branch, mark := gitInfo()
	return gitMsg{branch: branch, mark: mark}
}

// statusMsg 携带 GET /statusline 的拉取结果
type statusMsg struct {
	model string
}

func fetchStatusCmd(c *client.Client) tea.Cmd {
	return func() tea.Msg {
		s, err := c.FetchStatusline()
		if err != nil {
			return statusMsg{}
		}
		return statusMsg{model: s.Model}
	}
}

// render 渲染状态栏一行；无任何段时返回 ""（调用方按空串跳过）
func (sl statusline) render() string {
	icons := sl.icons()
	var segs []string
	if sl.showModel && sl.model != "" {
		segs = append(segs, slModelIconStyle.Render(icons.model)+" "+slModelTextStyle.Render(sl.model))
	}
	if sl.showDir && sl.dir != "" {
		segs = append(segs, slDirIconStyle.Render(icons.dir)+" "+slDirTextStyle.Render(sl.dir))
	}
	if sl.showGit && sl.branch != "" {
		text := sl.branch
		if sl.gitMark != "" {
			text += " " + sl.gitMark
		}
		segs = append(segs, slGitIconStyle.Render(icons.git)+" "+slGitTextStyle.Render(text))
	}
	return strings.Join(segs, slSepStyle.Render(" | "))
}

// ==================== /statusline 命令（纯本地，不经 core） ====================

// statuslineCmd 是本地命令的注册项（合入斜杠菜单，见 main.go menuMatches）
var statuslineCmd = client.Command{
	Name:        "statusline",
	Description: "配置状态栏：icons emoji|nerd，enable|disable model|directory|git",
}

const statuslineUsage = "用法: /statusline [icons emoji|nerd] [enable|disable model|directory|git]"

// applyStatuslineCmd 解析 /statusline 的参数（不含命令名本身），返回更新后的状态与提示文本。
// 无参数 = 查看当前配置；参数不合法一律回退到原值 + 用法提示，绝不半改。
func (sl statusline) applyStatuslineCmd(rest string) (statusline, string) {
	args := strings.Fields(rest)
	if len(args) == 0 {
		return sl, sl.describe()
	}
	if len(args) != 2 {
		return sl, statuslineUsage
	}
	verb, arg := args[0], args[1]
	switch verb {
	case "icons":
		if arg != "emoji" && arg != "nerd" {
			return sl, "未知图标集: " + arg + "（emoji|nerd）\n" + statuslineUsage
		}
		sl.iconSet = arg
		return sl, "✅ 图标切换为 " + arg

	case "enable", "disable":
		on := verb == "enable"
		switch arg {
		case "model":
			sl.showModel = on
		case "directory":
			sl.showDir = on
		case "git":
			sl.showGit = on
		default:
			return sl, "未知段: " + arg + "（model|directory|git）\n" + statuslineUsage
		}
		state := "隐藏"
		if on {
			state = "显示"
		}
		return sl, "✅ " + state + " " + arg

	default:
		return sl, statuslineUsage
	}
}

// panel 把状态栏配置做成子面板（/statusline 无参数时打开）：
// 每项确认后走的是和手打完全一样的 /statusline 子命令，本地生效不走网络。
func (sl statusline) panel() *panel {
	p := &panel{title: "状态栏配置（Enter 切换）"}
	seg := func(name, arg string, on bool) panelItem {
		verb := "enable"
		if on {
			verb = "disable"
		}
		state := "隐藏"
		if on {
			state = "显示"
		}
		return panelItem{label: name + "  " + state, mark: on, submit: "/statusline " + verb + " " + arg}
	}
	p.items = append(p.items,
		seg("model", "model", sl.showModel),
		seg("directory", "directory", sl.showDir),
		seg("git", "git", sl.showGit),
		panelItem{label: "icons  emoji", mark: sl.iconSet == "emoji", submit: "/statusline icons emoji"},
		panelItem{label: "icons  nerd", mark: sl.iconSet == "nerd", submit: "/statusline icons nerd"},
	)
	return p
}

// describe 列出当前状态栏配置（/statusline 无参数时展示）
func (sl statusline) describe() string {
	seg := func(name string, on bool) string {
		if on {
			return name + ":on"
		}
		return name + ":off"
	}
	return "状态栏配置 — " + seg("model", sl.showModel) + " " + seg("directory", sl.showDir) +
		" " + seg("git", sl.showGit) + " icons:" + sl.iconSet
}
