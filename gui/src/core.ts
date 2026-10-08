// 与 core 的通信：网页直连，请求走 fetch，推送走 WebSocket（ADR-0028）。
// 连谁、client_id、workdir 三样由 Go 侧（main.go）给。协议的唯一真相是 docs/PROTOCOL.md，下面的类型照它抄。

export interface Agent {
  id: number;
  workdir: string;
  state: string;
  session_id?: string;
  in_edges?: number[];
  out_edges?: number[];
}

export interface Session {
  id: string;
  title: string;
  messages: number;
  mtime: number;
  opened_by: number | null;
}

export interface Command {
  name: string;
  description?: string;
  argument_hint?: string;
  kind: "builtin" | "prompt";
}

export interface Model {
  name: string;
  owned_by?: string;
  context?: number;
  current?: boolean;
}

export interface Approval {
  id: string;
  agent_id: number;
  tool: string;
  params?: Record<string, unknown>;
}

export interface Statusline {
  model?: string;
  owned_by?: string;
  context?: number;
}

// POST /agent、/session、/command、/message 的回执
export interface Reply<T = unknown> {
  ok?: boolean;
  error?: string;
  command?: string;
  data?: T;
  agent_id?: number;
}

// 推送帧与 GET /session 回放同形（回放用 type 作键，推送用 event，读进来时统一成 event）
type Of = { agent_id?: number };
export type Frame =
  | { event: "message_start"; data: Of & { role?: string; text?: string } }
  | { event: "message_update" | "thinking_update"; data: Of & { delta?: string } }
  | { event: "message_end" | "thinking_start" | "thinking_stop" | "turn_start" | "agent_start" | "interrupted"; data: Of }
  | { event: "turn_end"; data: Of & { error?: string } }
  | { event: "tool_execution_start"; data: Of & { name: string; id: string } }
  | { event: "tool_execution_end"; data: Of & { name: string; id: string; status?: number; interrupted?: boolean } }
  | { event: "tool_output"; data: Of & { call_id: string; stream?: string; text?: string } }
  | { event: "status_update"; data: Of & { cost?: number } }
  | { event: "statusline"; data: Statusline }
  | { event: "permission_request"; data: Approval }
  | { event: "agent_end"; data: Of & { cost?: number; recap?: string } };

export interface Replayed {
  type: Frame["event"];
  data: Frame["data"];
}

export type Method = "GET" | "POST";

interface Env {
  core: string;      // host:port
  client_id: string; // 一个进程一个，网页重载不换组（ADR-0021）
  workdir: string;
  setup: boolean;    // make setup-*：不管引导过没有，先重走一遍
}

declare global {
  interface Window {
    go: { main: { App: { Env(): Promise<Env> } } };
    runtime: { BrowserOpenURL(url: string): void };
  }
}

const env = window.go.main.App.Env();

// 参数一律是 JSON，client_id 在这里补。浏览器发不出带体的 GET，GET 的那份 JSON 放进查询串 ?body=
export async function call<T>(method: Method, path: string, body: object = {}): Promise<T> {
  const { core, client_id } = await env;
  const json = JSON.stringify({ ...body, client_id });
  const url = `http://${core}${path}`;
  const r = method === "GET"
    ? await fetch(`${url}?${new URLSearchParams({ body: json })}`)
    : await fetch(url, { method, headers: { "Content-Type": "application/json" }, body: json });
  return r.json();
}

export const workdir = async () => (await env).workdir;

export const setupForced = async () => (await env).setup;

export const openUrl = (url: string) => window.runtime.BrowserOpenURL(url);

// 推送流只开一条，断了 1.5 秒后重连
export async function subscribe(onFrame: (f: Frame) => void, onConn: (ok: boolean) => void) {
  const { core, client_id } = await env;
  const open = () => {
    const ws = new WebSocket(`ws://${core}/events?client_id=${client_id}`);
    ws.onopen = () => onConn(true);
    ws.onmessage = (m) => onFrame(JSON.parse(m.data));
    ws.onclose = () => {
      onConn(false);
      setTimeout(open, 1500);
    };
  };
  open();
}
