// probe — 手工冒烟：起一个 core，把对话 / 分组 / 历史回放这几条链路走一遍。
//
// 不进 go test：它要一个真的 core 在 12345 上跑着（端点也得配齐，不然发不出消息），
// 而单元测试不该依赖外部进程。
package main

import (
	"fmt"
	"os"
	"time"

	"realagent/tui/internal/client"
)

func main() {
	wd, _ := os.Getwd()

	a := client.New("127.0.0.1:12345", wd)
	b := client.New("127.0.0.1:12345", wd)

	ch := make(chan client.Event, 256)
	go a.SubscribeEvents(ch)
	time.Sleep(300 * time.Millisecond)

	// 新对话：第一条消息发出去，core 才开出它
	sid := a.SessionID()
	if r, err := a.Send("你好"); err != nil || r.Error != "" {
		fmt.Println("a 开对话失败:", err, r.Error)
		return
	}
	fmt.Println("a 开出对话:", sid)

	// 同一段对话在 a 那边开着，b 不许再开一个往同一个文件里写
	b.Use(sid)
	rb, _ := b.Send("插一句")
	fmt.Println("b 抢 a 的对话:", rb.Error)

	time.Sleep(time.Second)
	la, _ := a.FetchSessions()
	for _, s := range la {
		if s.ID == sid {
			fmt.Printf("清单里有它: %q state=%q\n", s.Title, s.State)
		}
	}
	h, err := a.FetchSession(sid)
	fmt.Printf("回放: %d 帧, err=%v\n", len(h), err)

	_ = a.Interrupt()
	a.CloseGroup()
	time.Sleep(200 * time.Millisecond)
	rb2, _ := b.Send("现在能接着说了")
	fmt.Println("a 关组后 b 接手:", rb2.Status, rb2.Error)
	b.CloseGroup()
}
