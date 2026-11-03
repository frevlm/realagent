// 条目模型：帧怎么折进对话，一条怎么画。
package main

import (
	"reflect"
	"testing"

	"github.com/charmbracelet/x/ansi"

	"realagent/tui/internal/client"
)

// 增量续写开着的那条，换了一类或收了尾就另起一条；空的 thinking_start 不留痕
func TestStreamGroupsByKind(t *testing.T) {
	m := testModel()
	feed(&m,
		client.Frame{Event: "thinking_start"},
		client.Frame{Event: "thinking_update", Delta: "先想"},
		client.Frame{Event: "thinking_update", Delta: "再想\n第二段"},
		client.Frame{Event: "thinking_stop"},
		client.Frame{Event: "message_update", Delta: "答"},
		client.Frame{Event: "message_update", Delta: "案"},
		client.Frame{Event: "turn_end"},
		client.Frame{Event: "message_update", Delta: "第二轮"},
	)
	want := []string{"thinking:先想再想\n第二段", "assistant:答案", "assistant:第二轮"}
	if got := dump(m); !reflect.DeepEqual(got, want) {
		t.Errorf("对话 = %q, want %q", got, want)
	}
}

// 一次工具调用是一条：输出按 call_id 挂到它下面（交错到达也各回各家），结束时状态就地改
func TestToolCallIsOneItem(t *testing.T) {
	m := testModel()
	feed(&m,
		client.Frame{Event: "message_update", Delta: "我来查"},
		client.Frame{Event: "tool_execution_start", Name: "a", ID: "c1"},
		client.Frame{Event: "tool_execution_start", Name: "b", ID: "c2"},
		client.Frame{Event: "tool_output", CallID: "c1", Text: "a1\nA"},
		client.Frame{Event: "tool_output", CallID: "c2", Text: "b1\n"},
		client.Frame{Event: "tool_output", CallID: "c1", Text: "2\n"},
		client.Frame{Event: "tool_execution_end", Name: "b", ID: "c2", Status: 1},
		client.Frame{Event: "tool_execution_end", Name: "a", ID: "c1"},
		client.Frame{Event: "message_update", Delta: "查完了"},
	)
	want := []string{"assistant:我来查", "tool:a:ok|a1\nA2\n", "tool:b:failed|b1\n", "assistant:查完了"}
	if got := dump(m); !reflect.DeepEqual(got, want) {
		t.Errorf("对话 = %q, want %q", got, want)
	}
}

// 一条画成什么样：首行前缀与续行缩进、思考头、工具头随状态变、末尾的换行不画成空行
func TestItemRows(t *testing.T) {
	cases := []struct {
		it   item
		want []string
	}{
		{item{kind: kindUser}, []string{"> 第一行", "  第二行"}},
		{item{kind: kindThinking}, []string{"💭 思考过程", "第一行", "第二行"}},
		{item{kind: kindTool, name: "bash"}, []string{"🔧 bash …", "   第一行", "   第二行"}},
		{item{kind: kindTool, name: "bash", state: toolFailed}, []string{"✗ bash", "   第一行", "   第二行"}},
		{item{kind: kindError}, []string{"! 第一行", "  第二行"}},
	}
	for _, c := range cases {
		c.it.write("第一行\n第二行\n")
		if got := strip(c.it.rows(40, false)); !reflect.DeepEqual(got, c.want) {
			t.Errorf("rows = %q, want %q", got, c.want)
		}
	}
	if got := strip((&item{kind: kindTool, name: "bash", state: toolOK}).rows(40, false)); !reflect.DeepEqual(got, []string{"✓ bash"}) {
		t.Errorf("没有输出的工具只有头一行，got %q", got)
	}
}

// 工具输出收起时只留末尾几行，前面折成一行说明；展开后全在
func TestToolOutputFolds(t *testing.T) {
	it := item{kind: kindTool, name: "bash"}
	for i := range 12 {
		it.write("out" + string(rune('a'+i)) + "\n")
	}
	want := []string{"🔧 bash …", "   … 前面还有 7 行（ctrl+o 展开）", "   outh", "   outi", "   outj", "   outk", "   outl"}
	if got := strip(it.rows(40, false)); !reflect.DeepEqual(got, want) {
		t.Errorf("收起 = %q, want %q", got, want)
	}
	if full := it.rows(40, true); len(full) != 1+12 {
		t.Errorf("展开应是头一行加 12 行输出，got %d 行", len(full))
	}
}

// 一个字一个字流进来时，每一步画出的都与拿同一段正文从头画的一样（含空行、首行前缀、中途改宽）
func TestStreamedRenderMatchesFresh(t *testing.T) {
	text := []rune("第一行\n\n第二段很长很长很长，要折好几行才放得下\n第三\n\n第四段")
	streamed := item{kind: kindUser}
	for i, r := range text {
		streamed.write(string(r))
		width := 12
		if i == len(text)/2 {
			width = 16 // 中途改一次宽
		}
		fresh := item{kind: kindUser}
		fresh.write(string(text[:i+1]))
		if got, want := streamed.rows(width, false), fresh.rows(width, false); !reflect.DeepEqual(got, want) {
			t.Fatalf("流到第 %d 个字（宽 %d）：\ngot  %q\nwant %q", i, width, strip(got), strip(want))
		}
	}
}

// 中文字与字之间可断，英文按词断，标点不落到行首，超宽的词才从中间劈开
func TestWrap(t *testing.T) {
	cases := []struct {
		in   string
		want []string
	}{
		{"EEVDF 在此基础上加入了截止时间的概念，延迟敏感", []string{"EEVDF 在此基础上加入", "了截止时间的概念，延", "迟敏感"}},
		{"ab ab ab ab ab ab ab ab", []string{"ab ab ab ab ab ab ab", "ab"}},
		{"一二三四五六七八九十，好", []string{"一二三四五六七八九", "十，好"}},
		{"supercalifragilisticexpialidocious ok", []string{"supercalifragilistic", "expialidocious ok"}},
		{"", []string{""}},
	}
	for _, c := range cases {
		got := wrap(c.in, 20)
		if !reflect.DeepEqual(got, c.want) {
			t.Errorf("wrap(%q) = %q, want %q", c.in, got, c.want)
		}
		for _, r := range got {
			if w := ansi.StringWidth(r); w > 20 {
				t.Errorf("wrap(%q) 有一行宽 %d：%q", c.in, w, r)
			}
		}
	}
}

// 终端会自己解释的东西摊平成字面
func TestPrintable(t *testing.T) {
	for in, want := range map[string]string{
		"进度 10%\r进度 100%":         "进度 100%",
		"一行\r":                    "一行",
		"\x1b[31m红\x1b[0m字":       "红字",
		"\x1b[2K\r清行后重画":          "清行后重画",
		"a\tb":                    "a    b",
		"响铃\a退格\b标题\x1b]0;t\x07完": "响铃退格标题完",
	} {
		if got := printable(in); got != want {
			t.Errorf("printable(%q) = %q, want %q", in, got, want)
		}
	}
}
