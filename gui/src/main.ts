// realagent gui —— core 的桌面客户端。协议见 docs/PROTOCOL.md，传输见 ./core.ts。
//
// 推送流与 GET /session 回放是同形的帧，走同一个 handle()：实时看与翻历史长得一样（ADR-0020）。
// 只渲染当前 agent 的帧；切 agent 就清空，从 GET /session 重读。审批例外：不管在看谁都要弹。

import "./app.css";
import * as core from "./core";
import type { Agent, Approval, Command, Frame, Model, Replayed, Reply, Session, Statusline } from "./core";
import { $, ago, basename, el, esc, ic, q, shortPath, toast, type Icon } from "./dom";
import { md } from "./md";

const S = {
  agentId: 0,
  agents: new Map<number, Agent>(), // GET /agents
  workdir: "",                      // 新建 agent 时的默认目录
  commands: [] as Command[],        // GET /commands
  approvals: [] as Approval[],      // 挂起的 permission_request
  since: 0, verb: "", cost: null as number | null, timer: 0, interrupted: false,
  slashSel: 0, slashHidden: false,
};

// 所有请求都带当前 agent；端点不认的键 core 不看
const call = <T>(method: core.Method, path: string, body: object = {}) =>
  core.call<T>(method, path, { agent_id: S.agentId, ...body });

// ==================== 对话流 ====================

interface Text { node: HTMLElement; buf: string }
interface Think { node: HTMLDetailsElement; body: HTMLElement; start: number; replay: boolean }

// 当前正在长的那几块：思考、正文、工具卡片
const view = {
  think: null as Think | null,
  text: null as Text | null,
  tools: new Map<string, HTMLElement>(),
  lastTool: null as HTMLElement | null,
};

const thread = $("thread");
const wrap = $("thread-wrap");
let stick = true; // 贴着底就继续贴底；用户滚上去了就不拽回来
wrap.addEventListener("scroll", () => {
  stick = wrap.scrollHeight - wrap.scrollTop - wrap.clientHeight < 80;
  q(document, ".main").classList.toggle("scrolled", wrap.scrollTop > 0);
});
const follow = () => { if (stick) wrap.scrollTop = wrap.scrollHeight; };
new ResizeObserver(follow).observe(wrap); // 审批卡、多行输入把底栏撑高时，别把末尾挤到下面去

function add<T extends HTMLElement>(node: T): T {
  thread.append(node);
  $("empty").hidden = true;
  follow();
  return node;
}

function clearThread() {
  thread.textContent = "";
  view.think = view.text = view.lastTool = null;
  view.tools.clear();
  $("empty").hidden = false;
  stick = true;
}

const notice = (text: string) => add(el("div", "notice", esc(text)));

function alertMsg(text: string) {
  add(el("div", "alert alert-danger msg", ic("alert"))).append(text);
}

