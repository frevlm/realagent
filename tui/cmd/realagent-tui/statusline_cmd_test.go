// 外部状态栏命令：配置文件、喂进去的 JSON、跑命令、旧输出作废。
package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/ansi"
)

func writeConfig(t *testing.T, body string) {
	t.Helper()
	home := t.TempDir()
	t.Setenv("HOME", home)
	if body == "" {
		return
	}
	os.Mkdir(filepath.Join(home, ".realagent"), 0o755)
	if err := os.WriteFile(filepath.Join(home, ".realagent", "statusline.json"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
}

// 没文件用内置；Claude Code 的 statusLine 对象原样能用；写错了报出来，退回内置
func TestStatuslineConfig(t *testing.T) {
	cases := []struct {
		body, command string
		fails         bool
	}{
		{"", "", false},
		{`{"type": "command", "command": "ccline", "padding": 0}`, "ccline", false},
		{`{"type": "static"}`, "", true},
		{`{`, "", true},
	}
	for _, c := range cases {
		writeConfig(t, c.body)
		got, err := loadStatuslineCommand()
		if got != c.command || (err != nil) != c.fails {
			t.Errorf("%q: command %q err %v", c.body, got, err)
		}
	}
}

// 喂给命令的是 Claude Code 的字段名，ccline 这类现成的状态栏读得懂
func TestStatuslineInputIsClaudeCodeShape(t *testing.T) {
	m := testModel()
	m.sl = testStatusline()
	m.sl.cwd = "/home/u/app"
	var in map[string]any
	if err := json.Unmarshal(m.statusInput(), &in); err != nil {
		t.Fatal(err)
	}
	want := map[string]any{
		"cwd":          "/home/u/app",
		"session_id":   m.client.SessionID(),
		"session_name": "Fix the login redirect",
		"model":        map[string]any{"id": "Mythos Preview", "display_name": "Mythos Preview"},
		"workspace":    map[string]any{"current_dir": "/home/u/app", "project_dir": "/home/u/app"},
		"context_window": map[string]any{"context_window_size": 1e6, "total_input_tokens": 124000.0,
			"used_percentage": 12.4, "remaining_percentage": 87.6},
		"prompt_cache": map[string]any{"hit_ratio": 0.94},
		"realagent":    map[string]any{"context_tokens": 124000.0, "cache_hit": 0.94, "ttft_ms": 800.0, "tps": 74.2},
	}
	if !reflect.DeepEqual(in, want) {
		t.Errorf("statusInput =\n%v\nwant\n%v", in, want)
	}
}

// 命令的 stdout 一行一行就是状态栏，颜色原样，超宽截掉；失败了显示 stderr 原话
func TestStatuslineRunsCommand(t *testing.T) {
	got := runExternal(`cat >/dev/null; printf '\033[31mred\033[0m %s\nsecond\n' "$COLUMNS"`, nil, 42)
	if !reflect.DeepEqual(got, []string{"\x1b[31mred\x1b[0m 42", "second"}) {
		t.Errorf("输出 = %q", got)
	}
	sl := statusline{ext: external{command: "x", lines: []string{"abcdef"}}}
	eqLines(t, sl.render(3), []string{"abc"})

	got = runExternal(`echo boom >&2; exit 1`, nil, 80)
	if len(got) != 1 || ansi.Strip(got[0]) != "statusline: boom" {
		t.Errorf("失败时 = %q", got)
	}
}

// 输入不变不重跑；跑的期间又变了，先跑的那次回来作废
func TestStatuslineRerunsOnChange(t *testing.T) {
	m := testModel()
	m.sl.ext.command = "cat"
	m.width = 1000
	stale := m.watchStatusline()
	if m.watchStatusline() != nil {
		t.Error("输入没变不该再跑")
	}
	m.sl.session = "改了"
	fresh := m.watchStatusline()
	for _, run := range []func() tea.Msg{fresh, stale} {
		next, _ := m.Update(run())
		m = next.(model)
	}
	if s := strings.Join(m.sl.render(1000), ""); !strings.Contains(s, `"session_name":"改了"`) {
		t.Errorf("状态栏应是最新那次的输出（cat 回显的 JSON），got %q", s)
	}
}
