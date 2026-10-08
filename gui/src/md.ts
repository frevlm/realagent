// Markdown（够用就行：围栏代码、标题、列表、引用、表格、行内）

import { esc, ic } from "./dom";

function inline(s: string): string {
  return s.split(/(`[^`]+`)/).map((part, i) => {
    if (i % 2) return `<code>${esc(part.slice(1, -1))}</code>`;
    return esc(part)
      .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
      .replace(/(^|[^*])\*([^*\s][^*]*)\*/g, "$1<em>$2</em>")
      .replace(/\[([^\]]+)\]\((https?:\/\/[^\s)]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>');
  }).join("");
}

function codeBlock(code: string, lang: string): string {
  return `<div class="code"><div class="code-bar"><span>${esc(lang || "text")}</span>` +
    `<button class="code-copy" data-copy>${ic("copy")}<span>复制</span></button></div>` +
    `<pre><code>${esc(code)}</code></pre></div>`;
}

const LIST = /^(\s*)([-*+]|\d+[.)])\s+(.*)$/;
const isBlockStart = (l: string) => /^```|^#{1,4}\s|^>\s?/.test(l) || LIST.test(l);

export function md(src: string): string {
  const lines = src.split("\n");
  const out: string[] = [];
  for (let i = 0; i < lines.length;) {
    const l = lines[i];
    let m: RegExpMatchArray | null;
    if ((m = l.match(/^```\s*(\S*)/))) {
      const buf: string[] = [];
      for (i++; i < lines.length && !/^```/.test(lines[i]); i++) buf.push(lines[i]);
      i++;
      out.push(codeBlock(buf.join("\n"), m[1]));
    } else if ((m = l.match(/^(#{1,4})\s+(.*)/))) {
      out.push(`<h${m[1].length}>${inline(m[2])}</h${m[1].length}>`);
      i++;
    } else if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(l)) {
      out.push("<hr>");
      i++;
    } else if (/^>\s?/.test(l)) {
      const buf: string[] = [];
      for (; i < lines.length && /^>\s?/.test(lines[i]); i++) buf.push(lines[i].replace(/^>\s?/, ""));
      out.push(`<blockquote>${md(buf.join("\n"))}</blockquote>`);
    } else if ((m = l.match(LIST))) {
      const tag = /\d/.test(m[2]) ? "ol" : "ul";
      const items: string[] = [];
      for (; i < lines.length && lines[i].trim() !== ""; i++) {
        const lm = lines[i].match(LIST);
        if (lm) items.push(lm[3]);
        else if (items.length) items[items.length - 1] += "\n" + lines[i].trim();
      }
      out.push(`<${tag}>${items.map((t) => `<li>${inline(t).replace(/\n/g, "<br>")}</li>`).join("")}</${tag}>`);
    } else if (l.includes("|") && /^\s*\|?\s*:?-{3,}/.test(lines[i + 1] || "")) {
      const cells = (r: string) => r.trim().replace(/^\||\|$/g, "").split("|").map((c) => inline(c.trim()));
      const head = cells(l);
      const rows: string[][] = [];
      for (i += 2; i < lines.length && lines[i].includes("|"); i++) rows.push(cells(lines[i]));
      out.push(`<table><thead><tr>${head.map((c) => `<th>${c}</th>`).join("")}</tr></thead><tbody>` +
        rows.map((r) => `<tr>${r.map((c) => `<td>${c}</td>`).join("")}</tr>`).join("") + "</tbody></table>");
    } else if (l.trim() === "") {
      i++;
    } else {
      const buf: string[] = [];
      for (; i < lines.length && lines[i].trim() !== "" && (buf.length === 0 || !isBlockStart(lines[i])); i++) buf.push(lines[i]);
      out.push(`<p>${buf.map(inline).join("<br>")}</p>`);
    }
  }
  return out.join("");
}