function userMsg(text: string) {
  const m = el("div", "msg msg-user");
  const b = el("div", "bubble-user");
  b.textContent = text;
  m.append(b);
  add(m);
  stick = true;
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

function textAppend(delta: string, replay: boolean) {
  if (!view.text) view.text = { node: add(el("div", "prose msg")), buf: "" };
  view.text.buf += delta;
  if (replay) return;
  dirty.add(view.text);
  if (!raf) raf = requestAnimationFrame(paint);
}

function endText() {
  if (!view.text) return;
  dirty.delete(view.text);
  view.text.node.innerHTML = md(view.text.buf);
  view.text = null;
}

function thinkAppend(delta: string, replay: boolean) {
  if (!view.think) {
    const d = el("details", "think msg", `<summary>${ic("sparkles")}<span>思考中</span>${ic("chev", "chev")}</summary><div class="think-body"></div>`);
    d.open = !replay;
    view.think = { node: add(d), body: q(d, ".think-body"), start: Date.now(), replay };
  }
  view.think.body.textContent += delta;
  follow();
}

function endThink() {
  const t = view.think;
  if (!t) return;
  const secs = Math.round((Date.now() - t.start) / 1000);
  q(t.node, "summary span").textContent = t.replay || secs < 1 ? "思考过程" : `思考了 ${secs} 秒`;
  t.node.open = false;
  view.think = null;
}

function endAll() { endThink(); endText(); }

// ==================== 工具卡片 ====================

const TOOLS: Record<string, [Icon, string]> = {
  bash: ["terminal", "执行命令"], read: ["file", "读文件"], edit: ["pencil", "改文件"],
  search: ["search", "搜代码"], spawn: ["fork", "派生 agent"], send_message: ["send", "发消息"],
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

function toolStart(d: { name: string; id: string }) {
  const card = el("div", "tool msg",
    `<button class="tool-head"><span class="tool-icon">${ic(toolIcon(d.name))}</span><span class="tool-name"></span><span class="tool-label"></span>` +
    `<span class="tool-state"></span>${ic("chev", "chev")}</button><pre class="tool-out"></pre>`);
  q(card, ".tool-name").textContent = d.name;
  q(card, ".tool-head").onclick = () => card.classList.toggle("open");
  setToolState(card, "run");
  view.tools.set(d.id, card);
  view.lastTool = card;
  add(card);
}

// 按行推但不保证一帧一行：续写即可，不假设边界
function toolOut(d: { call_id: string; text?: string }, replay: boolean) {
  const card = view.tools.get(d.call_id) ?? view.lastTool;
  if (!card) return;
  const out = q(card, ".tool-out");
  out.textContent += d.text ?? "";
  if (!replay) card.classList.add("open");
  out.scrollTop = out.scrollHeight;
  follow();
}

function toolEnd(d: { id: string; status?: number; interrupted?: boolean }) {
  const card = view.tools.get(d.id) ?? view.lastTool;
  if (!card) return;
  const out = q(card, ".tool-out");
  out.textContent = (out.textContent ?? "").replace(/\n+$/, "");
  setToolState(card, d.interrupted ? "stop" : d.status ? "fail" : "ok");
  // 成功的收起来，失败的留着给人看
  if (!d.status && !d.interrupted) card.classList.remove("open");
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
  const aid = "agent_id" in f.data ? f.data.agent_id : undefined;
  // 侧栏的运行态看所有 agent，不只看当前这个
  if (aid && f.event === "agent_start") setAgentState(aid, "running");
  if (aid && f.event === "agent_end") setAgentState(aid, "idle");
  if (f.event === "permission_request") return addApproval(f.data);
  if (f.event === "statusline") return setModel(f.data.model);
  if (aid && aid !== S.agentId) return;

  switch (f.event) {
    case "message_start":
      endAll();
      if (f.data.text) userMsg(f.data.text);
      break;
    case "agent_start":
    case "turn_start":
    case "thinking_start":
      endText();
      if (!replay) busy("思考中");
      break;
    case "thinking_update":
      thinkAppend(f.data.delta ?? "", replay);
      break;
    case "thinking_stop":
      endThink();
      break;
    case "message_update":
      endThink();
      textAppend(f.data.delta ?? "", replay);
      if (!replay) busy("生成回复");
      break;
    case "message_end":
      endText();
      break;
    case "tool_execution_start":
      endAll();
      toolStart(f.data);
      if (!replay) busy(toolVerb(f.data.name));
      break;
    case "tool_output":
      toolOut(f.data, replay);
      break;
    case "tool_execution_end":
      toolEnd(f.data);
      if (!replay) busy("思考中");
      break;
    case "status_update":
      if (f.data.cost != null) S.cost = f.data.cost;
      renderStatus();
      break;
    case "turn_end":
      endAll();
      if (f.data.error) alertMsg(f.data.error);
      break;
    case "interrupted":
      S.interrupted = true;
      endAll();
      for (const c of view.tools.values()) if (c.dataset.state === "run" || c.dataset.state === "wait") setToolState(c, "stop");
      dropApprovals(S.agentId);
      break;
    case "agent_end": {
      endAll();
      if (f.data.recap) add(el("div", "recap msg", '<div class="recap-head">回顾</div>')).append(f.data.recap);
      const parts = [S.interrupted ? "已中断" : "完成"];
      if (S.since) parts.push(`${elapsed()} 秒`);
      if (f.data.cost) parts.push(money(f.data.cost));
      notice(parts.join(" · "));
      idle();
      refreshSessions();
      break;
    }
  }
}

// ==================== 审批 ====================

function addApproval(a: Approval) {
  S.approvals.push(a);
  if (a.agent_id === S.agentId && view.lastTool?.dataset.state === "run") {
    q(view.lastTool, ".tool-label").textContent = toolSummary(a.params);
    setToolState(view.lastTool, "wait");
    busy("等待你的审批");
  }
  renderApprovals();
}

function dropApprovals(agentId: number) {
  S.approvals = S.approvals.filter((a) => a.agent_id !== agentId);
  renderApprovals();
}

function renderApprovals() {
  const box = $("approvals");
  box.textContent = "";
  for (const a of S.approvals) {
    const card = el("div", "approval",
      `<div class="approval-head">${ic("shield")}<span></span><span class="who"></span></div><pre class="mono"></pre>` +
      `<div class="approval-actions"><button class="btn btn-secondary btn-sm">拒绝</button><button class="btn btn-primary btn-sm">允许</button></div>`);
    q(card, ".approval-head span").textContent = `${toolVerb(a.tool)}需要你的允许`;
    q(card, ".who").textContent = `agent ${a.agent_id} · ${a.tool}`;
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
  if (a.agent_id === S.agentId && view.lastTool?.dataset.state === "wait") {
    setToolState(view.lastTool, allow ? "run" : "fail");
    busy(allow ? toolVerb(a.tool) : "思考中");
  }
  const r = await call<Reply>("POST", "/approval-response", { id: a.id, allow });
  if (r.error) toast("审批回传失败：" + r.error);
}

// ==================== agent 与会话（侧栏） ====================

async function refreshAgents() {
  const list = await call<Agent[]>("GET", "/agents");
  if (!Array.isArray(list)) return;
  S.agents = new Map(list.map((a) => [a.id, a]));
  renderAgents();
}

function setAgentState(id: number, state: string) {
  const a = S.agents.get(id);
  if (!a) return void refreshAgents(); // spawn 出来的新 agent
  a.state = state;
  renderAgents();
}

function renderAgents() {
  const nav = $("agent-list");
  nav.textContent = "";
  for (const a of S.agents.values()) {
    const b = el("button", "nav-item" + (a.id === S.agentId ? " active" : ""),
      `<span class="agent-dot ${a.state === "running" ? "running" : ""}"></span>` +
      `<span class="grow"><span class="line1">Agent ${a.id}</span><span class="line2 mono"></span></span>`);
    q(b, ".line2").textContent = basename(a.workdir);
    b.title = a.workdir;
    if (a.in_edges?.length) b.append(el("span", "nav-meta", `子 · ${a.in_edges.join(",")}`));
    b.onclick = () => attach(a.id);
    nav.append(b);
  }
  renderTop();
}

function renderTop() {
  const a = S.agents.get(S.agentId);
  $("top-name").textContent = a ? `Agent ${a.id}` : "—";
  $("top-dir-text").textContent = a ? shortPath(a.workdir) : "";
  $("top-dir").title = a?.workdir ?? "";
  $("empty-agent").textContent = a ? `Agent ${a.id}` : "agent";
  $("empty-dir").textContent = a ? basename(a.workdir) : "";
  $("top-state").hidden = a?.state !== "running";
}

async function refreshSessions() {
  if (!S.agentId) return;
  const r = await call<Session[] | Reply>("GET", "/sessions");
  if (Array.isArray(r)) renderSessions(r);
  else if (!$("session-list").children.length) $("session-list").innerHTML = '<div class="side-note">agent 运行中，稍后再看</div>';
}

function renderSessions(list: Session[] = []) {
  const nav = $("session-list");
  nav.textContent = "";
  if (!list.length) nav.innerHTML = '<div class="side-note">还没有会话</div>';
  for (const s of list) {
    const mine = s.opened_by === S.agentId;
    const other = s.opened_by != null && !mine;
    const b = el("button", "nav-item" + (mine ? " active" : ""),
      '<span class="grow"><span class="line1"></span><span class="line2"></span></span>');
    q(b, ".line1").textContent = s.title || "空会话";
    q(b, ".line2").textContent = other ? `Agent ${s.opened_by} 打开着` : `${s.messages} 条 · ${ago(s.mtime)}`;
    b.disabled = other;
    if (!mine && !other) b.onclick = () => resumeSession(s.id);
    nav.append(b);
  }
}

async function resumeSession(id: string) {
  const r = await call<Reply<Session[]>>("POST", "/session", { id });
  if (!r.ok) return toast(r.error || "恢复失败");
  renderSessions(r.data);
  await loadHistory();
  closeSidebar();
}

async function newSession() {
  const r = await call<Reply<Session[]>>("POST", "/session");
  if (!r.ok) return toast(r.error || "新建失败");
  clearThread();
  renderSessions(r.data);
  closeSidebar();
}

async function loadHistory() {
  clearThread();
  const frames = await call<Replayed[]>("GET", "/session");
  if (Array.isArray(frames)) for (const f of frames) handle({ event: f.type, data: f.data } as Frame, true);
  endAll();
  wrap.scrollTop = wrap.scrollHeight;
}

async function attach(id: number) {
  S.agentId = id;
  idle();
  renderAgents();
  closeSidebar();
  await loadHistory();
  if (S.agents.get(id)?.state === "running") busy("运行中");
  refreshSessions();
  call<Command[]>("GET", "/commands").then((c) => { if (Array.isArray(c)) S.commands = c; });
  input.focus();
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
  const r = await call<Reply>("POST", "/message", { message: text }).catch((e): Reply => ({ error: String(e) }));
  if (r.error) {
    alertMsg(r.error);
    idle();
  }
}

// 斜杠命令直接回结果，不启动 agent turn。无参的 /model /resume 列成可点的清单
async function runCommand(text: string) {
  const r = await call<Reply>("POST", "/message", { message: text }).catch((e): Reply => ({ error: String(e) }));
  if (!r.ok) return toast(r.error || "命令失败");
  const arg = text.split(/\s+/)[1];
  if (r.command === "new") {
    clearThread();
    renderSessions(r.data as Session[]);
  } else if (r.command === "resume") {
    const list = r.data as Session[];
    renderSessions(list);
    if (arg) await loadHistory();
    else cmdList(text, list, (s) => [s.title || "空会话", `${s.messages} 条`], (s) => resumeSession(s.id));
  } else if (r.command === "model") {
    if (!arg) cmdList(text, r.data as Model[], (m) => [m.name, m.current ? "当前" : m.owned_by ?? ""], (m) => runCommand("/model " + m.name));
  } else if (r.data != null) {
    const box = add(el("div", "cmd-result msg", '<div class="cmd-result-head"></div><pre class="mono"></pre>'));
    q(box, ".cmd-result-head").textContent = text;
    q(box, "pre").textContent = JSON.stringify(r.data, null, 2);
  } else {
    toast(`已执行 /${r.command}`, "success");
  }
}

function cmdList<T>(title: string, items: T[] = [], label: (it: T) => [string, string], pick: (it: T) => void) {
  const box = add(el("div", "cmd-result msg", '<div class="cmd-result-head"></div>'));
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

// ==================== 新建 agent ====================

const agentDir = $<HTMLInputElement>("agent-dir");

function openModal() {
  agentDir.value = S.agents.get(S.agentId)?.workdir ?? S.workdir;
  $("modal").hidden = false;
  agentDir.select();
}
const closeModal = () => ($("modal").hidden = true);

$<HTMLFormElement>("agent-form").onsubmit = async (e) => {
  e.preventDefault();
  const r = await call<Reply>("POST", "/agent", { workdir: agentDir.value.trim() });
  if (!r.agent_id) return toast(r.error || "建 agent 失败");
  closeModal();
  await refreshAgents();
  attach(r.agent_id);
};

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
$("new-agent").onclick = openModal;
$("agent-cancel").onclick = closeModal;
$("new-session").onclick = newSession;
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

document.addEventListener("keydown", (e) => {
  if (e.key !== "Escape") return;
  if (!$("modal").hidden) return closeModal();
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
  if (everOpen && S.agentId) refreshAgents().then(() => attach(S.agentId));
  everOpen = true;
}

(async () => {
  applyTheme();
  S.workdir = await core.workdir();
  await core.subscribe(handle, onConn);
  const r = await call<Reply>("POST", "/agent", { workdir: S.workdir });
  if (!r.agent_id) throw new Error(r.error || "建 agent 失败");
  setModel((await call<Statusline>("GET", "/statusline")).model);
  await refreshAgents();
  await attach(r.agent_id);
})().catch((e) => {
  setConn(false);
  toast("连不上 core：" + (e instanceof Error ? e.message : String(e)) + "\n先运行 make run");
});
