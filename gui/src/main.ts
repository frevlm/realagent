// realagent gui —— core 的桌面客户端。协议见 docs/PROTOCOL.md，传输见 ./core.ts。
//
// 推送流与 GET /session 回放是同形的帧，走同一个 handle()：实时看与翻历史长得一样（ADR-0020）。
// 用户看见的只有对话，没有 agent（ADR-0029）：只渲染当前那段对话的帧，换一段就清空、从 GET /session 重读；
// 它派生出去的子 agent 画进可折叠的块里，挂在派生它的那张工具卡片下面。审批例外：不管在看哪段都要弹。

import "./app.css";
import * as core from "./core";
import type { Approval, Command, Frame, Model, Replayed, Reply, Session, Statusline, Who } from "./core";
import { $, ago, basename, el, esc, ic, q, shortPath, toast, type Icon } from "./dom";
import { md } from "./md";
import { setup, type Settings } from "./setup";

const S = {
  sessionId: "",               // 当前那段对话。新对话的 id 在这里生成，第一条消息到了 core 才开出它
  sessions: [] as Session[],   // GET /sessions
  workdir: "",                 // 对话在哪个目录里开
  commands: [] as Command[],   // GET /commands + 本地的 /new /resume
  approvals: [] as Approval[], // 挂起的 permission_request
  since: 0, verb: "", cost: null as number | null, timer: 0, interrupted: false,
  slashSel: 0, slashHidden: false,
};

// 所有请求都带「在哪、说的是哪段」；端点不认的键 core 不看
const call = <T>(method: core.Method, path: string, body: object = {}) =>
  core.call<T>(method, path, { workdir: S.workdir, session_id: S.sessionId, ...body });

// 换对话不需要 core 做任何事，所以这两条是本地的
const LOCAL: Command[] = [
  { name: "new", description: "开一段新对话（当前这段留在盘上）", kind: "builtin" },
  { name: "resume", description: "换到另一段对话", kind: "builtin" },
];

// ==================== 对话流 ====================

interface Text { node: HTMLElement; buf: string }
interface Think { node: HTMLDetailsElement; body: HTMLElement; start: number; replay: boolean }

// 一块往里长东西的地方：主对话是一块，每个子 agent 各一块。正在长的思考、正文、工具卡片各归各块
interface Pane {
  host: HTMLElement;
  think: Think | null;
  text: Text | null;
  tools: Map<string, HTMLElement>;
  lastTool: HTMLElement | null;
}
const pane = (host: HTMLElement): Pane => ({ host, think: null, text: null, tools: new Map(), lastTool: null });

const thread = $("thread");
const wrap = $("thread-wrap");
const main = pane(thread);
const subs = new Map<string, { node: HTMLDetailsElement; pane: Pane; loaded: boolean }>(); // 子 agent 的 session_id → 它那一块

let stick = true; // 贴着底就继续贴底；用户滚上去了就不拽回来
wrap.addEventListener("scroll", () => {
  stick = wrap.scrollHeight - wrap.scrollTop - wrap.clientHeight < 80;
  q(document, ".main").classList.toggle("scrolled", wrap.scrollTop > 0);
});
const follow = () => { if (stick) wrap.scrollTop = wrap.scrollHeight; };
new ResizeObserver(follow).observe(wrap); // 审批卡、多行输入把底栏撑高时，别把末尾挤到下面去

function add<T extends HTMLElement>(p: Pane, node: T): T {
  p.host.append(node);
  $("empty").hidden = true;
  follow();
  return node;
}

function clearThread() {
  thread.textContent = "";
  Object.assign(main, pane(thread));
  subs.clear();
  $("empty").hidden = false;
  stick = true;
}

const notice = (text: string) => add(main, el("div", "notice", esc(text)));

function alertMsg(p: Pane, text: string) {
  add(p, el("div", "alert alert-danger msg", ic("alert"))).append(text);
}

function userMsg(p: Pane, text: string) {
  const m = el("div", "msg msg-user");
  const b = el("div", "bubble-user");
  b.textContent = text;
  m.append(b);
  add(p, m);
  if (p === main) stick = true;
  follow();
}

// 正文流式到达：攒进 buf，下一帧再整段重排（折行与代码块都是整段才判得准）
let raf = 0;
const dirty = new Set<Text>();
function paint() {
  raf = 0;
  for (const t of dirty) t.node.innerHTML = md(t.buf);
  dirty.clear();
  follow();
}

