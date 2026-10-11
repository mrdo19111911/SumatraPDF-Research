// GROBID segmentation port: feature rows (grobid-segfeatures) and labelled pieces (grobid-segment).
// Usage: bun tests/ad-hoc-grobid-segmentation.ts [--no-build] [--dump <pdf>]
import { existsSync, readdirSync, writeFileSync } from "node:fs";
import { basename, join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra, type ControlClient } from "./control";
import { EXE, runStandalone, tmpPath } from "./util";

const root = resolve(import.meta.dir, "..");
const NCOLS = 34;
const BLOCK_STATUS = new Set(["BLOCKSTART", "BLOCKIN", "BLOCKEND"]);
const PAGE_STATUS = new Set(["PAGESTART", "PAGEIN", "PAGEEND"]);
const FONT_STATUS = new Set(["NEWFONT", "SAMEFONT"]);
const FONT_SIZE = new Set(["HIGHERFONT", "SAMEFONTSIZE", "LOWERFONT"]);
// papers where GROBID's own answer misses the checks: 1929 OCR scan with footnote
// citations only and an OCR-garbage first line labelled <header>
// small benchmark papers whose file name is the title
const BENCHMARK = ["0006-", "0007-", "0012-", "0017-", "0029-", "0033-"];
const NO_BIBLIOGRAPHY = new Set(["0030-A divisor problem.pdf"]);
const HEADING = /^[ \t]*(?:\d+\.?\s*)?(references|bibliography|literature cited|works cited)[ \t]*$/gim;

function pdfsInDir(dir: string, max: number): string[] {
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .filter((f) => f.toLowerCase().endsWith(".pdf"))
    .sort()
    .slice(0, max)
    .map((f) => join(dir, f));
}

function testPdfs(): string[] {
  return [
    ...pdfsInDir("E:\\Work\\Research_Papers\\data\\semantic_search\\pdf", 8),
    ...pdfsInDir(join(root, "research", "final-2026-10-11", "paper2"), 6),
    ...pdfsInDir(join(root, "research", "benchmark-2026-10-11", "1912.02928v4"), 40).filter((f) =>
      BENCHMARK.some((n) => basename(f).startsWith(n)),
    ),
  ];
}

async function probe(client: ControlClient, action: string, pdf: string): Promise<{ code: number; out: string }> {
  const r = await client.request(ControlCommand.TestResearchTools, [action, pdf, "", ""]);
  return { code: Number(r[0]), out: String(r[1] ?? "") };
}

type Piece = { label: string; start: number; end: number; text: string; offset: number };

// "label\tstart\tend\ttext\n" per piece; text spans lines
function parsePieces(out: string): Piece[] {
  const re = /^(<[^>\t\n]+>)\t(-?\d+)\t(-?\d+)\t/gm;
  const heads: { label: string; start: number; end: number; at: number; textAt: number }[] = [];
  for (let m = re.exec(out); m; m = re.exec(out)) {
    heads.push({ label: m[1]!, start: +m[2]!, end: +m[3]!, at: m.index, textAt: m.index + m[0].length });
  }
  const pieces: Piece[] = [];
  let offset = 0;
  for (let i = 0; i < heads.length; i++) {
    const h = heads[i]!;
    const stop = i + 1 < heads.length ? heads[i + 1]!.at : out.length;
    const text = out.slice(h.textAt, stop);
    pieces.push({ label: h.label, start: h.start, end: h.end, text, offset });
    offset += text.length;
  }
  return pieces;
}

function checkRows(name: string, out: string): string[] {
  const errors: string[] = [];
  const rows = out.split("\n").filter((l) => l.length > 0);
  if (rows.length < 20) errors.push(`${name}: only ${rows.length} feature rows`);
  for (let i = 0; i < rows.length; i++) {
    const cols = rows[i]!.split(" ");
    const bad = (why: string) => errors.push(`${name}: row ${i} ${why}: ${rows[i]}`);
    if (cols.length !== NCOLS) {
      bad(`has ${cols.length} columns`);
      continue;
    }
    if (cols.some((c) => c.length === 0)) bad("has an empty column");
    if (cols[3] !== Array.from(cols[0]!)[0]) bad("prefix1 != first char");
    if (!BLOCK_STATUS.has(cols[7]!)) bad("bad block status");
    if (!PAGE_STATUS.has(cols[8]!)) bad("bad page status");
    if (!FONT_STATUS.has(cols[9]!)) bad("bad font status");
    if (!FONT_SIZE.has(cols[10]!)) bad("bad font size");
    if (cols[NCOLS - 1] !== "0") bad("last column not 0");
    if (errors.length > 5) break;
  }
  const first = rows[0]?.split(" ") ?? [];
  if (first[8] !== "PAGESTART") errors.push(`${name}: first row not PAGESTART`);
  if (first[9] !== "NEWFONT") errors.push(`${name}: first row not NEWFONT`);
  return errors;
}

