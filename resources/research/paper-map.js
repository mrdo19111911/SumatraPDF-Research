const kinds = new Map([
  ["problem", "Bài toán"],
  ["assumption", "Giả định"],
  ["definition", "Định nghĩa"],
  ["lemma", "Bổ đề"],
  ["theorem", "Định lý"],
  ["method", "Phương pháp"],
  ["consequence", "Hệ quả"],
  ["limitation", "Giới hạn"],
  ["reference", "Tài liệu"],
]);
const relations = new Map([
  ["uses", "dùng"],
  ["assumes", "cần giả định"],
  ["defines", "định nghĩa"],
  ["implies", "suy ra"],
  ["cites", "viện dẫn"],
  ["generalizes", "tổng quát hóa"],
]);
const maxNodes = 60,
  maxEdges = 100;
const bounded = (s, n) => typeof s === "string" && s.trim().length > 0 && s.length <= n;
const anchor = (n) => Number.isSafeInteger(n.page) && n.page > 0 && bounded(n.quote, 400) && n.quote.trim().length >= 8;

export function parseMap(text) {
  text = text
    .trim()
    .replace(/^```(?:json)?\s*/i, "")
    .replace(/\s*```$/, "");
  const data = JSON.parse(text);
  if (
    !data ||
    !Array.isArray(data.nodes) ||
    !Array.isArray(data.edges) ||
    data.nodes.length > maxNodes ||
    data.edges.length > maxEdges
  )
    throw Error("Cấu trúc KG không hợp lệ.");
  const ids = new Set(),
    nodes = [],
    edges = [];
  for (const n of data.nodes) {
    if (!n || !bounded(n.id, 80) || ids.has(n.id) || !kinds.has(n.kind) || !bounded(n.label, 1000) || !anchor(n))
      throw Error("Nút KG thiếu định danh hoặc điểm nguồn hợp lệ.");
    ids.add(n.id);
    nodes.push({ id: n.id, kind: n.kind, label: n.label, page: n.page, quote: n.quote });
  }
  for (const e of data.edges) {
    if (!e || !ids.has(e.from) || !ids.has(e.to) || e.from === e.to || !relations.has(e.relation) || !anchor(e))
      throw Error("Quan hệ KG thiếu điểm nguồn hoặc trỏ đến nút không tồn tại.");
    edges.push({
      id: `edge-${edges.length}`,
      from: e.from,
      to: e.to,
      relation: e.relation,
      page: e.page,
      quote: e.quote,
    });
  }
  if (!nodes.length) throw Error("KG chưa có nút có dẫn chứng. Thử paper có văn bản trích xuất được.");
  return { nodes, edges, coverage: bounded(data.coverage, 3000) ? data.coverage : "AI chưa nêu phạm vi đã đọc." };
}

