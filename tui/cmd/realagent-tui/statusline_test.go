// 状态栏：与 ccline cometix 预设逐字一致的渲染、折行降级、数据来源、/statusline 命令。
package main

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"

	"github.com/charmbracelet/x/ansi"
	"realagent/tui/internal/client"
)

func ptr[T any](v T) *T { return &v }

// frame 按线上的 JSON 解一帧：status_update 的键缺不缺，要走真的解码才测得到
func frame(t *testing.T, event, data string) client.Frame {
	t.Helper()
	var f client.Frame
	if err := json.Unmarshal([]byte(`{"event":"`+event+`","data":`+data+`}`), &f); err != nil {
		t.Fatal(err)
	}
	return f
}

// ccline preview 的 normal 场景里 realagent 有数据的那些段
func testStatusline() statusline {
	return statusline{
		model: "Mythos Preview", window: 1_000_000, cwd: "u/app",
		git:     &gitInfo{branch: "main", ahead: 2, modified: 1},
		session: "Fix the login redirect",
		live: liveStats{ContextTokens: ptr[int64](124_000), CacheHit: ptr(0.94),
			TTFTMs: ptr[int64](800), TPS: ptr(74.2)},
		tier: "nerd",
	}
}

func plain(lines []string) []string {
	out := make([]string, len(lines))
	for i, l := range lines {
		out[i] = ansi.Strip(l)
	}
	return out
}

