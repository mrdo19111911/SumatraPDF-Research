import { readFileSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { runStandalone } from "./util";

const ROOT = resolve("research/title-benchmark-2026-10-11");
const MIN_TITLE_COVERAGE = 0.8;
const MIN_MATCH_MARGIN = 0.05;
const MIN_WEB_WORDS = 3;
const METHODS = ["delimiter", "structure", "scoring", "metadata", "web"] as const;
function norm(s: string): string {
  return s
    .replace(/<[^>]+>/g, " ")
    .replace(/\\operatorname\{([^}]+)\}/g, "$1")
    .replace(/\\([A-Za-z]+)/g, "$1")
    .normalize("NFKD")
    .replace(/\p{M}/gu, "")
    .toLowerCase()
    .replace(/[^a-z0-9]/g, "");
}
function words(s: string): string[] {
  return [
    ...new Set(
      s
        .replace(/<[^>]+>/g, " ")
        .replace(/\\operatorname\{([^}]+)\}/g, "$1")
        .replace(/\\([A-Za-z]+)/g, "$1")
        .normalize("NFKD")
        .replace(/\p{M}/gu, "")
        .toLowerCase()
        .match(/[a-z0-9]+/g) || [],
    ),
  ];
}
function coverage(title: string, raw: string): number {
  const ts = words(title),
    rs = new Set(words(raw));
  return ts.length ? ts.filter((w) => rs.has(w)).length / ts.length : 0;
}
function metadata(row: any) {
  const doi = row.raw
    .match(/10\.\d{4,9}\/[^\s]+/)?.[0]
    ?.replace(/[.,;]+$/g, "")
    .toLowerCase();
  const options = row.metadata
    .map((m: any) => {
      const title = m.title.replace(/<[^>]+>/g, " ").trim();
      const titleCoverage = coverage(title, row.raw);
      const exactDoi = Boolean(doi && doi === m.doi?.toLowerCase());
      const author = Boolean(m.authors?.some((a: any) => a.family && norm(row.raw).includes(norm(a.family))));
      const trusted = titleCoverage >= MIN_TITLE_COVERAGE && (exactDoi || (!doi && author));
      return { ...m, title, titleCoverage, trusted, rank: exactDoi ? 100 : words(title).length * titleCoverage };
    })
    .filter((m: any) => m.trusted)
    .sort((a: any, b: any) => b.rank - a.rank);
  const first = options[0];
  if (!first)
    return {
      title: "",
      reason: row.metadataStatus === 200 ? "No verified metadata match" : "HTTP " + row.metadataStatus,
    };
  const rival = options.find((m: any) => norm(m.title) !== norm(first.title));
  if (rival && first.rank - rival.rank < MIN_MATCH_MARGIN) return { title: "", reason: "Ambiguous metadata" };
  return { title: first.title, url: first.url, reason: "Title coverage and author / original DOI verified" };
}
function web(row: any, batches: any[]) {
  const batch = batches.find((b) => b.keys.includes(row.key));
  const originalDoi = row.raw
    .match(/10\.\d{4,9}\/[^\s]+/)?.[0]
    ?.replace(/[.,;]+$/g, "")
    .toLowerCase();
  const cs = (batch?.candidates || [])
    .map((c: any) => {
      const title = c.title.replace(/^\[\d+\]\s*/, "");
      const n = words(title).length;
      const match = coverage(title, row.raw);
      const exactDoi = Boolean(originalDoi && c.url.toLowerCase().includes(originalDoi));
      return { ...c, title, n, match, rank: n * match + (exactDoi ? 100 : 0) };
    })
    .filter(
      (c: any) =>
        c.match >= MIN_TITLE_COVERAGE &&
        (c.n >= MIN_WEB_WORDS || c.rank >= 100) &&
        !/^(?:volume|vol\.|article metrics|abstract|contents|table of contents)/i.test(c.title),
    )
    .sort((a: any, b: any) => b.rank - a.rank);
  if (!cs.length) return { title: "", reason: "No primary result title passed coverage threshold" };
  return {
    title: cs[0].title,
    url: cs[0].url,
    reason: "Primary search-result title; automatic selection, not human correction",
  };
}
function escape(s: unknown): string {
  return String(s ?? "").replace(
    /[&<>"']/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]!,
  );
}
export async function testit(): Promise<void> {
  const rows = JSON.parse(readFileSync(join(ROOT, "predictions.json"), "utf8"));
  const gold = JSON.parse(readFileSync(join(ROOT, "gold.json"), "utf8"));
  const batches = JSON.parse(readFileSync(join(ROOT, "web-candidates.json"), "utf8"));
  const firstRun = JSON.parse(readFileSync(join(ROOT, "evaluation-v1.json"), "utf8"));
  if (rows.length !== 30 || new Set(rows.map((r: any) => r.paper)).size !== 10)
    throw new Error("Need 30 references from 10 papers");
  const results = rows.map((row: any) => {
    const g = gold.find((g: any) => g.key === row.key);
    if (!g) throw new Error("Missing gold: " + row.key);
    const accepted = [g.title, ...(g.aliases || [])].map(norm);
    const predictions = { ...row.local, metadata: metadata(row), web: web(row, batches) };
    for (const method of METHODS) {
      const p = predictions[method];
      p.correct = accepted.includes(norm(p.title));
      p.outcome = p.correct ? "correct" : p.title ? "wrong" : "unresolved";
    }
    const candidateHit = row.local.delimiter.candidates.some((t: string) => accepted.includes(norm(t)));
    return { key: row.key, paper: row.paper, label: row.label, raw: row.raw, gold: g, predictions, candidateHit };
  });
  const summary = METHODS.map((method) => ({
    method,
    correct: results.filter((r: any) => r.predictions[method].correct).length,
    wrong: results.filter((r: any) => r.predictions[method].outcome === "wrong").length,
    unresolved: results.filter((r: any) => r.predictions[method].outcome === "unresolved").length,
    total: results.length,
  }));
  writeFileSync(
    join(ROOT, "evaluation.json"),
    JSON.stringify(
      { summary, delimiterCandidateRecall: results.filter((r: any) => r.candidateHit).length, results },
      null,
      2,
    ),
  );
  let html = `<!doctype html><html lang="vi"><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src 'none';style-src 'unsafe-inline'"><title>So sánh 5 cách nhận tên reference</title><style>body{font:15px Segoe UI,sans-serif;margin:24px}table{border-collapse:collapse;width:100%;margin:20px 0}th,td{border:1px solid #bbb;padding:9px;text-align:left;vertical-align:top}th{background:#eef2f7}details{margin:10px 0}.correct{color:#17643a}.wrong{color:#a00}.unresolved{color:#805500}small{display:block}article{border-top:1px solid #bbb;margin:20px 0}a{overflow-wrap:anywhere}</style><h1>5 cách nhận tên reference</h1><p>10 bài được lấy ngẫu nhiên từ 100 bài mới nhất thuộc math.OC, math.AG, math.NT, math.DG. Seed 20261011; 3 reference ngẫu nhiên mỗi bài. Tổng 30: 26 bài và 4 sách.</p><p>Đây là prototype thử thuật toán, không thay đổi mã ứng dụng. Input là bibliography HTML arXiv chuyển thành text; chưa thử lỗi OCR hoặc trích PDF. HTML có thể thêm External Links / Cited by. Web dùng công cụ tìm kiếm của phiên làm việc, không phải Google trong ứng dụng. Metadata thử Crossref, chưa thử OpenAlex.</p><p>Đúng: toàn bộ tên khớp sau chuẩn hóa dấu, hoa/thường, LaTeX; chấp nhận các tên khác đã ghi rõ trong gold.json. Thiếu phụ đề hoặc lẫn tạp chí vẫn tính sai. Tên chuẩn do đọc bibliography và đối chiếu nguồn riêng; ca chưa mở được trang gốc có ghi giới hạn.</p><p>Lượt đầu được giữ ở evaluation-v1.json và predictions-v1.json. Sau khi thấy lỗi parser lấy phần sau năm, đã viết 9 test tổng hợp, thấy test thất bại, sửa parser và thấy 9 test đạt. Chỉ cách structure thay đổi. Kết quả cuối là phép thử khám phá trên cùng mẫu, không phải đánh giá holdout độc lập. Crossref từng trả 429; các truy vấn đó được thử lại tuần tự cách nhau 1,25 giây, cuối cùng 30/30 HTTP 200. Không build hay mở ứng dụng.</p><table><tr><th>Cách</th><th>Đúng lượt đầu</th><th>Đúng sau sửa parser</th><th>Sai</th><th>Chưa xác định</th></tr>${summary.map((s) => `<tr><td>${escape(s.method)}</td><td>${firstRun.summary.find((v: any) => v.method === s.method)?.correct}/30</td><td>${s.correct}/30</td><td>${s.wrong}</td><td>${s.unresolved}</td></tr>`).join("")}</table><p>Recall tập ứng viên của cách dấu câu: ${results.filter((r: any) => r.candidateHit).length}/30. Chỉ số này không đồng nghĩa chọn đúng tên đầu tiên.</p><p>Những lỗi cần ưu tiên: dấu phẩy trong GL(n,F) bị tách tên; dấu hai chấm của phụ đề bị hiểu nhầm thành ranh giới tác giả; tiêu đề metadata thiếu subtitle; web chọn heading nhà xuất bản hoặc dòng tập/trang thay cho tên. Crossref không xác nhận được ba preprint trong mẫu; đây không phải bằng chứng bài không tồn tại. Không dùng điểm dài nhất hoặc kết quả đầu tiên làm tên đã xác nhận.</p>`;
  for (const r of results) {
    html += `<article><h2>${escape(r.paper)} ${escape(r.label)}</h2><p><b>Reference nguyên văn:</b> ${escape(r.raw)}</p><p><b>Tên chuẩn:</b> ${escape(r.gold.title)} (${escape(r.gold.kind)})</p><p>${r.gold.sources.map((u: string) => `<a href="${escape(u)}">${escape(u)}</a>`).join("<br>")}</p>${r.gold.verification ? `<p>${escape(r.gold.verification)}</p>` : ""}<table><tr><th>Cách</th><th>Tên đoán</th><th>Kết quả / lý do</th></tr>${METHODS.map(
      (method) => {
        const p = r.predictions[method];
        return `<tr><td>${method}</td><td>${escape(p.title)}${p.url ? `<small><a href="${escape(p.url)}">Nguồn</a></small>` : ""}</td><td class="${p.outcome}">${p.outcome}<small>${escape(p.reason)}</small></td></tr>`;
      },
    ).join("")}</table></article>`;
  }
  writeFileSync(join(ROOT, "report.html"), html + "</html>");
  console.log(JSON.stringify(summary, null, 2));
  console.log("Delimiter candidate recall", results.filter((r: any) => r.candidateHit).length);
}
if (import.meta.main) await runStandalone(testit);