function textAppend(p: Pane, delta: string, replay: boolean) {
  if (!p.text) p.text = { node: add(p, el("div", "prose msg")), buf: "" };
  p.text.buf += delta;
  if (replay) return;
  dirty.add(p.text);
  if (!raf) raf = requestAnimationFrame(paint);
}

function endText(p: Pane) {
  if (!p.text) return;
  dirty.delete(p.text);
  p.text.node.innerHTML = md(p.text.buf);
  p.text = null;
}

function thinkAppend(p: Pane, delta: string, replay: boolean) {
  if (!p.think) {
    const d = el("details", "think msg", `<summary>${ic("sparkles")}<span>思考中</span>${ic("chev", "chev")}</summary><div class="think-body"></div>`);
    d.open = !replay;
    p.think = { node: add(p, d), body: q(d, ".think-body"), start: Date.now(), replay };
  }
  p.think.body.textContent += delta;
  follow();
}

function endThink(p: Pane) {
  const t = p.think;
  if (!t) return;
  const secs = Math.round((Date.now() - t.start) / 1000);
  q(t.node, "summary span").textContent = t.replay || secs < 1 ? "思考过程" : `思考了 ${secs} 秒`;
  t.node.open = false;
  p.think = null;
}

function endAll(p: Pane) { endThink(p); endText(p); }

// ==================== 工具卡片 ====================

const TOOLS: Record<string, [Icon, string]> = {
  bash: ["terminal", "执行命令"], read: ["file", "读文件"], edit: ["pencil", "改文件"],
  search: ["search", "搜代码"], spawn: ["fork", "派生子任务"], send_message: ["send", "发消息"],
};
const toolIcon = (n: string): Icon => TOOLS[n]?.[0] ?? "wrench";
const toolVerb = (n: string) => TOOLS[n]?.[1] ?? `调用 ${n}`;

// 卡片头一行写什么：审批帧带着参数，挑人最想看的那个键
function toolSummary(params: Approval["params"]): string {
  if (!params || typeof params !== "object") return "";
  for (const k of ["command", "file_path", "pattern", "text", "task"]) if (params[k]) return String(params[k]);
  return JSON.stringify(params);
}

type ToolState = "run" | "wait" | "ok" | "fail" | "stop";
const TOOL_STATES: Record<ToolState, [string, string, string]> = {
  run: ["", ic("loader", "spin"), "运行中"],
  wait: ["wait", ic("shield"), "等待审批"],
  ok: ["ok", ic("check"), "完成"],
  fail: ["fail", ic("x"), "失败"],
  stop: ["wait", ic("x"), "已中断"],
};

function setToolState(card: HTMLElement, state: ToolState) {
  const s = q(card, ".tool-state");
  const [cls, icon, text] = TOOL_STATES[state];
  s.className = "tool-state " + cls;
  s.innerHTML = icon + text;
  card.dataset.state = state;
}

function toolStart(p: Pane, d: { name: string; id: string }) {
  const card = el("div", "tool msg",
    `<button class="tool-head"><span class="tool-icon">${ic(toolIcon(d.name))}</span><span class="tool-name"></span><span class="tool-label"></span>` +
    `<span class="tool-state"></span>${ic("chev", "chev")}</button><pre class="tool-out"></pre>`);
  q(card, ".tool-name").textContent = d.name;
  q(card, ".tool-head").onclick = () => card.classList.toggle("open");
  card.dataset.name = d.name;
  setToolState(card, "run");
  p.tools.set(d.id, card);
  p.lastTool = card;
  add(p, card);
}

// 按行推但不保证一帧一行：续写即可，不假设边界
function toolOut(p: Pane, d: { call_id: string; text?: string }, replay: boolean) {
  const card = p.tools.get(d.call_id) ?? p.lastTool;
  if (!card) return;
  const out = q(card, ".tool-out");
  out.textContent += d.text ?? "";
  if (!replay) card.classList.add("open");
  out.scrollTop = out.scrollHeight;
  follow();
  // 回放时 spawn 的结果里写着子 agent 的会话：在卡片下面挂一块，点开再读
  const sid = card.dataset.name === "spawn" && d.text?.match(/\(session (\S+)\)/)?.[1];
  if (sid) subBlock(sid, card);
}

