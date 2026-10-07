// 状态栏：/statusline 命令解析、段开关、图标切换、渲染降级。
package main

import (
	"strings"
	"testing"

	"realagent/tui/internal/client"
)

func testStatusline() statusline {
	return statusline{
		model: "claude-sonnet-5", dir: "realagent", branch: "main",
		showModel: true, showDir: true, showGit: true, iconSet: "emoji",
	}
}

func TestStatuslineDescribeNoArgs(t *testing.T) {
	sl, msg := testStatusline().applyStatuslineCmd("")
	if sl != testStatusline() {
		t.Error("无参数不应改配置")
	}
	for _, want := range []string{"model:on", "directory:on", "git:on", "icons:emoji"} {
		if !strings.Contains(msg, want) {
			t.Errorf("describe() = %q，缺 %q", msg, want)
		}
	}
}

func TestStatuslineToggleSegment(t *testing.T) {
	sl, msg := testStatusline().applyStatuslineCmd("disable git")
	if sl.showGit {
		t.Error("disable git 后 showGit 应为 false")
	}
	if !strings.Contains(msg, "隐藏") {
		t.Errorf("提示应含隐藏，got %q", msg)
	}
	sl2, _ := sl.applyStatuslineCmd("enable git")
	if !sl2.showGit {
		t.Error("enable git 后 showGit 应为 true")
	}
}

func TestStatuslineSwitchIcons(t *testing.T) {
	sl, msg := testStatusline().applyStatuslineCmd("icons nerd")
	if sl.iconSet != "nerd" {
		t.Errorf("iconSet = %q, want nerd", sl.iconSet)
	}
	if !strings.Contains(msg, "nerd") {
		t.Errorf("提示应含 nerd，got %q", msg)
	}
}

func TestStatuslineRejectsUnknownArgs(t *testing.T) {
	cases := []string{"icons foo", "enable bar", "disable", "wat", "icons"}
	orig := testStatusline()
	for _, rest := range cases {
		sl, msg := orig.applyStatuslineCmd(rest)
		if sl != orig {
			t.Errorf("applyStatuslineCmd(%q) 不应改配置，got %+v", rest, sl)
		}
		if msg == "" {
			t.Errorf("applyStatuslineCmd(%q) 应给出提示", rest)
		}
	}
}

// core 推来的 statusline 帧覆盖模型名：/model 切档走这一条路（手改 settings.json 需重启 core，ADR-0010）
func TestStatuslineEventUpdatesModel(t *testing.T) {
	m := testModel()
	m.sl = testStatusline()
	feedEvents(&m, client.Event{Type: "statusline",
		Payload: `{"model":"deepseek-v4","owned_by":"deepseek","context":131072}`})
	if m.sl.model != "deepseek-v4" {
		t.Errorf("statusline 帧后 model = %q, want deepseek-v4", m.sl.model)
	}
	if !strings.Contains(m.sl.render(), "deepseek-v4") {
		t.Errorf("状态栏应渲染新模型: %q", m.sl.render())
	}
	if len(m.lines) != 0 {
		t.Errorf("statusline 帧不该往对话流里写东西，got %v", roleTexts(m))
	}
}

func TestStatuslineRenderHidesDisabledOrEmptySegments(t *testing.T) {
	sl := testStatusline()
	sl.showGit = false
	got := sl.render()
	if strings.Contains(got, "main") {
		t.Errorf("disable 的段不应出现在渲染结果: %q", got)
	}
	if !strings.Contains(got, "claude-sonnet-5") || !strings.Contains(got, "realagent") {
		t.Errorf("未 disable 的段应渲染: %q", got)
	}

	empty := statusline{showModel: true, showDir: true, showGit: true}
	if got := empty.render(); got != "" {
		t.Errorf("全无数据应返回空串，got %q", got)
	}
}

// git 段带工作区标记（cometix 格式）：分支名后跟 ✓ / ●；拿不到标记就只显示分支
func TestStatuslineRenderGitMark(t *testing.T) {
	sl := testStatusline()
	sl.gitMark = "●"
	if got := sl.render(); !strings.Contains(got, "main ●") {
		t.Errorf("git 段应含 \"main ●\": %q", got)
	}
	sl.gitMark = ""
	if got := sl.render(); !strings.Contains(got, "main") {
		t.Errorf("无标记时仍应渲染分支: %q", got)
	}
}

// agent_end 后重取的 git 信息覆盖写进状态栏
func TestStatuslineGitMsgUpdates(t *testing.T) {
	m := testModel()
	m.sl = testStatusline()
	m, _ = m.update(gitMsg{branch: "feat", mark: "●"})
	if m.sl.branch != "feat" || m.sl.gitMark != "●" {
		t.Errorf("gitMsg 后 branch/mark = %q/%q, want feat/●", m.sl.branch, m.sl.gitMark)
	}
}