function titleWords(pdf: string): string[] {
  const t = basename(pdf, ".pdf").replace(/^\d+-/, "");
  return t
    .toLowerCase()
    .split(/[^a-z]+/)
    .filter((w) => w.length >= 4);
}

function norm(s: string): string {
  return s.toLowerCase().replace(/[^a-z]+/g, " ");
}

function checkPieces(pdf: string, code: number, out: string): string[] {
  const name = basename(pdf);
  const errors: string[] = [];
  if (code !== 0) errors.push(`${name}: grobid-segment failed (${out.slice(0, 200)})`);
  const pieces = parsePieces(out);
  const labels = [...new Set(pieces.map((p) => p.label))].join(" ");
  console.log(`  ${name}: ${pieces.length} pieces: ${labels}`);

  const header = pieces.find((p) => p.label === "<header>");
  const body = pieces.find((p) => p.label === "<body>");
  const refs = pieces.find((p) => p.label === "<references>");
  if (!body) errors.push(`${name}: no <body>`);
  if (!header) errors.push(`${name}: no <header>`);
  if (NO_BIBLIOGRAPHY.has(name)) {
    if (refs) errors.push(`${name}: unexpected <references>`);
    return errors;
  }
  if (header && !pdf.includes("semantic_search")) {
    const words = titleWords(pdf);
    const text = norm(
      pieces
        .filter((p) => p.label === "<header>")
        .map((p) => p.text)
        .join(" "),
    );
    const found = words.filter((w) => text.includes(w));
    if (found.length * 3 < words.length * 2) errors.push(`${name}: <header> lacks title words ${words.join(",")}`);
  }
  if (!refs) {
    errors.push(`${name}: no <references>`);
    return errors;
  }
  const all = pieces.map((p) => p.text).join("");
  // headings near the piece start; one must sit right before or at it (a paper can repeat it)
  const headings: RegExpExecArray[] = [];
  HEADING.lastIndex = 0;
  for (let m = HEADING.exec(all); m; m = HEADING.exec(all)) {
    if (m.index <= refs.offset + 200) headings.push(m);
  }
  const refText = refs.text.replace(/\s+/g, " ").trim();
  console.log(`    references @${refs.start}: ${refText.slice(0, 140)}`);
  if (headings.length === 0) {
    console.log(`    (no references heading found before the references piece)`);
  } else {
    const fits = headings.some((h) => refs.offset >= h.index - 2 && refs.offset <= h.index + h[0].length + 120);
    const last = headings[headings.length - 1]!;
    if (!fits) errors.push(`${name}: <references> starts at ${refs.offset}, heading at ${last.index}`);
  }
  if (refText.length < 200) errors.push(`${name}: <references> too short`);
  return errors;
}

export async function testit(): Promise<void> {
  const dumpAt = process.argv.indexOf("--dump");
  const pdfs = dumpAt > 0 ? [resolve(process.argv[dumpAt + 1]!)] : testPdfs();
  if (pdfs.length === 0) throw new Error("no test PDFs found");
  const errors: string[] = [];
  await withControlledSumatra(EXE, async (client) => {
    for (const pdf of pdfs) {
      const name = basename(pdf);
      const feats = await probe(client, "grobid-segfeatures", pdf);
      const seg = await probe(client, "grobid-segment", pdf);
      if (dumpAt > 0) {
        const base = tmpPath(`grobid-seg-${name}`);
        writeFileSync(`${base}.features.txt`, feats.out);
        writeFileSync(`${base}.pieces.txt`, seg.out);
        console.log(`dumped ${base}.features.txt / .pieces.txt`);
      }
      errors.push(...checkRows(name, feats.out));
      errors.push(...checkPieces(pdf, seg.code, seg.out));
    }
  });
  if (errors.length > 0) throw new Error(`${errors.length} failures:\n${errors.join("\n")}`);
}

if (import.meta.main) {
  await runStandalone(testit);
}