function toolEnd(p: Pane, d: { id: string; status?: number; interrupted?: boolean }) {
  const card = p.tools.get(d.id) ?? p.lastTool;
  if (!card) return;
  const out = q(card, ".tool-out");
  out.textContent = (out.textContent ?? "").replace(/\n+$/, "");
  setToolState(card, d.interrupted ? "stop" : d.status ? "fail" : "ok");
  // 成功的收起来，失败的留着给人看
  if (!d.status && !d.interrupted) card.classList.remove("open");
}

// ==================== 子 agent ====================

// 子 agent 那一块。实时：它的第一帧到了才建，挂在正跑着的 spawn 卡片下面；回放：spawn 的结果里认出来
function subBlock(sid: string, after?: HTMLElement) {
  const have = subs.get(sid);
  if (have) return have;
  const node = el("details", "sub msg",
    `<summary>${ic("fork")}<span class="sub-title">子任务</span><span class="sub-state"></span>${ic("chev", "chev")}</summary><div class="sub-body"></div>`);
  const anchor = after ?? (main.lastTool?.dataset.name === "spawn" ? main.lastTool : null);
  if (anchor) anchor.after(node);
  else add(main, node);
  const b = { node, pane: pane(q(node, ".sub-body")), loaded: !after };
  // 回放出来的块是空的：第一次点开才去读它的会话
  if (after) node.addEventListener("toggle", () => { if (node.open && !b.loaded) loadSub(sid, b); });
  subs.set(sid, b);
  follow();
  return b;
}

async function loadSub(sid: string, b: { pane: Pane; loaded: boolean }) {
  b.loaded = true;
  const frames = await call<Replayed[]>("GET", "/session", { session_id: sid });
  if (Array.isArray(frames)) for (const f of frames) render(b.pane, { event: f.type, data: f.data } as Frame, true);
  endAll(b.pane);
}

function subState(b: { node: HTMLDetailsElement }, text: string) {
  q(b.node, ".sub-state").textContent = text;
}

// ==================== 读秒行 ====================

function busy(verb: string) {
  if (!S.since) S.since = Date.now();
  S.verb = verb;
  if (!S.timer) S.timer = setInterval(renderStatus, 1000);
  renderStatus();
}

function idle() {
  S.since = 0;
  S.cost = null;
  S.interrupted = false;
  clearInterval(S.timer);
  S.timer = 0;
  renderStatus();
}

const elapsed = () => Math.floor((Date.now() - S.since) / 1000);
const money = (c: number) => `$${c.toFixed(4)}`;

function renderStatus() {
  const on = !!S.since;
  $("status").hidden = !on;
  $("send").hidden = on;
  $("stop").hidden = !on;
  if (!on) return;
  let s = `${S.verb} · ${elapsed()} 秒`;
  if (S.cost != null) s += ` · ${money(S.cost)}`;
  $("status-text").textContent = s;
}

// ==================== 事件 ====================

function handle(f: Frame, replay = false) {
  const { root, session_id: sid } = f.data as Who;
  if (root && sid === root && (f.event === "agent_start" || f.event === "agent_end")) setRunning(root, f.event === "agent_start");
  if (f.event === "permission_request") return addApproval(f.data);
  if (f.event === "statusline") return setModel(f.data.model);
  if (!root) return render(main, f, replay); // 回放出来的帧：已经按对话取过了
  if (root !== S.sessionId) return;
  if (sid && sid !== root) return render(subBlock(sid).pane, f, replay);
  render(main, f, replay);
}

