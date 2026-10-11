import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";

type Attempt = { source: string; query: string; url: string; status: string; error: string };
type Row = {
  raw: string;
  title: string;
  doi: string;
  arxiv: string;
  path: string;
  source: string;
  status: string;
  error: string;
  attempts?: Attempt[];
};
type Audit = {
  sourcePaper: string;
  index: number;
  originalCitation: string;
  canonicalTitle: string;
  metadataURL: string;
  metadataTitleInCitation: boolean;
  originalDoi: string;
  originalArxiv: string;
};
type Kind = "paper" | "book" | "web";
type Entry = { paper: string; index: number; kind: Kind; original: Row; result: Row; audit?: Audit };

const args = process.argv.slice(2);
function value(flag: string): string {
  const i = args.indexOf(flag);
  if (i < 0 || !args[i + 1]) throw new Error(`Missing ${flag}`);
  return resolve(args[i + 1]);
}
const base = value("--base"),
  output = value("--out");
const retries = args.flatMap((v, i) => (v === "--retry" ? [resolve(args[i + 1])] : []));
const audit: Audit[] = JSON.parse(readFileSync(value("--audit"), "utf8"));
const kinds: Record<string, Kind> = args.includes("--types") ? JSON.parse(readFileSync(value("--types"), "utf8")) : {};
const papers = [...new Set(audit.map((a) => a.sourcePaper.replace(/\.pdf$/i, "")))];
const entries: Entry[] = [];
for (const paper of papers) {
  const original: Row[] = JSON.parse(readFileSync(join(base, paper, "extracted.json"), "utf8")).rows;
  const states = [base, ...retries]
    .filter((p) => existsSync(join(p, paper, "collection.json")))
    .map((p) => JSON.parse(readFileSync(join(p, paper, "collection.json"), "utf8")).rows as Row[]);
  for (const [i, row] of original.entries()) {
    let result = row;
    for (const rows of states) {
      const candidate = rows.find((r) => r.raw === row.raw);
      if (candidate && !result.path) result = candidate;
    }
    entries.push({
      paper,
      index: i + 1,
      kind: kinds[`${paper}-${i + 1}`] || "paper",
      original: row,
      result,
      audit: audit.find((a) => a.originalCitation === row.raw && a.sourcePaper === paper + ".pdf"),
    });
  }
}
const escape = (s = "") =>
  s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]!);
