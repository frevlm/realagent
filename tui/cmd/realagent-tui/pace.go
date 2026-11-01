// 推送流的节拍器。
//
// 上游的增量是成团到的：几十个字一团，团与团之间停顿可达一秒，原样上屏就是一卡一卡。
// 所以事件先排队，正文/思考的增量按匀速吐字，其余事件排到队头就放——顺序与到达时一致。
// 新货到时把速度重定为「积压 / paceLag」：积压永远在 paceLag 内吐完，停顿被匀开。
package main

import (
	"encoding/json"
	"time"

	"github.com/charmbracelet/bubbletea"
	"realagent/tui/internal/client"
)

const (
	paceFrame = 16 * time.Millisecond // 吐字的帧间隔，与 Bubble Tea 的 60fps 对齐
	paceLag   = 900 * time.Millisecond
)

type paceMsg time.Time

// queued 是排队中的一帧。runes 非空 = 匀速吐的增量（还没吐的那截），空 = 到队头就放
type queued struct {
	ev    client.Event
	runes []rune
}

type pacer struct {
	q       []queued
	backlog int     // 队里还没吐的字数
	rate    float64 // 字/秒
	budget  float64 // 攒下的可吐字数（不足一个字的零头）
	last    time.Time
	ticking bool // 有且只有一拍在路上；只有 onPace 让它停
}

func paceTick() tea.Cmd {
	return tea.Tick(paceFrame, func(t time.Time) tea.Msg { return paceMsg(t) })
}

// enqueue 收下一帧。只有画进当前对话正文的增量才匀速吐，别的不占节拍
func (m *model) enqueue(ev client.Event, now time.Time) tea.Cmd {
	item := queued{ev: ev}
	if (ev.Type == "message_update" || ev.Type == "thinking_update") && m.mine(ev) {
		item.runes = []rune(deltaOf(ev.Payload))
	}
	p := &m.pace
	p.q = append(p.q, item)
	p.backlog += len(item.runes)
	p.rate = float64(p.backlog) / paceLag.Seconds()
	if p.ticking {
		return nil
	}
	p.ticking, p.last = true, now
	return tea.Batch(m.play(0), paceTick())
}

// play 按攒下的额度放帧：先放队头所有不占节拍的，再吐增量，额度用完就等下一拍
func (m *model) play(budget float64) tea.Cmd {
	p := &m.pace
	p.budget += budget
	var cmds []tea.Cmd
	for len(p.q) > 0 {
		head := &p.q[0]
		n := min(len(head.runes), int(p.budget+1e-6)) // 1e-6：别让 0.9999… 个字的浮点零头卡住最后一个字
		if len(head.runes) > 0 && n == 0 {
			break
		}
		ev := head.ev
		if n > 0 {
			ev.Payload = withDelta(ev.Payload, string(head.runes[:n]))
		}
		if head.runes = head.runes[n:]; len(head.runes) == 0 {
			p.q = p.q[1:]
		}
		p.budget -= float64(n)
		p.backlog -= n
		cmds = append(cmds, m.handleEvent(ev))
	}
	if len(p.q) == 0 {
		p.budget = 0 // 断货期间不攒额度，下一团来了从零起步
	}
	return tea.Batch(cmds...)
}

// onPace 是一拍：按真实流逝的时间折算额度
func (m *model) onPace(t time.Time) tea.Cmd {
	p := &m.pace
	dt := t.Sub(p.last)
	p.last = t
	cmd := m.play(p.rate * dt.Seconds())
	if p.ticking = len(p.q) > 0; p.ticking {
		return tea.Batch(cmd, paceTick())
	}
	return cmd
}

// flush 把队里的全放出来（Esc 中断时：收到多少就立刻看到多少）
func (m *model) flush() tea.Cmd {
	return m.play(float64(m.pace.backlog))
}

// withDelta 换掉载荷里的 delta，其余字段（root / session_id）原样留着给 handleEvent 筛
func withDelta(payload, delta string) string {
	var o map[string]json.RawMessage
	jsonUnmarshal(payload, &o)
	o["delta"], _ = json.Marshal(delta)
	b, _ := json.Marshal(o)
	return string(b)
}