// 一帧画进一块。读秒、收工、回顾这些只归主对话：子 agent 收工不是这段对话收工
function render(p: Pane, f: Frame, replay: boolean) {
  const top = p === main && !replay;
  const sub = [...subs.values()].find((b) => b.pane === p);
  switch (f.event) {
    case "message_start":
      endAll(p);
      if (!f.data.text) break;
      if (sub && !sub.pane.host.childElementCount) q(sub.node, ".sub-title").textContent = f.data.text.split("\n")[0];
      userMsg(p, f.data.text);
      break;
    case "agent_start":
    case "turn_start":
    case "thinking_start":
      endText(p);
      if (top) busy("思考中");
      if (sub && !replay) subState(sub, "运行中");
      break;
    case "thinking_update":
      thinkAppend(p, f.data.delta ?? "", replay);
      break;
    case "thinking_stop":
      endThink(p);
      break;
    case "message_update":
      endThink(p);
      textAppend(p, f.data.delta ?? "", replay);
      if (top) busy("生成回复");
      break;
    case "message_end":
      endText(p);
      break;
    case "tool_execution_start":
      endAll(p);
      toolStart(p, f.data);
      if (top) busy(toolVerb(f.data.name));
      break;
    case "tool_output":
      toolOut(p, f.data, replay);
      break;
    case "tool_execution_end":
      toolEnd(p, f.data);
      if (top) busy("思考中");
      break;
    case "status_update":
      if (p !== main) break;
      if (f.data.cost != null) S.cost = f.data.cost;
      renderStatus();
      break;
    case "turn_end":
      endAll(p);
      if (f.data.error) alertMsg(p, f.data.error);
      break;
    case "interrupted":
      endAll(p);
      for (const c of p.tools.values()) if (c.dataset.state === "run" || c.dataset.state === "wait") setToolState(c, "stop");
      if (sub) subState(sub, "已中断");
      if (p !== main) break;
      S.interrupted = true;
      dropApprovals(S.sessionId);
      break;
    case "agent_end": {
      endAll(p);
      if (sub) { subState(sub, "完成"); break; }
      if (f.data.recap) add(main, el("div", "recap msg", '<div class="recap-head">回顾</div>')).append(f.data.recap);
      const parts = [S.interrupted ? "已中断" : "完成"];
      if (S.since) parts.push(`${elapsed()} 秒`);
      if (f.data.cost) parts.push(money(f.data.cost));
      notice(parts.join(" · "));
      idle();
      break;
    }
  }
}

// ==================== 审批 ====================

// 审批卡片挂在哪张工具卡片上：当前对话本身，或它的某个子 agent；别的对话的没有卡片可挂
function paneOf(w: Who): Pane | undefined {
  if (!w.root || w.root !== S.sessionId) return undefined;
  return w.session_id === w.root ? main : subs.get(w.session_id ?? "")?.pane;
}

function addApproval(a: Approval) {
  S.approvals.push(a);
  const p = paneOf(a);
  if (p && p.lastTool?.dataset.state === "run") {
    q(p.lastTool, ".tool-label").textContent = toolSummary(a.params);
    setToolState(p.lastTool, "wait");
    if (p === main) busy("等待你的审批");
  }
  renderApprovals();
}

// 中断是整段对话的事：它和它派生出去的一起停，挂着的审批一起掐掉
function dropApprovals(root: string) {
  S.approvals = S.approvals.filter((a) => a.root !== root);
  renderApprovals();
}

function whoAsks(a: Approval): string {
  if (a.root !== S.sessionId) return "另一段对话";
  return a.session_id === a.root ? "" : "子任务";
}

function renderApprovals() {
  const box = $("approvals");
  box.textContent = "";
  for (const a of S.approvals) {
    const card = el("div", "approval",
      `<div class="approval-head">${ic("shield")}<span></span><span class="who"></span></div><pre class="mono"></pre>` +
      `<div class="approval-actions"><button class="btn btn-secondary btn-sm">拒绝</button><button class="btn btn-primary btn-sm">允许</button></div>`);
    q(card, ".approval-head span").textContent = `${toolVerb(a.tool)}需要你的允许`;
    q(card, ".who").textContent = [whoAsks(a), a.tool].filter(Boolean).join(" · ");
    q(card, "pre").textContent = typeof a.params?.command === "string" ? a.params.command : JSON.stringify(a.params, null, 2);
    const [deny, allow] = card.querySelectorAll("button");
    deny.onclick = () => decide(a, false);
    allow.onclick = () => decide(a, true);
    box.append(card);
  }
}

async function decide(a: Approval, allow: boolean) {
  S.approvals = S.approvals.filter((x) => x !== a);
  renderApprovals();
  const p = paneOf(a);
  if (p && p.lastTool?.dataset.state === "wait") {
    setToolState(p.lastTool, allow ? "run" : "fail");
    if (p === main) busy(allow ? toolVerb(a.tool) : "思考中");
  }
  const r = await call<Reply>("POST", "/approval-response", { id: a.id, allow });
  if (r.error) toast("审批回传失败：" + r.error);
}

// ==================== 对话（侧栏） ====================

async function refreshSessions() {
  const list = await call<Session[]>("GET", "/sessions");
  if (!Array.isArray(list)) return;
  S.sessions = list;
  renderSessions();
}