func eqLines(t *testing.T, got, want []string) {
	t.Helper()
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Errorf("render =\n%s\nwant\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}
}

// 对照 `ccline preview -w 200` 的输出；realagent 没有的值（effort、配额、版本……）填 --
func TestStatuslineMatchesCcline(t *testing.T) {
	eqLines(t, plain(testStatusline().render(200)), []string{
		" Mythos Preview · -- | \U000f024b u/app | \U000f02a2 main ● ↑2 |  Fix the login redirect |  ▰▱▱▱▱▱▱▱▱▱ 12% · 124k",
		"\U000f0520 --% · -- | \U000f0520 --% · -- |  -- · 94% hit |  0.8s | \U000f04c5 74 tok/s |  --",
	})
}

// 宽度不够时在段与段之间折行，段不拆开
func TestStatuslineWrapsBetweenSegments(t *testing.T) {
	eqLines(t, plain(testStatusline().render(45)), []string{
		"\uef23 Mythos Preview · -- | \U000f024b u/app | \U000f02a2 main ● ↑2",
		"\uf02b Fix the login redirect",
		"\uf49b ▰▱▱▱▱▱▱▱▱▱ 12% · 124k",
		"\U000f0520 --% · -- | \U000f0520 --% · -- | \uf1c0 -- · 94% hit",
		"\uf251 0.8s | \U000f04c5 74 tok/s | \uf05a --",
	})
}

// 一段自己就超宽：降到 compact（进度条与 tokens 没了），再不够降 minimal
func TestStatuslineStepsDownWhenOneSegmentIsTooWide(t *testing.T) {
	sl := statusline{window: 1_000_000, live: liveStats{ContextTokens: ptr[int64](124_000)}, tier: "unicode"}
	for i := range sl.hidden {
		sl.hidden[i] = i != segContext
	}
	eqLines(t, plain(sl.render(30)), []string{"◔ ▰▱▱▱▱▱▱▱▱▱ 12% · 124k"})
	eqLines(t, plain(sl.render(10)), []string{"◔ 12%"})
	eqLines(t, plain(sl.render(3)), []string{"12%"})
}

func TestStatuslineSeverity(t *testing.T) {
	sl := testStatusline()
	cases := []struct {
		tokens int64
		want   severity
	}{{124_000, sevNormal}, {500_000, sevWarn}, {810_000, sevCrit}}
	for _, c := range cases {
		sl.live.ContextTokens = &c.tokens
		if got := sl.item(segContext).sev; got != c.want {
			t.Errorf("context %d/1M severity = %v, want %v", c.tokens, got, c.want)
		}
	}
	sl.git = &gitInfo{branch: "feature/auth", behind: 3, conflicts: 1}
	if it := sl.item(segGit); it.sev != sevCrit {
		t.Errorf("有冲突的 git 段应为 crit，got %v", it.sev)
	}
	eqLines(t, plain(layout([]slItem{*sl.item(segGit)}, 200)), []string{"\U000f02a2 feature/auth ⚠ ↓3"})
}

// 段色是 ccline terminal 调色板；dir 的图标色单独指定（本机配置 icon_color = bright_yellow）
func TestStatuslineColors(t *testing.T) {
	sl := testStatusline()
	dir := *sl.item(segDir)
	if c := pieceColor(dir, piece{role: roleIcon}); c != "11" {
		t.Errorf("dir 图标色 = %s, want 11", c)
	}
	if c := pieceColor(dir, piece{role: roleText}); c != "10" {
		t.Errorf("dir 文字色 = %s, want 10", c)
	}
	sl.live.ContextTokens = ptr[int64](900_000)
	ctx := *sl.item(segContext)
	for r, want := range map[role]string{roleIcon: "13", roleText: "1", roleMuted: "8", roleSep: "7"} {
		if c := pieceColor(ctx, piece{role: r}); c != want {
			t.Errorf("crit 的 context 段 role %d 色 = %s, want %s", r, c, want)
		}
	}
}

func TestStatuslineGlyphTiers(t *testing.T) {
	sl := testStatusline()
	sl.tier = "ascii"
	eqLines(t, plain(sl.render(200))[:1], []string{
		"Mythos Preview · -- | u/app | main * +2 | Fix the login redirect | [=---------] 12% · 124k",
	})
	sl.tier = "unicode"
	eqLines(t, plain(sl.render(200))[:1], []string{
		"◆ Mythos Preview · -- | ▣ u/app | ⎇ main ● ↑2 | # Fix the login redirect | ◔ ▰▱▱▱▱▱▱▱▱▱ 12% · 124k",
	})
}

// 全无数据：每段照画，值填 --，进度条全空
func TestStatuslineDefaults(t *testing.T) {
	eqLines(t, plain(statusline{tier: "unicode"}.render(200)), []string{
		"◆ -- · -- | ▣ -- | ⎇ -- | # -- | ◔ ▱▱▱▱▱▱▱▱▱▱ --% · --",
		"◷ --% · -- | ◷ --% · -- | ≈ -- · --% hit | ⧖ -- | ⇶ -- tok/s | v --",
	})
}

// 窗口不知道（模型表里查不到）：token 数照报，百分比填 --，进度条全空
func TestStatuslineContextWithoutWindow(t *testing.T) {
	sl := testStatusline()
	sl.window = 0
	got := plain(layout([]slItem{*sl.item(segContext)}, 200))
	eqLines(t, got, []string{" ▱▱▱▱▱▱▱▱▱▱ --% · 124k"})
}

func TestFmtTokens(t *testing.T) {
	for n, want := range map[int64]string{842: "842", 8_000: "8k", 45_200: "45.2k", 124_499: "124k", 1_000_000: "1M", 1_270_000: "1.3M"} {
		if got := fmtTokens(n); got != want {
			t.Errorf("fmtTokens(%d) = %q, want %q", n, got, want)
		}
	}
	for r, want := range map[float64]string{0.624: "62", 0.001: "1", 0: "0"} {
		if got := percent(r); got != want {
			t.Errorf("percent(%v) = %q, want %q", r, got, want)
		}
	}
}

func TestParseGitStatus(t *testing.T) {
	g := parseGitStatus(`# branch.oid 9224f01a1b2c3d4e5f
# branch.head v2
# branch.upstream origin/v2
# branch.ab +2 -1
1 .M N... 100644 100644 100644 a b src/main.rs
1 M. N... 100644 100644 100644 a b src/lib.rs
1 MM N... 100644 100644 100644 a b Cargo.toml
2 R. N... 100644 100644 100644 a b R100 new.rs	old.rs
u UU N... 100644 100644 100644 100644 a b c conflict.rs
? notes.txt
? tmp/
`)
	want := gitInfo{branch: "v2", oid: "9224f01a1b2c3d4e5f", ahead: 2, behind: 1, staged: 3, modified: 2, untracked: 2, conflicts: 1}
	if g != want {
		t.Errorf("parseGitStatus = %+v, want %+v", g, want)
	}
}

// core 推来的 statusline 帧覆盖模型名与上下文窗口：/model 切档走这一条路
func TestStatuslineEventUpdatesModel(t *testing.T) {
	m := testModel()
	feed(&m, client.Frame{Event: "statusline", Model: "deepseek-v4", Context: 131072})
	if m.sl.model != "deepseek-v4" || m.sl.window != 131072 {
		t.Errorf("statusline 帧后 model/window = %q/%d", m.sl.model, m.sl.window)
	}
	if len(m.tr.items) != 0 {
		t.Errorf("statusline 帧不该往对话里写条目，got %v", dump(m))
	}
}

// status_update 帧喂上下文、缓存、时延；缺的键保留旧值，换对话清空
func TestStatuslineLiveStats(t *testing.T) {
	m := testModel()
	feed(&m,
		frame(t, "message_start", `{"role":"user","text":"修一下\n登录跳转"}`),
		frame(t, "message_start", `{"role":"user","text":"第二条"}`),
		frame(t, "status_update", `{"context_tokens":48213,"cache_hit":0.5}`),
		frame(t, "status_update", `{"cost":0.01}`),
		frame(t, "status_update", `{"ttft_ms":1234,"tps":61.7}`),
	)
	if m.sl.session != "修一下 登录跳转" {
		t.Errorf("session = %q, want 第一条 user 消息", m.sl.session)
	}
	l := m.sl.live
	if *l.ContextTokens != 48213 || *l.CacheHit != 0.5 || *l.TTFTMs != 1234 || *l.TPS != 61.7 {
		t.Errorf("live = %d %v %d %v", *l.ContextTokens, *l.CacheHit, *l.TTFTMs, *l.TPS)
	}
	m.reset()
	if m.sl.session != "" || m.sl.live != (liveStats{}) {
		t.Errorf("换对话后应清空，got %q %+v", m.sl.session, m.sl.live)
	}
}

func TestStatuslineToggleSegment(t *testing.T) {
	sl, msg := testStatusline().applyStatuslineCmd("disable timing.tps")
	if !sl.hidden[segTPS] || !strings.Contains(msg, "隐藏") {
		t.Errorf("disable 后应隐藏，got %v %q", sl.hidden[segTPS], msg)
	}
	if strings.Contains(ansi.Strip(strings.Join(sl.render(200), "\n")), "tok/s") {
		t.Error("隐藏的段不应渲染")
	}
	sl, _ = sl.applyStatuslineCmd("enable timing.tps")
	if sl.hidden[segTPS] {
		t.Error("enable 后应显示")
	}
}

func TestStatuslineSwitchIcons(t *testing.T) {
	sl, msg := testStatusline().applyStatuslineCmd("icons ascii")
	if sl.tier != "ascii" || !strings.Contains(msg, "ascii") {
		t.Errorf("tier = %q, msg = %q", sl.tier, msg)
	}
}

func TestStatuslineRejectsUnknownArgs(t *testing.T) {
	orig := testStatusline()
	for _, rest := range []string{"icons emoji", "enable bar", "disable", "wat", "icons"} {
		sl, msg := orig.applyStatuslineCmd(rest)
		if !reflect.DeepEqual(sl, orig) {
			t.Errorf("applyStatuslineCmd(%q) 不应改配置", rest)
		}
		if !strings.Contains(msg, "用法") {
			t.Errorf("applyStatuslineCmd(%q) 应给出用法，got %q", rest, msg)
		}
	}
}
