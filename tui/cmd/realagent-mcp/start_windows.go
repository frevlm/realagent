package main

import "realagent/tui/internal/client"

// startCore 在 Windows 上什么都不做：core 是 POSIX 的，跑在 WSL 里，桥连 127.0.0.1:12345 那一个
func startCore(*client.Client) {}