// 侧栏的运行态看这个目录下的所有对话，不只看当前这段
function setRunning(root: string, on: boolean) {
  const s = S.sessions.find((x) => x.id === root);
  if (s) s.state = on ? "running" : "";
  renderSessions();
  if (!s || !on) refreshSessions(); // 刚开出来的对话；跑完了条数也变了
}

function renderSessions() {
  const nav = $("session-list");
  nav.textContent = "";
  if (!S.sessions.length) nav.innerHTML = '<div class="side-note">还没有对话</div>';
  for (const s of S.sessions) {
    const mine = s.id === S.sessionId;
    const elsewhere = s.state === "elsewhere";
    const b = el("button", "nav-item" + (mine ? " active" : ""),
      `<span class="agent-dot ${s.state === "running" ? "running" : ""}"></span>` +
      '<span class="grow"><span class="line1"></span><span class="line2"></span></span>');
    q(b, ".line1").textContent = s.title || "空对话";
    q(b, ".line2").textContent = elsewhere ? "在别的窗口里开着" : `${s.messages} 条 · ${ago(s.mtime)}`;
    b.disabled = elsewhere;
    if (!mine && !elsewhere) b.onclick = () => openConversation(s.id);
    nav.append(b);
  }
  renderTop();
}

function renderTop() {
  const s = S.sessions.find((x) => x.id === S.sessionId);
  $("top-name").textContent = s?.title || "新对话";
  $("top-dir-text").textContent = shortPath(S.workdir);
  $("top-dir").title = S.workdir;
  $("empty-dir").textContent = basename(S.workdir);
  $("top-state").hidden = s?.state !== "running";
}

async function loadHistory() {
  clearThread();
  const frames = await call<Replayed[]>("GET", "/session");
  if (Array.isArray(frames)) for (const f of frames) handle({ event: f.type, data: f.data } as Frame, true);
  endAll(main);
  wrap.scrollTop = wrap.scrollHeight;
}

// 换一段对话：原来那段在 core 里照跑，回来时从盘上读得到
async function openConversation(id: string) {
  S.sessionId = id;
  idle();
  renderSessions();
  closeSidebar();
  await loadHistory();
  if (S.sessions.find((x) => x.id === id)?.state === "running") busy("运行中");
  input.focus();
}

function newConversation() {
  S.sessionId = crypto.randomUUID();
  idle();
  clearThread();
  renderSessions();
  closeSidebar();
  input.focus();
}

async function refreshCommands() {
  const c = await call<Command[]>("GET", "/commands");
  S.commands = [...LOCAL, ...(Array.isArray(c) ? c : [])];
}

// ==================== 模型 ====================

function setModel(name?: string) { $("model-name").textContent = name || "未配置模型"; }

async function openModelMenu() {
  const menu = $("model-menu");
  if (!menu.hidden) return void (menu.hidden = true);
  menu.innerHTML = '<div class="menu-empty">读取中…</div>';
  menu.hidden = false;
  const r = await call<Reply<Model[]>>("POST", "/command", { command: "/model" });
  if (!r.ok) { menu.hidden = true; return toast(r.error || "读不到模型清单"); }
  menu.textContent = "";
  if (!r.data?.length) menu.innerHTML = '<div class="menu-empty">模型数据表是空的</div>';
  for (const m of r.data ?? []) {
    const b = el("button", "menu-item model-item",
      `<span class="check">${m.current ? ic("check") : ""}</span><span class="grow mono"></span><span class="text-3"></span>`);
    q(b, ".grow").textContent = m.name;
    q(b, ".text-3").textContent = m.owned_by ?? "";
    b.onclick = async () => {
      menu.hidden = true;
      const x = await call<Reply>("POST", "/command", { command: "/model " + m.name });
      if (!x.ok) toast(x.error || "切换失败");
    };
    menu.append(b);
  }
}

// ==================== 输入框与斜杠菜单 ====================

const input = $<HTMLTextAreaElement>("input");

function autosize() {
  input.style.height = "auto";
  input.style.height = Math.min(input.scrollHeight, 240) + "px";
}

function slashMatches(): Command[] {
  const v = input.value;
  if (S.slashHidden || !v.startsWith("/") || /\s/.test(v)) return [];
  return S.commands.filter((c) => c.name.startsWith(v.slice(1)));
}

