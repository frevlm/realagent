//go:build !windows

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
	"time"

	"realagent/tui/internal/client"
)

// startCore 连不上 core 就在后台拉起 <本程序目录>/core/realagent-core，最多等 5 秒
func startCore(c *client.Client) {
	if _, err := c.FetchSetup(); err == nil {
		return
	}
	exe, _ := os.Executable()
	home, _ := os.UserHomeDir()
	log, _ := os.OpenFile(filepath.Join(home, ".realagent", "core.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	cmd := exec.Command(filepath.Join(filepath.Dir(exe), "core", "realagent-core"))
	cmd.Stdout, cmd.Stderr = log, log
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if cmd.Start() != nil {
		return
	}
	for i := 0; i < 50; i++ {
		if _, err := c.FetchSetup(); err == nil {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
}
