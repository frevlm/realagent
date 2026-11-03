// 输入行编辑器的单元测试：rune 光标 / 行编辑键 / 多行 / 粘贴。
package main

import (
	"testing"

	tea "charm.land/bubbletea/v2"
)

// 退格按字符走，不按字节：中文不会被劈成半个字符
func TestBackspaceRunes(t *testing.T) {
	var e editor
	e.insert("你好世界")
	e.backspace()
	if got := e.value(); got != "你好世" {
		t.Fatalf("退格一次 = %q, want 你好世", got)
	}
	e.backspace()
	e.backspace()
	e.backspace()
	if got := e.value(); got != "" {
		t.Errorf("退干净 = %q, want 空", got)
	}
	e.backspace() // 空串再退格不应 panic
}

func TestCursorMovement(t *testing.T) {
	var e editor
	e.insert("中a文")
	e.left()
	e.insert("X")
	if got := e.value(); got != "中aX文" {
		t.Errorf("光标处插入 = %q, want 中aX文", got)
	}
	e.home()
	e.insert("头")
	if got := e.value(); got != "头中aX文" {
		t.Errorf("行首插入 = %q, want 头中aX文", got)
	}
	e.end()
	e.insert("尾")
	if got := e.value(); got != "头中aX文尾" {
		t.Errorf("行尾插入 = %q, want 头中aX文尾", got)
	}
	// 越界移动不应越位
	for range 20 {
		e.left()
	}
	if e.pos != 0 {
		t.Errorf("左移越界 pos = %d, want 0", e.pos)
	}
	for range 20 {
		e.right()
	}
	if e.pos != len(e.runes) {
		t.Errorf("右移越界 pos = %d, want %d", e.pos, len(e.runes))
	}
}

func TestDelete(t *testing.T) {
	var e editor
	e.insert("中文abc")
	e.home()
	e.del()
	if got := e.value(); got != "文abc" {
		t.Errorf("delete = %q, want 文abc", got)
	}
	e.end()
	e.del() // 行尾 delete 不应 panic
	if got := e.value(); got != "文abc" {
		t.Errorf("行尾 delete 不应改内容，got %q", got)
	}
}

func TestKillOperations(t *testing.T) {
	var e editor
	e.insert("hello 世界 world")
	e.killWord()
	if got := e.value(); got != "hello 世界 " {
		t.Errorf("ctrl+w = %q, want %q", got, "hello 世界 ")
	}
	e.killWord()
	if got := e.value(); got != "hello " {
		t.Errorf("ctrl+w 再来 = %q, want %q", got, "hello ")
	}

	var e2 editor
	e2.insert("abc中文")
	e2.left()
	e2.killToEnd()
	if got := e2.value(); got != "abc中" {
		t.Errorf("ctrl+k = %q, want abc中", got)
	}
	e2.killToStart()
	if got := e2.value(); got != "" || e2.pos != 0 {
		t.Errorf("ctrl+u = %q pos=%d, want 空 pos=0", got, e2.pos)
	}
}

// 粘贴整块进输入框（含换行），不当按键解释
func TestPasteMultiline(t *testing.T) {
	m := testModel()
	nm, _ := m.Update(tea.PasteMsg{Content: "第一行\r\n第二行"})
	m = nm.(model)
	if got := m.ed.value(); got != "第一行\n第二行" {
		t.Errorf("粘贴 = %q, want 第一行\\n第二行", got)
	}
}

// Alt+Enter 换行，Enter 才是发送
func TestAltEnterInsertsNewline(t *testing.T) {
	m := testModel()
	m, _ = m.handleKey(keyRunes("第一行"))
	m, cmd := m.handleKey(tea.KeyPressMsg{Code: tea.KeyEnter, Mod: tea.ModAlt})
	if cmd != nil {
		t.Error("alt+enter 不应发送")
	}
	m, _ = m.handleKey(keyRunes("第二行"))
	if got := m.ed.value(); got != "第一行\n第二行" {
		t.Errorf("alt+enter 应插入换行，got %q", got)
	}
}

// 空输入按 Enter 什么都不该发生
func TestSubmitEmpty(t *testing.T) {
	m := testModel()
	m, cmd := m.handleKey(key(tea.KeyEnter))
	if cmd != nil {
		t.Error("空输入不应发送")
	}
	if len(m.tr.items) != 0 {
		t.Errorf("空输入不应产生条目，got %v", texts(m))
	}
}

// 审批模态下只认裁决键：esc 视为拒绝，其余按键一概不进输入框
func TestApprovalModalKeys(t *testing.T) {
	m := testModel()
	m.approval = &pendingApproval{id: "req-1", tool: "bash"}
	m2, _ := m.handleKey(keyRunes("x"))
	if m2.approval == nil || m2.ed.value() != "" {
		t.Error("模态下普通按键应被忽略")
	}
	m3, cmd := m.handleKey(key(tea.KeyEscape))
	if m3.approval != nil || cmd == nil {
		t.Error("esc 应视为拒绝并回传 core")
	}
}