function renderSlash() {
  const list = slashMatches();
  const menu = $("slash");
  menu.hidden = !list.length;
  if (!list.length) return;
  S.slashSel = Math.min(S.slashSel, list.length - 1);
  menu.textContent = "";
  list.forEach((c, i) => {
    const b = el("button", "menu-item" + (i === S.slashSel ? " active" : ""),
      '<span class="cmd"></span><span class="grow desc"></span>');
    q(b, ".cmd").textContent = "/" + c.name + (c.argument_hint ? " " + c.argument_hint : "");
    q(b, ".desc").textContent = c.description ?? "";
    b.onmousedown = (e) => { e.preventDefault(); input.value = "/" + c.name; submit(); };
    menu.append(b);
  });
  menu.children[S.slashSel]?.scrollIntoView({ block: "nearest" });
}

const syncSend = () => ($<HTMLButtonElement>("send").disabled = !input.value.trim());
input.addEventListener("input", () => { S.slashHidden = false; autosize(); syncSend(); renderSlash(); });

input.addEventListener("keydown", (e) => {
  const list = slashMatches();
  if (list.length) {
    if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      S.slashSel = (S.slashSel + (e.key === "ArrowDown" ? 1 : -1) + list.length) % list.length;
      return renderSlash();
    }
    if (e.key === "Tab") {
      e.preventDefault();
      input.value = "/" + list[S.slashSel].name + " ";
      syncSend();
      return renderSlash();
    }
    if (e.key === "Enter" && !e.shiftKey && !e.isComposing) input.value = "/" + list[S.slashSel].name;
  }
  if (e.key === "Enter" && !e.shiftKey && !e.isComposing) {
    e.preventDefault();
    submit();
  }
});

async function submit() {
  const text = input.value.trim();
  if (!text) return;
  input.value = "";
  autosize();
  syncSend();
  S.slashSel = 0;
  renderSlash();
  if (text.startsWith("/")) return runCommand(text);
  stick = true;
  busy("发送中"); // 读秒从按下 Enter 起算；正文由 core 的 message_start 帧画
  const r = await post(text);
  if (r.error) {
    alertMsg(main, r.error);
    idle();
  }
}

const post = (text: string) =>
  call<Reply>("POST", "/message", { message: text }).catch((e): Reply => ({ error: String(e) }));

// 斜杠命令直接回结果，不启动 turn；plugin 的 prompt 命令展开后就是一条消息。无参的 /model /resume 列成可点的清单
async function runCommand(text: string) {
  const [cmd, arg] = text.split(/\s+/);
  if (cmd === "/new") return newConversation();
  if (cmd === "/resume") {
    if (arg) return openConversation(arg);
    await refreshSessions();
    return cmdList(text, S.sessions.filter((s) => s.state !== "elsewhere"), (s) => [s.title || "空对话", `${s.messages} 条`], (s) => openConversation(s.id));
  }
  const r = await post(text);
  if (r.status === "processing") return busy("发送中");
  if (!r.ok) return toast(r.error || "命令失败");
  if (r.command === "model") {
    if (!arg) cmdList(text, r.data as Model[], (m) => [m.name, m.current ? "当前" : m.owned_by ?? ""], (m) => runCommand("/model " + m.name));
  } else if (r.data != null) {
    const box = add(main, el("div", "cmd-result msg", '<div class="cmd-result-head"></div><pre class="mono"></pre>'));
    q(box, ".cmd-result-head").textContent = text;
    q(box, "pre").textContent = JSON.stringify(r.data, null, 2);
  } else {
    toast(`已执行 /${r.command}`, "success");
  }
}

function cmdList<T>(title: string, items: T[] = [], label: (it: T) => [string, string], pick: (it: T) => void) {
  const box = add(main, el("div", "cmd-result msg", '<div class="cmd-result-head"></div>'));
  q(box, ".cmd-result-head").textContent = title;
  for (const it of items) {
    const [a, b] = label(it);
    const row = el("button", "menu-item", '<span class="grow"></span><span class="text-3"></span>');
    q(row, ".grow").textContent = a;
    q(row, ".text-3").textContent = b;
    row.onclick = () => pick(it);
    box.append(row);
  }
}

function interrupt() {
  if (!S.since) return;
  S.verb = "正在中断";
  renderStatus();
  call("POST", "/interrupt");
}

// ==================== 主题 ====================