export function mountMap() {
  const $ = (id) => document.getElementById(id);
  let model, hovered, selected;
  const sources = new Map(),
    pending = new Set(),
    buttons = new Map();
  const key = (n) => `${n.page}\n${n.quote}`;
  const notify = (method, params) => window.__sumatra__.notify(method, params);
  const status = (text) => {
    $("status").textContent = text;
  };
  function request(n) {
    if (sources.has(key(n)) || pending.has(key(n))) return;
    pending.add(key(n));
    notify("map-context", { page: n.page, quote: n.quote });
  }
  function preview(n, rect) {
    hovered = n;
    request(n);
    const s = sources.get(key(n));
    const tip = $("tooltip");
    tip.replaceChildren();
    const title = document.createElement("strong");
    title.textContent = `Trang ${n.page} · ${s?.verified ? "Đoạn gốc PDF" : "Đang đối chiếu nguồn"}`;
    const text = document.createElement("span");
    const passage = s ? s.text : "Đang lấy đoạn văn quanh vị trí nguồn…";
    const quote = n.quote.replace(/\s+/g, " ").trim();
    const at = s?.verified ? passage.indexOf(quote) : -1;
    if (at < 0) text.textContent = passage;
    else {
      const mark = document.createElement("mark");
      mark.textContent = quote;
      text.append(
        document.createTextNode(passage.slice(0, at)),
        mark,
        document.createTextNode(passage.slice(at + quote.length)),
      );
    }
    tip.append(title, text);
    tip.hidden = false;
    tip.style.left = `${Math.max(12, Math.min(rect.left, innerWidth - tip.offsetWidth - 12))}px`;
    tip.style.top = `${Math.max(12, Math.min(rect.bottom + 8, innerHeight - tip.offsetHeight - 12))}px`;
    const mark = tip.querySelector("mark");
    if (mark) tip.scrollTop = Math.max(0, mark.offsetTop - tip.clientHeight / 3);
  }
  function jump(n) {
    request(n);
    notify("map-jump", { page: n.page, quote: n.quote });
  }
  function detail(n) {
    selected = n.id;
    for (const [id, b] of buttons) b.classList.toggle("selected", id === selected);
    const panel = $("details");
    panel.hidden = false;
    panel.replaceChildren();
    const title = document.createElement("strong");
    title.textContent = n.label;
    const instruction = document.createElement("p");
    instruction.textContent = "Quan hệ do AI đề xuất. Bấm từng quan hệ để kiểm tra đoạn nguồn.";
    panel.append(title, instruction);
    for (const e of model.edges.filter((e) => e.from === n.id || e.to === n.id)) {
      const from = model.nodes.find((x) => x.id === e.from),
        to = model.nodes.find((x) => x.id === e.to);
      const b = document.createElement("button");
      b.textContent = `${from.label} → ${relations.get(e.relation)} → ${to.label}`;
      b.addEventListener("mouseenter", () => preview(e, b.getBoundingClientRect()));
      b.addEventListener("mouseleave", hideTip);
      b.addEventListener("focus", () => preview(e, b.getBoundingClientRect()));
      b.addEventListener("blur", hideTip);
      b.addEventListener("click", () => jump(e));
      panel.append(b);
    }
  }
  function hideTip() {
    hovered = undefined;
    $("tooltip").hidden = true;
  }
  function refreshTip() {
    if (!hovered) return;
    const b = buttons.get(hovered.id);
    preview(hovered, b ? b.getBoundingClientRect() : $("details").getBoundingClientRect());
  }
  function connect() {
    const svg = $("connections"),
      root = $("map").getBoundingClientRect();
    svg.replaceChildren();
    if (!model) return;
    const ns = "http://www.w3.org/2000/svg";
    const defs = document.createElementNS(ns, "defs"),
      marker = document.createElementNS(ns, "marker");
    for (const [attr, value] of Object.entries({
      id: "arrow",
      markerWidth: "8",
      markerHeight: "8",
      refX: "7",
      refY: "4",
      orient: "auto",
    }))
      marker.setAttribute(attr, value);
    const tip = document.createElementNS(ns, "path");
    tip.setAttribute("d", "M0,0 L8,4 L0,8 Z");
    tip.setAttribute("fill", "var(--user)");
    marker.append(tip);
    defs.append(marker);
    svg.append(defs);
    for (const e of model.edges) {
      const from = buttons.get(e.from),
        to = buttons.get(e.to);
      if (!from || !to) continue;
      const a = from.getBoundingClientRect(),
        b = to.getBoundingClientRect();
      const sameRow = Math.abs(a.top - b.top) < 5;
      const sign = sameRow ? (b.left > a.left ? 1 : -1) : b.top > a.top ? 1 : -1;
      const x = (sameRow ? (sign > 0 ? a.right : a.left) : a.left + a.width / 2) - root.left;
      const y = (sameRow ? a.top + a.height / 2 : sign > 0 ? a.bottom : a.top) - root.top;
      const tx = (sameRow ? (sign > 0 ? b.left : b.right) : b.left + b.width / 2) - root.left;
      const ty = (sameRow ? b.top + b.height / 2 : sign > 0 ? b.top : b.bottom) - root.top;
      const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
      const bend = sign * 20;
      path.setAttribute(
        "d",
        sameRow
          ? `M${x},${y} C${x + bend},${y} ${tx - bend},${ty} ${tx},${ty}`
          : `M${x},${y} C${x},${y + bend} ${tx},${ty - bend} ${tx},${ty}`,
      );
      path.setAttribute("marker-end", "url(#arrow)");
      svg.append(path);
    }
  }
  function render() {
    if (!model) return;
    hideTip();
    buttons.clear();
    $("cards").replaceChildren();
    const layer = $("layer").value,
      query = $("query").value.trim().toLocaleLowerCase();
    const groups = {
      ideas: ["problem", "method", "theorem", "consequence", "limitation"],
      proof: ["assumption", "definition", "lemma", "theorem", "consequence"],
      references: ["reference"],
    };
    for (const n of model.nodes) {
      if (layer !== "all" && !groups[layer].includes(n.kind)) continue;
      if (query && !n.label.toLocaleLowerCase().includes(query)) continue;
      const b = document.createElement("button");
      b.className = "node";
      b.dataset.node = n.id;
      b.setAttribute("aria-describedby", "tooltip");
      const kind = document.createElement("small"),
        label = document.createElement("span");
      kind.textContent = `${kinds.get(n.kind)} · tr. ${n.page}`;
      label.textContent = n.label;
      b.append(kind, label);
      b.addEventListener("mouseenter", () => preview(n, b.getBoundingClientRect()));
      b.addEventListener("mouseleave", hideTip);
      b.addEventListener("focus", () => preview(n, b.getBoundingClientRect()));
      b.addEventListener("blur", hideTip);
      b.addEventListener("click", () => {
        detail(n);
        jump(n);
      });
      b.classList.toggle("selected", n.id === selected);
      b.classList.toggle("unverified", sources.get(key(n))?.verified === false);
      $("cards").append(b);
      buttons.set(n.id, b);
    }
    status(`${buttons.size}/${model.nodes.length} nút · ${model.edges.length} quan hệ AI đề xuất`);
    requestAnimationFrame(connect);
  }
  window.paperMap = {
    load(text) {
      try {
        model = parseMap(text);
        $("coverage").textContent = model.coverage;
        render();
      } catch (e) {
        model = undefined;
        $("cards").replaceChildren();
        status(`Không dựng được KG: ${e.message}`);
      }
    },
    source(page, quote, verified, text) {
      const n = { page, quote };
      pending.delete(key(n));
      sources.set(key(n), { verified, text });
      if (hovered && key(hovered) === key(n)) refreshTip();
      for (const node of model?.nodes || [])
        if (key(node) === key(n)) buttons.get(node.id)?.classList.toggle("unverified", !verified);
    },
    result(ok) {
      status(ok ? "Đã đến và tô sáng đoạn nguồn trong PDF." : "Không xác minh được điểm nguồn; PDF giữ nguyên vị trí.");
    },
    inspect() {
      return {
        nodes: model?.nodes || [],
        edges: model?.edges || [],
        tooltip: $("tooltip").innerText,
        tooltipVisible: !$("tooltip").hidden,
        status: $("status").textContent,
        images: document.images.length,
      };
    },
  };
  $("close").addEventListener("click", () => notify("map-close", {}));
  $("rebuild").addEventListener("click", () => notify("map-rebuild", {}));
  $("layer").addEventListener("change", render);
  $("query").addEventListener("input", render);
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") hideTip();
  });
  new ResizeObserver(() => {
    connect();
    refreshTip();
  }).observe($("cards"));
  window.addEventListener("resize", refreshTip);
  notify("map-ready", {});
}

if (typeof document !== "undefined") mountMap();
