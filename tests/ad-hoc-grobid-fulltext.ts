// GROBID fulltext port (src/GrobidFulltext.cpp): feature rows (grobid-fullfeatures) and body items (grobid-fulltext).
// Usage: bun tests/ad-hoc-grobid-fulltext.ts [--no-build] [--all] [--only <name part>] [--dump <dir>]
// --dump writes segment, features and items of each pdf to <dir>/fulltext-*.txt
import { existsSync, readdirSync, writeFileSync } from "node:fs";
import { basename, join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra, type ControlClient } from "./control";
import { EXE, runStandalone } from "./util";

const root = resolve(import.meta.dir, "..");

// FeaturesVectorFulltext.printVector(): 27 columns, no label column
const NCOLS = 27;
const BLOCK_STATUS = new Set(["BLOCKSTART", "BLOCKIN", "BLOCKEND"]);
const LINE_STATUS = new Set(["LINESTART", "LINEIN", "LINEEND"]);
const ALIGN_STATUS = new Set(["ALIGNEDLEFT", "LINEINDENT"]);
const FONT_STATUS = new Set(["NEWFONT", "SAMEFONT"]);
const FONT_SIZE = new Set(["HIGHERFONT", "SAMEFONTSIZE", "LOWERFONT"]);
const CALLOUT = new Set(["UNKNOWN", "NUMBER", "AUTHOR"]);
const LABELS = new Set([
  "<section>",
  "<paragraph>",
  "<figure>",
  "<table>",
  "<equation>",
  "<equation_label>",
  "<citation_marker>",
  "<figure_marker>",
  "<table_marker>",
  "<equation_marker>",
  "<section_marker>",
  "<other>",
]);

// benchmark papers skipped by default: 0003 is a 500-page book
const SKIP = ["0003-"];
const INTRO_LINE = /^[ \t]*(?:\d+\.?|[IVX]+\.)?[ \t]*Introduction\.?[ \t]*$/im;
const CAPTION = /^[ \t]*(?:Figure|Fig\.)[ \t]*(\d+)[ \t]*[.:|]/gim;
const NUM_CITE = /\[\d+(?:\s*[,–-]\s*\d+)*\]/g;

function pdfsInDir(dir: string): string[] {
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .filter((f) => f.toLowerCase().endsWith(".pdf"))
    .sort()
    .map((f) => join(dir, f));
}