type Theme = "system" | "light" | "dark";
const THEMES: Record<Theme, [Icon, string]> = { system: ["monitor", "跟随系统"], light: ["sun", "浅色"], dark: ["moon", "深色"] };
const NEXT: Record<Theme, Theme> = { system: "light", light: "dark", dark: "system" };
const dark = matchMedia("(prefers-color-scheme: dark)");

function theme(): Theme {
  try {
    const t = localStorage.getItem("theme");
    return t && t in THEMES ? (t as Theme) : "system";
  } catch {
    return "system";
  }
}

function applyTheme() {
  const t = theme();
  document.documentElement.dataset.theme = t === "dark" || (t === "system" && dark.matches) ? "dark" : "light";
  $("theme").innerHTML = ic(THEMES[t][0]);
  $("theme").title = "主题：" + THEMES[t][1];
}

$("theme").onclick = () => {
  try { localStorage.setItem("theme", NEXT[theme()]); } catch {}
  applyTheme();
};
dark.addEventListener("change", applyTheme);

// ==================== 杂项接线 ====================

function closeSidebar() {
  $("sidebar").classList.remove("open");
  $("scrim").classList.remove("open");
}

$("menu").onclick = () => { $("sidebar").classList.add("open"); $("scrim").classList.add("open"); };
$("scrim").onclick = closeSidebar;
$("new-session").onclick = newConversation;
q(document, ".side-head .brand").onclick = newConversation;
$("send").onclick = submit;
$("stop").onclick = interrupt;
$("model-btn").onclick = openModelMenu;

document.addEventListener("click", (e) => {
  const t = e.target as Element;
  if (!t.closest(".dropdown")) $("model-menu").hidden = true;
  // 外链交给系统浏览器，不在应用窗口里跳走
  const a = t.closest<HTMLAnchorElement>("a[href^='http']");
  if (a) {
    e.preventDefault();
    core.openUrl(a.href);
  }
  const copy = t.closest<HTMLElement>("[data-copy]");
  if (copy) {
    navigator.clipboard.writeText(q(copy.closest(".code") ?? document, "pre").textContent ?? "");
    copy.innerHTML = ic("check") + "<span>已复制</span>";
    setTimeout(() => (copy.innerHTML = ic("copy") + "<span>复制</span>"), 1500);
  }
});

// 设置 = 重走引导，现值预填。⌘,（macOS）/ Ctrl+,（其余）与系统菜单都进这里；引导页开着时不再开一遍
const MAC = /Mac/.test(navigator.userAgent);
async function openSettings() {
  if (!$("setup").hidden) return;
  await setup(await core.call<Settings>("GET", "/setup"));
  setModel((await call<Statusline>("GET", "/statusline")).model);
}

document.addEventListener("keydown", (e) => {
  const mod = MAC ? e.metaKey && !e.ctrlKey : e.ctrlKey && !e.metaKey;
  if (mod && !e.shiftKey && !e.altKey && e.key === ",") return void (e.preventDefault(), openSettings());
  if (e.key !== "Escape") return;
  if (!$("model-menu").hidden) return void ($("model-menu").hidden = true);
  if (slashMatches().length) { S.slashHidden = true; return renderSlash(); }
  interrupt();
});

function setConn(ok: boolean) {
  $("conn").className = "conn " + (ok ? "ok" : "down");
  $("conn-text").textContent = ok ? "已连接 core" : "core 未连接";
}

// 推送流断了 core.ts 自己重连；重连之后补一次清单与历史，断线期间的帧拿不回来
let everOpen = false;
function onConn(ok: boolean) {
  setConn(ok);
  if (!ok) return;
  if (everOpen) refreshSessions().then(() => openConversation(S.sessionId));
  everOpen = true;
}

(async () => {
  applyTheme();
  // 首启引导（setup.ts）：settings.json 里 setup_done 不为 true 就先走一遍
  const cfg = await core.call<Settings>("GET", "/setup");
  if ((await core.setupForced()) || !cfg.setup_done) await setup(cfg);
  core.onMenu("settings", openSettings);
  S.workdir = await core.workdir();
  await core.subscribe(handle, onConn);
  setModel((await call<Statusline>("GET", "/statusline")).model);
  // 起步是一段新对话：第一条消息发出去，core 才开出它
  newConversation();
  await Promise.all([refreshSessions(), refreshCommands()]);
})().catch((e) => {
  setConn(false);
  toast("连不上 core：" + (e instanceof Error ? e.message : String(e)) + "\n先运行 make run");
});