const link = (url: string) => (/^https?:\/\//i.test(url) ? `<a href="${escape(url)}">${escape(url)}</a>` : escape(url));
const title = (e: Entry) =>
  (e.result.path ? e.result.title : e.audit?.canonicalTitle || e.result.title || e.original.raw).replace(
    /<[^>]*>/g,
    "",
  );
const sourceAttempts = (e: Entry, source: string) => (e.result.attempts || []).filter((a) => a.source === source);
function short(e: Entry, source: string): string {
  const attempts = sourceAttempts(e, source);
  if (!attempts.length) return "Không có nhật ký truy vấn";
  const a = attempts.find((a) => a.error) || attempts.at(-1)!;
  if (a.status.startsWith("Skipped: no DOI")) return "Bỏ qua: chưa có DOI";
  if (a.error.includes("12175")) return "Kết nối HTTPS thất bại (12175)";
  if (a.error.includes("No PDF link found")) return "Không lấy được link PDF từ trang trả về";
  if (a.error.includes("HTTP 404")) return "HTTP 404";
  if (a.error.includes("HTTP 403")) return "HTTP 403";
  if (a.error.includes("HTTP 429")) return "HTTP 429";
  if (a.error) return a.error;
  if (source === "arXiv") return "Không có kết quả khớp sau các truy vấn đã ghi";
  if (a.status === "No PDF links in response") return "Phản hồi tìm kiếm không chứa link PDF dùng được";
  return a.status;
}
const missing = entries.filter((e) => !e.result.path);
let html = `<!doctype html><html lang="vi"><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src 'none';style-src 'unsafe-inline';script-src 'unsafe-inline'"><title>Reference chưa tải được</title><style>body{font:15px Segoe UI,sans-serif;margin:24px;color:#222;background:#f6f7f9}table{border-collapse:collapse;width:100%;background:white}td,th{padding:10px;border:1px solid #ddd;vertical-align:top;text-align:left}th{background:#e5eaf1;position:sticky;top:0}small{display:block;color:#555;margin:5px 0}a{overflow-wrap:anywhere}h3{margin:0}input{font:inherit;padding:8px;width:60%}.failed{color:#a22}p{max-width:1100px}details{margin:8px 0}</style><h1>Vì sao chưa tải được reference?</h1><p>${missing.length} reference chưa tải được trong hai paper đã thử. “Không khớp” chỉ mô tả kết quả truy vấn, không chứng minh paper không có trên arXiv. “Không lấy được link” cũng không chứng minh Sci-Hub không có bài. Tên chuẩn từ metadata độc lập được ghi riêng với câu truy vấn thực tế.</p><p>Báo cáo do <code>cmd/library-report.ts</code> tạo từ JSON của các lần chạy. Không tính link tìm thấy là PDF đã tải.</p><label>Lọc tên / lỗi: <input id="filter"></label>`;
let md = `Các reference chưa tải được sau khi thử lại hai paper. Chi tiết truy vấn và URL nằm trong HTML cùng thư mục.\n\n`;
function render(e: Entry): string {
  let attempts = "";
  for (const source of ["arXiv", "Sci-Hub", "Google", "Crossref", "Anna's Archive"]) {
    const list = sourceAttempts(e, source);
    if (!list.length) {
      attempts += `<p><b>${escape(source)}</b>: không có nhật ký truy vấn.</p>`;
      continue;
    }
    attempts += `<details><summary>${escape(source)}: ${escape(short(e, source))}</summary>`;
    for (const a of list)
      attempts += `<p><b>${escape(a.status)}</b><small>Query: ${escape(a.query || "—")}</small><small>${escape(a.error || "Không có lỗi kết nối được ghi")}</small>${link(a.url)}</p>`;
    attempts += "</details>";
  }
  const kind = { paper: "Bài báo", book: "Sách", web: "Trang web / công thức" }[e.kind];
  return `<tr><td>${escape(e.paper)}<br>[${e.index}]<small>${kind}</small></td><td><h3>${escape(title(e))}</h3><details><summary>Nguồn trong paper</summary>${escape(e.original.raw)}<small>DOI gốc: ${escape(e.original.doi || "không ghi")}; ID arXiv gốc: ${escape(e.original.arxiv || "không ghi")}</small></details><small>Tên dùng để tìm: ${escape(e.result.title)}</small>${e.audit?.metadataURL ? `<small>Metadata độc lập: ${link(e.audit.metadataURL)}</small>` : ""}</td><td>${e.result.path ? `Đã tải: ${escape(e.result.source)}<small>${escape(e.result.path)}</small>` : `<b class="failed">Chưa tải</b><small>${escape(e.result.error)}</small>`}</td><td>${attempts}</td></tr>`;
}
for (const paper of papers) {
  const rows = missing.filter((e) => e.paper === paper);
  html += `<h2>${escape(paper)}.pdf — ${rows.length} reference chưa tải</h2><table><thead><tr><th>Reference</th><th>Tên / citation</th><th>Kết quả</th><th>Nguồn đã thử / lỗi</th></tr></thead><tbody>${rows.map(render).join("")}</tbody></table>`;
  md += `**${paper}.pdf**\n\n| # | Tên bài | arXiv | Sci-Hub | Google |\n|---|---|---|---|---|\n`;
  const cell = (s: string) => s.replaceAll("|", "\\|").replace(/\s+/g, " ");
  for (const e of rows)
    md += `| ${e.index} | ${cell(title(e))} | ${cell(short(e, "arXiv"))} | ${cell(short(e, "Sci-Hub"))} | ${cell(short(e, "Google"))} |\n`;
  md += "\n";
}
html += `<details><summary>Reference đã tải</summary><table><tbody>${entries
  .filter((e) => e.result.path)
  .map(render)
  .join(
    "",
  )}</tbody></table></details><script>document.getElementById('filter').oninput=e=>{const q=e.target.value.toLowerCase();document.querySelectorAll('tbody tr').forEach(r=>r.hidden=!r.textContent.toLowerCase().includes(q))}</script></html>`;
mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, html);
writeFileSync(join(dirname(output), "UNRESOLVED.md"), md);
writeFileSync(join(dirname(output), "combined-results.json"), JSON.stringify(entries, null, 2));
console.log(`Generated ${output}; ${missing.length} unresolved references; ${entries.length} total`);
