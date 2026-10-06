// probe — 手工冒烟：起一个 core，把多 agent / 分组 / 历史回放这几条链路走一遍。
//
// 不进 go test：它要一个真的 core 在 12345 上跑着，而单元测试不该依赖外部进程。
package main

import (
	"fmt"
	"os"
	"time"

	"realagent/tui/internal/client"
)

func main() {
	wd, _ := os.Getwd()

	a := client.New("127.0.0.1:12345")
	b := client.New("127.0.0.1:12345")

	if err := a.CreateAgent(wd); err != nil {
		fmt.Println("a 建 agent 失败:", err)
		return
	}
	if err := b.CreateAgent(wd); err != nil {
		fmt.Println("b 建 agent 失败:", err)
		return
	}

	la, _ := a.FetchAgents()
	lb, _ := b.FetchAgents()
	fmt.Printf("清单: a 看到 %d 个 (%d), b 看到 %d 个 (%d)\n",
		len(la), la[0].ID, len(lb), lb[0].ID)

	r, _ := b.SendTo(la[0].ID, "你好")
	fmt.Println("b 发消息:", r.Status)

	// 会话内容：新会话还没落盘，应是空数组而不是错
	h, err := a.FetchSession(a.AgentID())
	fmt.Printf("新 agent 的会话帧: %d 帧, err=%v\n", len(h), err)

	// 会话清单：opened_by 指向自己那个 agent
	ch := make(chan client.Event, 64)
	go a.SubscribeEvents(ch)
	time.Sleep(300 * time.Millisecond)

	a.CloseGroup()
	time.Sleep(200 * time.Millisecond)
	la2, _ := a.FetchAgents()
	lb2, _ := b.FetchAgents()
	fmt.Printf("a 关组后: a=%d b=%d\n", len(la2), len(lb2))
}
