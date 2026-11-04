// /config 的面板与本地 statusline 项
package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"realagent/tui/internal/client"
)

var configData = json.RawMessage(`{"items":[
	{"key":"compact_threshold","description":"d","value":85,"choices":[80,85,100]},
	{"key":"permission","description":"d","value":"ask","choices":["ask","deny"]}]}`)

func TestConfigPanelWant(t *testing.T) {
	for in, want := range map[string]string{
		"/config":                      "config",
		"/config permission":           "config-key",
		"/config permission deny":      "",
		"/config statusline ~/bin/sl ": "",
	} {
		if got := panelWantOf(in, false); got != want {
			t.Errorf("panelWantOf(%q) = %q, want %q", in, got, want)
		}
	}
}

// 第一层：每个键一项进取值面板，末尾是本地的 statusline 项，Enter 把当前命令填进输入框
func TestConfigPanel(t *testing.T) {
	p := configPanel(configData, "cat")
	if len(p.items) != 3 {
		t.Fatalf("两个键 + statusline，got %+v", p.items)
	}
	if p.items[0].submit != "/config compact_threshold" || p.items[0].edit {
		t.Errorf("键项 Enter 直接发 /config <键>，got %+v", p.items[0])
	}
	if sl := p.items[2]; sl.submit != "/config statusline cat " || !sl.edit {
		t.Errorf("statusline 项填进输入框等改，got %+v", sl)
	}
}

// 第二层：取值原样成命令，数字不带引号；当前值打 ●、光标落在那儿
func TestChoicePanel(t *testing.T) {
	data := json.RawMessage(`{"items":[{"key":"compact_threshold","description":"d","value":85,"choices":[80,85,100]}]}`)
	p := choicePanel(data)
	if len(p.items) != 3 || p.items[1].submit != "/config compact_threshold 85" || !p.items[1].mark || p.sel != 1 {
		t.Fatalf("got %+v sel=%d", p.items, p.sel)
	}
}

// 回包按期待开对应的面板：无参开键面板，带键开取值面板
func TestConfigReplyOpensPanel(t *testing.T) {
	m := testModel()
	m.awaiting, m.panelWant = true, "config"
	nm, _ := m.Update(sendMsg{reply: client.Reply{Ok: true, Command: "config", Data: configData}})
	if p := nm.(model).panel; p == nil || len(p.items) != 3 {
		t.Fatalf("应开键面板，got %+v", p)
	}
}

// /config statusline <命令> 写 statusline.json 并即刻换上；清空 = 删文件回内置
func TestConfigStatuslineLocal(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	path := filepath.Join(home, ".realagent", "statusline.json")

	m := typed("/config statusline echo hi")
	m, _ = m.submitInput(false)
	b, err := os.ReadFile(path)
	if err != nil || m.sl.ext.command != "echo hi" {
		t.Fatalf("应写文件并换上命令，err=%v command=%q", err, m.sl.ext.command)
	}
	if got, _ := loadStatuslineCommand(); got != "echo hi" {
		t.Errorf("写出的文件应能被启动时读回，got %q（%s）", got, b)
	}

	m.ed.set("/config statusline")
	m, _ = m.submitInput(false)
	if _, err := os.Stat(path); !os.IsNotExist(err) || m.sl.ext.command != "" {
		t.Errorf("清空应删文件回内置，stat err=%v command=%q", err, m.sl.ext.command)
	}
}
