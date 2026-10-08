// realagent gui 的原生侧：开窗口，告诉网页连谁、自己是谁、用户站在哪（ADR-0028）。
// 与 core 的通信在网页里直连，这里只在退出时替它关组。
package main

import (
	"context"
	"crypto/rand"
	"embed"
	"encoding/hex"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
)

//go:embed all:dist
var assets embed.FS

// Env 是网页启动时向这里要的几样
type Env struct {
	Core     string `json:"core"`
	ClientID string `json:"client_id"`
	Workdir  string `json:"workdir"`
	Setup    bool   `json:"setup"` // REALAGENT_SETUP 非空（make setup-*）：不管引导过没有，先重走一遍
}

// App 绑定给网页：window.go.main.App
type App struct{ env Env }

func (a *App) Env() Env { return a.env }

// 正常退出前关组；断线 60 秒 core 自己也会关，那是兜底（ADR-0021）
func (a *App) closeGroup(context.Context) {
	body := strings.NewReader(fmt.Sprintf(`{"client_id":%q}`, a.env.ClientID))
	hc := http.Client{Timeout: 2 * time.Second}
	if r, err := hc.Post("http://"+a.env.Core+"/group/close", "application/json", body); err == nil {
		r.Body.Close()
	}
}

// core 不猜 workdir（ADR-0019）：REALAGENT_WORKDIR 优先（wails dev 在 gui/ 里跑 app），
// 否则是进程启动时站的地方，与 TUI 同。从 Finder / 开始菜单启动时 cwd 是根目录，那不是用户站的地方，退回家目录
func workdir() string {
	if wd := os.Getenv("REALAGENT_WORKDIR"); wd != "" {
		return wd
	}
	wd, _ := os.Getwd()
	if filepath.Dir(wd) != wd {
		return wd
	}
	if home, err := os.UserHomeDir(); err == nil {
		return home
	}
	return wd
}

func main() {
	core := os.Getenv("REALAGENT_CORE")
	if core == "" {
		core = "127.0.0.1:12345"
	}
	var b [8]byte
	_, _ = rand.Read(b[:])
	app := &App{Env{Core: core, ClientID: hex.EncodeToString(b[:]), Workdir: workdir(), Setup: os.Getenv("REALAGENT_SETUP") != ""}}

	err := wails.Run(&options.App{
		Title:       "realagent",
		Width:       1200,
		Height:      800,
		MinWidth:    380,
		MinHeight:   480,
		AssetServer: &assetserver.Options{Assets: assets},
		Bind:        []any{app},
		OnShutdown:  app.closeGroup,
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "realagent gui 启动失败:", err)
		os.Exit(1)
	}
}
