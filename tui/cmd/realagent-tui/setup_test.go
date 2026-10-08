package main

import (
	"errors"
	"strings"
	"testing"

	"github.com/charmbracelet/bubbletea"
	"realagent/tui/internal/client"
)

var (
	kEnter = tea.KeyMsg{Type: tea.KeyEnter}
	kDown  = tea.KeyMsg{Type: tea.KeyDown}
)

func kType(s string) tea.KeyMsg { return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(s)} }

// press 依次喂按键；拉清单的命令不跑，由测试喂 modelsFetched
func press(m setupModel, keys ...tea.KeyMsg) setupModel {
	for _, k := range keys {
		m, _ = m.key(k)
	}
	return m
}

func fetched(m setupModel, list []string, err error) setupModel {
	next, _ := m.Update(modelsFetched{list: list, err: err})
	return next.(setupModel)
}

func TestSetupStepsInOrder(t *testing.T) {
	m := newSetupModel(nil, client.Setup{})
	m = press(m, kDown, kEnter, kType("http://x/v1"), kEnter, kType("sk-1"), kEnter)
	if m.step != stepModel || !m.loading {
		t.Fatalf("api_key 之后应进模型页拉清单，step=%d loading=%v", m.step, m.loading)
	}
	m = fetched(m, []string{"a", "b"}, nil)
	m = press(m, kDown, kEnter)                              // model：选 b
	m = press(m, kDown, kDown, kEnter, kType("own"), kEnter) // small_model：手动输入
	want := client.Setup{Protocol: "openai-chat", BaseURL: "http://x/v1", APIKey: "sk-1", Model: "b", SmallModel: "own"}
	if got := m.values(); got != want {
		t.Fatalf("走完 = %+v，想要 %+v", got, want)
	}
}

func TestSetupPrefillAndFetchFail(t *testing.T) {
	m := newSetupModel(nil, client.Setup{Protocol: "openai-chat", BaseURL: "http://x/v1", APIKey: "sk-old", Model: "m"})
	m = press(m, kEnter, kEnter)
	if v := m.View(); !strings.Contains(v, "sk-old") {
		t.Fatalf("已配的 api_key 应预填：\n%s", v)
	}
	m = press(m, kEnter)
	m = fetched(m, nil, errors.New("端点返回 HTTP 404"))
	if !m.manual || !strings.Contains(m.View(), "HTTP 404") || m.eds[stepModel].value() != "m" {
		t.Fatalf("拉不到清单应手填现值并说明原因：\n%s", m.View())
	}
}
