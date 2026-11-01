// 端到端：跑真的 Bubble Tea 程序，验证一整回合的事件确实画到了屏幕上，
// 且每行只画一次。
//
// altscreen 之后屏幕每帧全量重绘（ADR-0020），所以断言的是**最后一帧**：
// 输出流里前面那些是被覆盖掉的中间帧，拿整个流去数出现次数只会数到重绘次数。
//
// 单元测试只能证明行流对；这一层证明历史区通路对。
package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/x/ansi"

	"realagent/tui/internal/client"
)

// safeBuf 是并发安全的输出缓冲（渲染在自己的 goroutine 里写）
type safeBuf struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *safeBuf) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *safeBuf) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

// headless 让模型脱离 core 跑起来（Init 不订阅推送流，事件由测试直接 Send）
type headless struct{ model }

func (h headless) Init() tea.Cmd { return nil }

func (h headless) Update(msg tea.Msg) (tea.Model, tea.Cmd) { return h.model.Update(msg) }

func (h headless) View() string { return h.model.View() }

func TestAltscreenPaintsWholeTurn(t *testing.T) {
	var out safeBuf
	p := tea.NewProgram(headless{testModel()},
		tea.WithInput(nil),
		tea.WithOutput(&out),
		tea.WithoutSignalHandler(),
		tea.WithAltScreen(),
	)

	done := make(chan struct{})
	go func() {
		defer close(done)
		p.Run()
	}()

	p.Send(tea.WindowSizeMsg{Width: 40, Height: 24})
	// 一个完整回合：用户提问 → 流式回答（多段）→ 工具 → 收工。
	// 用户那一行也从 core 来（message_start 带正文），本地不回显
	p.Send(eventMsg(client.Event{Type: "message_start", Payload: `{"role":"user","text":"问一句"}`}))
	p.Send(eventMsg(client.Event{Type: "turn_start", Payload: `{}`}))
	for _, delta := range []string{"第一段回答。\n", "第二段回答，", "它比较长需要折行折行折行。\n"} {
		payload, _ := json.Marshal(map[string]string{"delta": delta})
		p.Send(eventMsg(client.Event{Type: "message_update", Payload: string(payload)}))
	}
	p.Send(eventMsg(client.Event{Type: "tool_execution_start", Payload: `{"name":"bash"}`}))
	p.Send(eventMsg(client.Event{Type: "tool_execution_end", Payload: `{"name":"bash","status":0}`}))
	p.Send(eventMsg(client.Event{Type: "turn_end", Payload: `{"tool_uses":0}`}))

	time.Sleep(200 * time.Millisecond) // 让渲染器把帧刷出去
	p.Quit()
	<-done

	got := ansi.Strip(out.String())
	for _, want := range []string{"问一句", "第一段回答。", "第二段回答", "🔧 bash", "✓ bash"} {
		if !strings.Contains(got, want) {
			t.Errorf("屏幕应含 %q\n实际输出:\n%s", want, got)
		}
	}
}

// 最后一帧里每行只出现一次——重复即说明历史被画了两遍
func TestAltscreenNoDuplicateLines(t *testing.T) {
	m := testModel()
	m.width, m.height = 40, 24
	m.handleEvent(client.Event{Type: "message_start", Payload: `{"role":"user","text":"问一句"}`})
	m.handleEvent(client.Event{Type: "message_update", Payload: `{"delta":"答一句"}`})
	m.closeLine()

	screen := ansi.Strip(m.View())
	for _, want := range []string{"问一句", "答一句"} {
		if n := strings.Count(screen, want); n != 1 {
			t.Errorf("%q 应只出现 1 次，got %d\n屏幕:\n%s", want, n, screen)
		}
	}
}
