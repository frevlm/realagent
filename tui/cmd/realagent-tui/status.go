// 读秒状态行（输入框上方）：模型在干什么 + 已耗时，参考 claude code。
//
// 数据结构决定一切：整个特性只有 activity 一个状态，active=false 即空闲，
// 渲染层与事件层都不需要额外的特殊判断分支。
package main

import (
	"fmt"
	"time"

	"github.com/charmbracelet/bubbletea"
)

// 状态行刷新周期（spinner 转动帧率；读秒精度 1s 足够，120ms 只为动画顺滑）
const statusTickInterval = 120 * time.Millisecond

var spinnerFrames = []string{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}

// activity 是"模型正在干活"的唯一状态源。
// start 跨 turn 连续（工具执行 → 下一轮 LLM 调用是同一次等待，读秒不重置），
// 直到 agent_end 收工才停——turn 结束不是收工，收工判定不通过就还有下一轮。
type activity struct {
	active bool
	verb   string    // 当前动作：思考中 / 生成回复 / 执行工具 bash / 等待审批
	start  time.Time // 本次等待的起点（读秒基准）
	now    time.Time // 最近一次 tick 时间（elapsed = now - start）
	seq    int       // tick 代号：只有 seq 相同的 tick 续命，杜绝并发计时循环
	frame  int       // spinner 帧序号
	cost   cost      // 本次 run 的累计花费（core 的 status_update 帧，绝对值覆盖写）
}

// cost 是 status_update 帧的载荷（PROTOCOL.md：本次 run 累计、绝对值）。
// 钱由 core 按模型数据表算好报上来，TUI 不碰单价（ADR-0009）。
// 零 = 没数据（表里没这个模型或端点不报用量），渲染时整段隐藏，绝不显示 $0。
type cost struct {
	USD float64 `json:"cost"`
}

func (c cost) empty() bool { return c.USD == 0 }

// text 渲染为 "$0.0123"：四位小数，够看清一次对话花了多少
func (c cost) text() string {
	if c.empty() {
		return ""
	}
	return fmt.Sprintf("$%.4f", c.USD)
}

// tickMsg 是状态行的定时刷新（seq 标识所属计时循环）
type tickMsg struct {
	seq int
	t   time.Time
}

func tickCmd(seq int) tea.Cmd {
	return tea.Tick(statusTickInterval, func(t time.Time) tea.Msg {
		return tickMsg{seq: seq, t: t}
	})
}

// begin 开始（或切换）一次等待。已在计时则只换措辞，不重置读秒、不再起计时循环。
func (a *activity) begin(verb string, now time.Time) tea.Cmd {
	a.verb = verb
	if a.active {
		return nil
	}
	a.active = true
	a.start = now
	a.now = now
	a.frame = 0
	a.cost = cost{} // 新一次 run，花费跟读秒同一个重置点
	a.seq++
	return tickCmd(a.seq)
}

// stop 结束等待。计时循环在下一个 tick 自然消亡（seq 或 active 不匹配即不续命）。
func (a *activity) stop() {
	a.active = false
}

// tick 推进动画并续命。过期 tick（旧 seq / 已停止）返回 nil，循环终止。
func (a *activity) tick(msg tickMsg) tea.Cmd {
	if !a.active || msg.seq != a.seq {
		return nil
	}
	a.now = msg.t
	a.frame++
	return tickCmd(a.seq)
}

func (a activity) elapsed() time.Duration {
	if a.now.Before(a.start) {
		return 0
	}
	return a.now.Sub(a.start)
}

// elapsedText 读秒文本：1 分钟内 "12s"，超过则 "1m05s"
func elapsedText(d time.Duration) string {
	secs := int(d.Seconds())
	if secs < 60 {
		return fmt.Sprintf("%ds", secs)
	}
	return fmt.Sprintf("%dm%02ds", secs/60, secs%60)
}

// render 渲染状态行；空闲返回 ""（调用方按空串跳过，无需判空逻辑）。
// 窄终端按"提示 → 花费 → 括号全丢"逐级降级，保证 spinner 与动作词永远可见。
func (a activity) render(width int) string {
	if !a.active {
		return ""
	}
	spin := spinnerFrames[a.frame%len(spinnerFrames)]
	head := spin + " " + a.verb + "…"
	elapsed := elapsedText(a.elapsed())

	tails := []string{
		fmt.Sprintf(" (%s · %s · esc 中断)", elapsed, a.cost.text()),
		fmt.Sprintf(" (%s · %s)", elapsed, a.cost.text()),
		fmt.Sprintf(" (%s · esc 中断)", elapsed),
		fmt.Sprintf(" (%s)", elapsed),
		"",
	}
	if a.cost.empty() {
		tails = tails[2:]
	}
	tail := tails[len(tails)-1]
	for _, t := range tails {
		if width <= 0 || len([]rune(head+t)) <= width {
			tail = t
			break
		}
	}
	return spinnerStyle.Render(spin) + " " + statusStyle.Render(a.verb+"…") + dimStyle.Render(tail)
}

// toolVerb 把工具名折成动作词（空名兜底为通用措辞）
func toolVerb(name string) string {
	if name == "" {
		return "执行工具"
	}
	return "执行工具 " + name
}
