// 首启引导（同 TUI 的 setup.go）：setup_done 不为 true（或 make setup-*）时，进主界面前先走一遍。
//
// 一页一项：protocol → base_url → api_key → model → small_model，现值预填。
// 模型两页先拉端点的模型清单（POST /setup/models）给人选，选「手动输入…」或拉不到就手填。
// 写盘归 core（POST /setup），走完直接进主界面。Esc / 上一步 回上一页，第一页是跳过。

import { call, type Reply } from "./core";
import { $, el, esc, ic, q } from "./dom";

export interface Settings {
  setup_done?: boolean;
  protocol?: string;
  base_url?: string;
  api_key?: string;
  model?: string;
  small_model?: string;
}

const PROTOCOLS = ["anthropic-messages", "openai-chat", "openai-responses"];
const MANUAL = "手动输入…";
// 说明里 `反引号` 包住的是标识符与地址：等宽、不在连字符处断行
const STEPS = [
  ["protocol", "协议", "端点说哪种协议。Anthropic 原厂及其兼容端点选 `anthropic-messages`。"],
  ["base_url", "端点地址", "`anthropic-messages` 不带 /v1，如 `https://api.anthropic.com`；`openai-*` 带到 /v1，如 `https://api.openai.com/v1`。"],
  ["api_key", "API key", "端点的 API key。"],
  ["model", "主模型", "对话用的模型。"],
  ["small_model", "小模型", "收工判定用的模型。手动输入并留空 = 不做收工判定。"],
] as const;
type Key = (typeof STEPS)[number][0];
const MODEL = 3; // 从这一页起是模型页

export function setup(cfg: Settings): Promise<void> {
  const v = Object.fromEntries(STEPS.map(([k]) => [k, cfg[k] ?? ""])) as Record<Key, string>;
  let step = 0, models: string[] = [], loading = false, manual = false, note = "";
  const root = $("setup");
  const key = () => STEPS[step][0];

  // 列表页的可选项；文本页返回 null
  const choices = () =>
    step === 0 ? PROTOCOLS : step >= MODEL && !manual && !loading ? [...models, MANUAL] : null;

  return new Promise((done) => {
    const finish = () => { root.hidden = true; done(); };

    // 进入某一页：模型页现值不在清单里、或清单是空的，就直接手填
    const enter = (s: number) => {
      step = s;
      note = "";
      manual = s >= MODEL && !loading && !models.includes(v[key()]) && (v[key()] !== "" || !models.length);
      render();
    };

    const back = () => {
      if (step === 0) return finish();
      loading = false; // 不等清单了；再进模型页会重拉
      enter(step - 1);
    };

    // 收下本页、去下一页；api_key 之后拉模型清单，最后一页保存
    const next = async (val: string) => {
      if (val === MANUAL) { manual = true; return render(); }
      v[key()] = val.trim();
      if ((key() === "base_url" || key() === "model") && !v[key()]) { note = `${key()} 不能为空`; return render(); }
      if (key() === "small_model") {
        const r = await call<Reply>("POST", "/setup", v).catch((e) => ({ error: String(e) }));
        if (!r.error) return finish();
        note = r.error;
        return render();
      }
      if (key() !== "api_key") return enter(step + 1);
      loading = true;
      models = [];
      enter(MODEL);
      const r = await call<Reply<string[]>>("POST", "/setup/models", { protocol: v.protocol, base_url: v.base_url, api_key: v.api_key })
        .catch((e) => ({ error: String(e), data: undefined }));
      if (!loading) return; // 已经退回上一页了
      loading = false;
      models = r.data ?? [];
      enter(MODEL);
      if (r.error) { note = `拉不到模型清单（${r.error}），请手动输入`; render(); }
    };

    function render() {
      const items = choices();
      const [k, title, hint] = STEPS[step];
      const last = k === "small_model";
      root.innerHTML = `
        <a class="brand"><span class="brand-mark">${ic("terminal")}</span>realagent</a>
        <div class="setup-card">
          <div class="setup-steps">${STEPS.map((_, i) => `<span class="${i <= step ? "on" : ""}"></span>`).join("")}</div>
          <div class="setup-head"><h2>${title}</h2><span class="mono">${k} · ${step + 1}/${STEPS.length}</span></div>
          <p class="text-3 small">${esc(hint).replace(/`([^`]+)`/g, "<code>$1</code>")}</p>
          <div class="setup-body"></div>
          ${note ? `<div class="alert alert-danger">${ic("alert")}<span>${esc(note)}</span></div>` : ""}
          <div class="setup-foot">
            <button type="button" class="btn btn-secondary" data-back>${step ? "上一步" : "跳过"}</button>
            <span class="grow caption">${items ? "<b>↑ ↓</b> 选择 · <b>Enter</b> 确认" : ""}</span>
            ${items || loading ? "" : `<button type="button" class="btn btn-primary" data-next>${last ? "保存" : "下一步"}</button>`}
          </div>
        </div>`;
      const body = q(root, ".setup-body");
      q(root, "[data-back]").onclick = back;
      if (loading) {
        body.innerHTML = `<div class="setup-loading">${ic("loader", "spin")}正在拉模型清单…</div>`;
      } else if (items) {
        const list = el("div", "setup-list");
        for (const it of items) {
          const manualRow = it === MANUAL;
          const cur = it === v[k];
          const b = el("button", "setup-opt" + (cur ? " current" : "") + (manualRow ? " manual" : ""),
            `<span class="grow">${esc(it)}</span>${cur ? ic("check") : manualRow ? ic("pencil") : ""}`);
          b.type = "button";
          b.onclick = () => next(it);
          list.append(b);
        }
        body.append(list);
        (list.querySelector<HTMLElement>(".current") ?? list.querySelector<HTMLElement>("button"))?.focus();
      } else {
        const input = el("input", "form-control mono");
        input.value = v[k];
        input.spellcheck = false;
        input.autocomplete = "off";
        input.onkeydown = (e) => { if (e.key === "Enter" && !e.isComposing) next(input.value); };
        q(root, "[data-next]").onclick = () => next(input.value);
        body.append(input);
        input.focus();
      }
    }

    // 键盘：↑/↓ 在清单里移动焦点，Esc 回上一页；不让它冒到主界面去（那边 Esc 是中断）
    root.onkeydown = (e) => {
      if (e.key === "Escape") { e.stopPropagation(); return back(); }
      if (e.key !== "ArrowDown" && e.key !== "ArrowUp") return;
      const list = [...root.querySelectorAll<HTMLElement>(".setup-opt")];
      if (!list.length) return;
      e.preventDefault();
      const i = list.indexOf(document.activeElement as HTMLElement);
      list[(i + (e.key === "ArrowDown" ? 1 : -1) + list.length) % list.length].focus();
    };

    root.hidden = false;
    enter(0);
  });
}