function argValue(name: string): string | undefined {
  const i = process.argv.indexOf(name);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

function testPdfs(): string[] {
  const all = process.argv.includes("--all");
  const only = argValue("--only");
  return [
    ...pdfsInDir("E:\\Work\\Research_Papers\\data\\semantic_search\\pdf"),
    ...pdfsInDir(join(root, "research", "final-2026-10-11", "paper2")),
    ...pdfsInDir(join(root, "research", "benchmark-2026-10-11", "1912.02928v4")).filter(
      (f) => all || !SKIP.some((p) => basename(f).startsWith(p)),
    ),
  ].filter((f) => !only || basename(f).includes(only));
}

async function probe(client: ControlClient, action: string, pdf: string): Promise<{ code: number; out: string }> {
  const r = await client.request(ControlCommand.TestResearchTools, [action, pdf, "", ""]);
  return { code: Number(r[0]), out: String(r[1] ?? "") };
}

type Piece = { label: string; start: number; end: number; text: string };

// "label\tstart\tend\ttext\n" per piece; text spans lines
function parsePieces(out: string): Piece[] {
  const re = /^(<[^>\t\n]+>)\t(-?\d+)\t(-?\d+)\t/gm;
  const heads: { label: string; start: number; end: number; at: number; textAt: number }[] = [];
  for (let m = re.exec(out); m; m = re.exec(out)) {
    heads.push({ label: m[1]!, start: +m[2]!, end: +m[3]!, at: m.index, textAt: m.index + m[0].length });
  }
  return heads.map((h, i) => {
    const stop = i + 1 < heads.length ? heads[i + 1]!.at : out.length;
    return { label: h.label, start: h.start, end: h.end, text: out.slice(h.textAt, stop) };
  });
}

type Item = {
  label: string;
  text: string;
  annex: boolean;
  start: number;
  end: number;
  page: number;
  x: number;
  y: number;
  w: number;
  h: number;
};

function checkRows(name: string, out: string, failures: string[]): number {
  const rows = out.split("\n").filter((r) => r.length > 0);
  for (const row of rows) {
    const c = row.split(" ");
    const bad =
      c.length !== NCOLS ||
      !BLOCK_STATUS.has(c[10]!) ||
      !LINE_STATUS.has(c[11]!) ||
      !ALIGN_STATUS.has(c[12]!) ||
      !FONT_STATUS.has(c[13]!) ||
      !FONT_SIZE.has(c[14]!) ||
      !/^\d+$/.test(c[21]!) ||
      !/^\d+$/.test(c[22]!) ||
      !CALLOUT.has(c[24]!);
    if (bad) {
      failures.push(`${name}: bad feature row '${row}'`);
      break;
    }
  }
  return rows.length;
}

// model-quality checks: each applies to some papers and must hold for a share of them,
// since the CRF (like GROBID itself) misses some, e.g. a heading followed by a footnote
type Quality = { name: string; minShare: number; tried: number; passed: number; misses: string[] };

const quality: Record<string, Quality> = {
  intro: { name: "Introduction heading -> <section>", minShare: 0.8, tried: 0, passed: 0, misses: [] },
  paragraphs: { name: "paragraphs cover >= 40% of body tokens", minShare: 0.85, tried: 0, passed: 0, misses: [] },
  figures: { name: "figures ~ distinct Figure N captions", minShare: 0.6, tried: 0, passed: 0, misses: [] },
  cites: { name: "numeric [N] citations -> <citation_marker>", minShare: 0.9, tried: 0, passed: 0, misses: [] },
};

function judge(key: string, name: string, ok: boolean, why: string) {
  const q = quality[key]!;
  q.tried++;
  if (ok) q.passed++;
  else q.misses.push(`${name}: ${why}`);
}

function checkItems(name: string, items: Item[], body: Piece[], failures: string[]): string {
  const fail = (s: string) => failures.push(`${name}: ${s}`);
  // body and annex are separate sequences, each in document order
  const prevEnd = { body: -1, annex: -1 };
  for (const it of items) {
    const seq = it.annex ? "annex" : "body";
    if (!LABELS.has(it.label)) fail(`unknown label ${it.label}`);
    if (it.start > it.end || it.start <= prevEnd[seq]) fail(`item out of order ${it.label} ${it.start}-${it.end}`);
    if (it.page < 1 || it.w < 0 || it.h < 0) fail(`bad box ${JSON.stringify(it)}`);
    prevEnd[seq] = it.end;
  }

  const bodyText = body.map((p) => p.text).join("\n");
  const bodyTokens = body.reduce((n, p) => n + p.end - p.start + 1, 0);
  const of = (l: string) => items.filter((it) => it.label === l);
  const sections = of("<section>");
  const figures = of("<figure>");
  const tables = of("<table>");
  const cites = of("<citation_marker>");

  if (INTRO_LINE.test(bodyText)) {
    const ok = sections.some((s) => /introduction/i.test(s.text));
    judge("intro", name, ok, `sections ${JSON.stringify(sections.map((s) => s.text).slice(0, 5))}`);
  }

  const paraTokens = of("<paragraph>")
    .filter((it) => !it.annex)
    .reduce((n, it) => n + it.end - it.start + 1, 0);
  const coverage = bodyTokens > 0 ? paraTokens / bodyTokens : 1;
  if (bodyTokens > 0) judge("paragraphs", name, coverage >= 0.4, `${(coverage * 100).toFixed(0)}%`);

  const captions = new Set<string>();
  for (const m of bodyText.matchAll(CAPTION)) captions.add(m[1]!);
  const nCap = captions.size;
  if (nCap > 0) {
    const ok = Math.abs(figures.length - nCap) <= Math.max(2, nCap / 2);
    judge("figures", name, ok, `${figures.length} figures for ${nCap} captions`);
  }

  const nNumCites = [...bodyText.matchAll(NUM_CITE)].length;
  if (nNumCites >= 3) {
    judge(
      "cites",
      name,
      cites.some((c) => /\d/.test(c.text)),
      `none for ${nNumCites} in text`,
    );
  }

  const intro = sections.find((s) => /introduction/i.test(s.text));
  return (
    `sections ${sections.length}, figures ${figures.length}/${nCap} captions, tables ${tables.length}, ` +
    `cites ${cites.length}, paragraphs ${(coverage * 100).toFixed(0)}%` +
    `${intro ? `, intro '${intro.text.slice(0, 30)}' p${intro.page}` : ""}`
  );
}

function checkQuality(failures: string[]) {
  for (const q of Object.values(quality)) {
    const share = q.tried > 0 ? q.passed / q.tried : 1;
    console.log(`${q.name}: ${q.passed}/${q.tried}`);
    for (const m of q.misses) console.log(`  miss ${m}`);
    if (share < q.minShare) failures.push(`${q.name}: ${q.passed}/${q.tried}, want ${q.minShare * 100}%`);
  }
}

export async function testit(): Promise<void> {
  const pdfs = testPdfs();
  if (pdfs.length === 0) throw new Error("no test pdfs");
  const failures: string[] = [];
  await withControlledSumatra(EXE, async (client) => {
    for (const pdf of pdfs) {
      const name = basename(pdf).slice(0, 40);
      const t0 = Date.now();
      const seg = await probe(client, "grobid-segment", pdf);
      const body = parsePieces(seg.out).filter((p) => p.label === "<body>");
      const feats = await probe(client, "grobid-fullfeatures", pdf);
      const nRows = checkRows(name, feats.out, failures);
      if (body.length > 0 && nRows === 0) failures.push(`${name}: no fulltext feature rows`);

      const ft = await probe(client, "grobid-fulltext", pdf);
      if (ft.code !== 0) {
        failures.push(`${name}: grobid-fulltext failed: ${ft.out.slice(0, 200)}`);
        continue;
      }
      let items: Item[] = [];
      try {
        items = JSON.parse(ft.out);
      } catch (e) {
        failures.push(`${name}: bad json ${String(e)} ${ft.out.slice(0, 200)}`);
        continue;
      }
      const dumpDir = argValue("--dump");
      if (dumpDir) {
        const base = join(dumpDir, `fulltext-${basename(pdf, ".pdf").slice(0, 20)}`);
        writeFileSync(`${base}-segment.txt`, seg.out);
        writeFileSync(`${base}-features.txt`, feats.out);
        writeFileSync(`${base}-items.txt`, items.map((it) => JSON.stringify(it)).join("\n"));
      }
      if (body.length > 0 && items.length === 0) failures.push(`${name}: no body items`);
      const summary = checkItems(name, items, body, failures);
      console.log(`${name}: rows ${nRows}, ${summary} (${Date.now() - t0} ms)`);
    }
  });
  checkQuality(failures);
  if (failures.length > 0) throw new Error(failures.join("\n"));
  console.log("PASS");
}

if (import.meta.main) {
  await runStandalone(testit);
}
