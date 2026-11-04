// 用户自己的状态栏命令，接口照抄 Claude Code 的 statusLine：
//
//	~/.realagent/statusline.json: {"type": "command", "command": "~/.claude/ccline/ccline"}
//
// 状态一变，TUI 用 sh -c 跑这条命令，把当前状态按 Claude Code 的 JSON
// 形状写进它的 stdin，它 stdout 的每一行就是状态栏的一行，ANSI 颜色原样保留。
// 终端宽度放在环境变量 COLUMNS 里。没有这个文件就画内置的 ccline 样式。
//
// 文件就是 Claude Code settings.json 里 statusLine 那个对象，原样拷过来能用；
// 给 Claude Code 写的状态栏脚本因此不用改。
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/ansi"
)

const extTimeout = 5 * time.Second

// external 是状态栏命令的状态：最近一次跑它时的输入和最近一次输出
type external struct {
	command string // 空 = 没配
	input   []byte
	width   int
	gen     int // 每跑一次 +1；回来的不是最新那次就丢
	lines   []string
}

type extOutput struct {
	gen   int
	lines []string
}

// loadStatuslineCommand 读 ~/.realagent/statusline.json；没有这个文件 = 用内置的
func loadStatuslineCommand() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", nil
	}
	b, err := os.ReadFile(filepath.Join(home, ".realagent", "statusline.json"))
	if errors.Is(err, fs.ErrNotExist) {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("statusline.json: %w", err)
	}
	var c struct{ Type, Command string }
	if err := json.Unmarshal(b, &c); err != nil {
		return "", fmt.Errorf("statusline.json: %w", err)
	}
	if c.Type != "command" || c.Command == "" {
		return "", errors.New(`statusline.json: 应为 {"type": "command", "command": "..."}，状态栏用内置的`)
	}
	return c.Command, nil
}

// watchStatusline 在每次 Update 之后看一眼：命令的输入或终端宽度变了就跑一次
func (m *model) watchStatusline() tea.Cmd {
	e := &m.sl.ext
	if e.command == "" {
		return nil
	}
	in := m.statusInput()
	if bytes.Equal(in, e.input) && m.width == e.width {
		return nil
	}
	e.input, e.width = in, m.width
	e.gen++
	gen, command, width := e.gen, e.command, e.width
	return func() tea.Msg { return extOutput{gen, runExternal(command, in, width)} }
}

// render 是命令的输出，每行截到终端宽
func (e external) render(width int) []string {
	out := make([]string, len(e.lines))
	for i, l := range e.lines {
		out[i] = ansi.Truncate(l, max(width, 1), "")
	}
	return out
}

// runExternal 跑一次命令：失败了状态栏上就是错误原话
func runExternal(command string, in []byte, width int) []string {
	ctx, cancel := context.WithTimeout(context.Background(), extTimeout)
	defer cancel()
	c := exec.CommandContext(ctx, "sh", "-c", command)
	c.Stdin = bytes.NewReader(in)
	c.Env = append(os.Environ(), fmt.Sprintf("COLUMNS=%d", width))
	var stderr bytes.Buffer
	c.Stderr = &stderr
	out, err := c.Output()
	if err != nil {
		why, _, _ := strings.Cut(strings.TrimSpace(stderr.String()), "\n")
		if why == "" {
			why = err.Error()
		}
		return []string{colored("1", "statusline: "+why)}
	}
	s := strings.TrimRight(string(out), "\n")
	if s == "" {
		return nil
	}
	return strings.Split(s, "\n")
}

// statusInput 是喂给命令的 JSON：字段名照 Claude Code，没有的字段不发；
// realagent 自己多出来的数据放在 "realagent" 下
func (m model) statusInput() []byte {
	sl := m.sl
	in := map[string]any{
		"cwd":       sl.cwd,
		"model":     map[string]any{"id": sl.model, "display_name": sl.model},
		"workspace": map[string]any{"current_dir": sl.cwd, "project_dir": sl.cwd},
		"realagent": sl.live,
	}
	if id := m.client.SessionID(); id != "" {
		in["session_id"] = id
	}
	if sl.session != "" {
		in["session_name"] = sl.session
	}
	cw := map[string]any{}
	if sl.window > 0 {
		cw["context_window_size"] = sl.window
	}
	if t := sl.live.ContextTokens; t != nil {
		cw["total_input_tokens"] = *t
		if sl.window > 0 {
			used := 100 * float64(*t) / float64(sl.window)
			cw["used_percentage"], cw["remaining_percentage"] = used, 100-used
		}
	}
	if len(cw) > 0 {
		in["context_window"] = cw
	}
	if h := sl.live.CacheHit; h != nil {
		in["prompt_cache"] = map[string]any{"hit_ratio": *h}
	}
	b, _ := json.Marshal(in)
	return b
}
