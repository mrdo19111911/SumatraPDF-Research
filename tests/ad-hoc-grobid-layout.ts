// GROBID Document model built from mupdf (src/GrobidDoc.cpp): checks the invariants
// PDFALTOSaxHandler / Document.java give the token stream, blocks, pages and images.
// Word content is compared with poppler's pdftotext (xpdf-based, like pdfalto).
import { spawnSync } from "node:child_process";
import { resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone } from "./util";

const semantic = "E:/Work/Research_Papers/data/semantic_search/pdf";
const paper2 = resolve("research/final-2026-10-11/paper2");

const FIGURE_PDF = `${semantic}/W2593864460.pdf`; // bitmap figure on page 10
const LONG_PDF = `${semantic}/W3128793550.pdf`; // 28 pages
const ARXIV_PDF = `${paper2}/0005-Imaginary quadratic number fields with class groups of small exponent.pdf`;
const HUGE_PDF = `${paper2}/0004-Bias in cubic Gauss sums_ Patterson's conjecture.pdf`; // 79 pages
const MAX_MS_PER_PAGE = 60; // debug build
const bench = resolve("research/benchmark-2026-10-11/1912.02928v4");
const SYMPLECTIC_PDF = `${bench}/0006-On symplectic optimization.pdf`;
const KINETIC_PDF = `${bench}/0029-Kinetic energy choice in Hamiltonian_hybrid Monte Carlo.pdf`;
const CONTACT_PDF = `${bench}/0024-New contributions to the Hamiltonian and Lagrangian contact formalisms for dissipative mechanical sy.pdf`;
const TWISTS_PDF = `${paper2}/0002-L-Functions with n-th-Order Twists.pdf`;

// TeX spacing accents drawn over a letter, composed like pdfalto: [pdf, words that must appear]
const accentCases: [string, string[]][] = [
  [SYMPLECTIC_PDF, ["León", "José"]],
  [KINETIC_PDF, ["Barthelmé", "BARTHELMÉ", "Rodríguez", "RODRÍGUEZ", "MÜLLER", "Störmer", "Dieudonné"]],
  [CONTACT_PDF, ["Gràcia"]],
  [TWISTS_PDF, ["BENOÎT", "Göttingen", "Fédérale", "Lindelöf", "idèle"]],
];
const spacingAccents = /[´`ˆ¨˜¸ˇ˘¯˙˚˝˛]/;

interface Tok {
  i: number;
  page: number;
  text: string;
  font: string;
  size: number;
  flags: string;
  x: number;
  y: number;
  w: number;
  h: number;
}
interface Block {
  page: number;
  start: number;
  end: number;
  first: number;
  n: number;
  x: number;
  y: number;
  w: number;
  h: number;
}
interface Page {
  n: number;
  w: number;
  h: number;
  firstBlock: number;
  nBlocks: number;
  lengthChar: number;
  main: number[];
}
interface Image {
  page: number;
  vector: boolean;
  x: number;
  y: number;
  w: number;
  h: number;
}
interface Layout {
  doc: Record<string, number>;
  pages: Page[];
  blocks: Block[];
  images: Image[];
  toks: Tok[];
}

function unescape(s: string): string {
  return s.replace(/\\(.)/g, (_, c) => (c === "n" ? "\n" : c === "t" ? "\t" : c));
}

// dump lines are tab separated: doc / page / block / image / tok records
function parseDump(dump: string): Layout {
  const res: Layout = { doc: {}, pages: [], blocks: [], images: [], toks: [] };
  for (const line of dump.split("\n")) {
    if (!line) continue;
    const f = line.split("\t");
    const num = (i: number) => Number(f[i]);
    switch (f[0]) {
      case "doc":
        for (const kv of f.slice(1)) {
          const [k, v] = kv.split("=");
          res.doc[k!] = Number(v);
        }
        break;
      case "page":
        res.pages.push({
          n: num(1),
          w: num(2),
          h: num(3),
          firstBlock: num(4),
          nBlocks: num(5),
          lengthChar: num(6),
          main: [num(7), num(8), num(9), num(10)],
        });
        break;
      case "block":
        res.blocks.push({
          page: num(2),
          start: num(3),
          end: num(4),
          first: num(5),
          n: num(6),
          x: num(7),
          y: num(8),
          w: num(9),
          h: num(10),
        });
        break;
      case "image":
        res.images.push({ page: num(1), vector: f[2] === "vector", x: num(3), y: num(4), w: num(5), h: num(6) });
        break;
      case "tok":
        res.toks.push({
          i: num(1),
          page: num(2),
          text: unescape(f[3]!),
          font: f[4]!,
          size: num(5),
          flags: f[6]!,
          x: num(7),
          y: num(8),
          w: num(9),
          h: num(10),
        });
        break;
      default:
        throw new Error(`unknown dump line: ${line}`);
    }
  }
  return res;
}

function check(cond: boolean, msg: string) {
  if (!cond) throw new Error(msg);
}

const isSep = (t: string) => t === " " || t === "\n";

// words of 3+ letters: pdftotext joins hyphenated words, sub/superscripts and plot
// labels ("10" "3" -> "103") that pdfalto keeps apart, and orders blocks differently
function words(text: string): Map<string, number> {
  const m = new Map<string, number>();
  const norm = text
    .normalize("NFKD")
    .toLowerCase()
    .replace(/-\n/g, "")
    .replace(/[^a-z]+/g, " ");
  for (const w of norm.split(" ")) {
    if (w.length >= 3) m.set(w, (m.get(w) ?? 0) + 1);
  }
  return m;
}

function dice(a: Map<string, number>, b: Map<string, number>): number {
  let common = 0,
    na = 0,
    nb = 0;
  for (const [w, c] of a) {
    na += c;
    common += Math.min(c, b.get(w) ?? 0);
  }
  for (const c of b.values()) nb += c;
  return na + nb === 0 ? 1 : (2 * common) / (na + nb);
}

function popplerPages(pdf: string): string[] {
  const r = spawnSync("pdftotext", ["-enc", "UTF-8", pdf, "-"], { encoding: "utf8", maxBuffer: 64 << 20 });
  if (r.status !== 0) throw new Error(`pdftotext failed: ${r.stderr}`);
  const pages = r.stdout.split("\f");
  pages.pop(); // after the last form feed
  return pages;
}

// GROBID invariants that hold for any document
function checkInvariants(name: string, L: Layout) {
  const toks = L.toks;
  check(L.doc.tokens === toks.length, `${name}: token count`);
  toks.forEach((t, i) => check(t.i === i, `${name}: token index ${i}`));

  // " " follows each pdfalto String unless its last sub-token ends with "-"
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i]!;
    check(t.text.length > 0, `${name}: empty token ${i}`);
    check(t.page >= 1 && t.page <= L.pages.length, `${name}: token ${i} page ${t.page}`);
    // a " " with a width is a sub-token (TextUtilities.clean turns U+2217 into " * ")
    const isSeparator = (k: number) => isSep(toks[k]!.text) && toks[k]!.w === 0;
    if (t.text === "\n") check(t.w === 0, `${name}: line end ${i} has width`);
    if (isSeparator(i)) {
      if (t.text === " ") {
        const prev = toks[i - 1];
        check(!!prev && !isSeparator(i - 1) && !prev.text.endsWith("-"), `${name}: " " at ${i} after "${prev?.text}"`);
      }
      continue;
    }
    if (isSep(t.text)) continue;
    check(t.font.length > 0 && t.font === t.font.toLowerCase(), `${name}: font "${t.font}" at ${i}`);
    check(t.size > 0, `${name}: size at ${i}`);
    const next = toks[i + 1];
    if (next && next.text === "\n") {
      check(t.text.endsWith("-"), `${name}: word "${t.text}" at ${i} ends a line without " "`);
    }
  }

  let sum = 0;
  let pageBlocks = 0;
  for (let p = 0; p < L.pages.length; p++) {
    const pg = L.pages[p]!;
    check(pg.n === p + 1, `${name}: page number`);
    check(pg.firstBlock === pageBlocks, `${name}: page ${pg.n} firstBlock`);
    let len = 0;
    for (let b = pg.firstBlock; b < pg.firstBlock + pg.nBlocks; b++) {
      const blk = L.blocks[b]!;
      check(blk.page === pg.n, `${name}: block ${b} page`);
      len += blk.n;
    }
    check(pg.lengthChar === len, `${name}: page ${pg.n} lengthChar ${pg.lengthChar} != ${len}`);
    pageBlocks += pg.nBlocks;
    const [mx, my, mw, mh] = pg.main;
    check(mx! >= 0 && my! >= 0 && mw! > 0 && mh! > 0, `${name}: page ${pg.n} main area ${pg.main}`);
    check(pg.w > 0 && pg.h > 0, `${name}: page ${pg.n} size`);
    const vectors = L.images.filter((im) => im.page === pg.n && im.vector);
    // figure-sized groups of paths only: no rules, no page backgrounds
    for (const v of vectors) {
      check(v.w >= 20 && v.h >= 20, `${name}: page ${pg.n} small vector box ${v.w}x${v.h}`);
      check(v.w * v.h <= 0.8 * pg.w * pg.h, `${name}: page ${pg.n} page-sized vector box`);
    }
  }
  check(pageBlocks === L.blocks.length, `${name}: pages own all blocks`);

  let prevEnd = 0;
  for (let b = 0; b < L.blocks.length; b++) {
    const blk = L.blocks[b]!;
    sum += blk.n;
    check(blk.first === blk.start, `${name}: block ${b} first ${blk.first} != start ${blk.start}`);
    check(blk.first >= prevEnd, `${name}: block ${b} overlaps previous`);
    prevEnd = blk.first + blk.n;
    check(blk.end > blk.start && blk.end <= prevEnd, `${name}: block ${b} end ${blk.end}`);
    const last = toks[prevEnd - 1]!;
    check(
      last.text === "\n" && toks[prevEnd - 2]?.text === "\n",
      `${name}: block ${b} must end with line + block "\\n"`,
    );
    // endToken is set after the block's last String: what follows is only "\n"
    const beforeEnd = toks[blk.end - 1]!;
    check(
      beforeEnd.text === " " || beforeEnd.text.endsWith("-"),
      `${name}: block ${b} endToken after "${beforeEnd.text}"`,
    );
    for (let i = blk.end; i < prevEnd; i++) check(toks[i]!.text === "\n", `${name}: block ${b} tail token ${i}`);
    // block box = union of its tokens with coordinates (BoundingBoxCalculator.calculateOneBox)
    const eps = 0.05;
    for (let i = blk.first; i < prevEnd; i++) {
      const t = toks[i]!;
      if (t.w <= 0) continue;
      check(t.page === blk.page, `${name}: block ${b} token ${i} page`);
      check(
        t.x >= blk.x - eps &&
          t.y >= blk.y - eps &&
          t.x + t.w <= blk.x + blk.w + eps &&
          t.y + t.h <= blk.y + blk.h + eps,
        `${name}: token ${i} outside block ${b}`,
      );
    }
  }
  // Document.documentLenghtChar starts at -1 before produceStatistics adds block tokens
  check(L.doc.lengthChar === sum - 1, `${name}: documentLengthChar ${L.doc.lengthChar} != ${sum} - 1`);
  check(L.doc.maxSpacing >= 0 && L.doc.minSpacing > 0, `${name}: block spacing`);
}

function checkText(name: string, pdf: string, L: Layout) {
  const ref = popplerPages(pdf);
  check(ref.length === L.pages.length, `${name}: ${L.pages.length} pages, pdftotext has ${ref.length}`);
  const ours: string[] = ref.map(() => "");
  for (const t of L.toks) ours[t.page - 1] += t.text;
  // pdftotext drops some math glyphs (e.g. U+2113) and orders plot labels differently, so
  // formula or figure pages score lower; only the mean is checked
  let worst = 1;
  let total = 0;
  let n = 0;
  for (let p = 0; p < ref.length; p++) {
    const a = words(ours[p]!);
    const b = words(ref[p]!);
    if (b.size < 20) continue; // scanned or empty page
    const d = dice(a, b);
    worst = Math.min(worst, d);
    total += d;
    n++;
  }
  const mean = n ? total / n : 1;
  console.log(
    `${name}: ${L.pages.length} pages, ${L.blocks.length} blocks, ${L.toks.length} tokens, dice mean ${mean.toFixed(3)} worst ${worst.toFixed(3)}`,
  );
  check(mean >= 0.95, `${name}: text differs from pdftotext (mean dice ${mean.toFixed(3)})`);
}

export async function testit(): Promise<void> {
  await withControlledSumatra(EXE, async (client) => {
    const layout = async (pdf: string): Promise<[Layout, number]> => {
      const t0 = performance.now();
      const r = await client.request(ControlCommand.TestResearchTools, ["grobid-layout", pdf, "", ""]);
      const ms = performance.now() - t0;
      if (r[0] !== 0) throw new Error(`grobid-layout failed for ${pdf}: ${r[1]}`);
      return [parseDump(String(r[1])), ms];
    };

    for (const pdf of [
      FIGURE_PDF,
      LONG_PDF,
      ARXIV_PDF,
      `${semantic}/W2142920810.pdf`,
      `${paper2}/0002-L-Functions with n-th-Order Twists.pdf`,
    ]) {
      const name = pdf.split("/").pop()!.slice(0, 24);
      const [L, ms] = await layout(pdf);
      checkInvariants(name, L);
      checkText(name, pdf, L);
      const perPage = ms / L.pages.length;
      console.log(`${name}: ${ms.toFixed(0)} ms (${perPage.toFixed(1)} ms/page)`);
      check(perPage < MAX_MS_PER_PAGE, `${name}: too slow, ${perPage.toFixed(1)} ms/page`);
    }

    // fonts: the title is the largest text on page 1; bold and italic exist; superscripts are rare
    const [F] = await layout(FIGURE_PDF);
    // horizontal text only: the rotated arXiv stamp in the margin is set larger
    const p1 = F.toks.filter((t) => t.page === 1 && !isSep(t.text) && t.x > 50);
    const maxSize = Math.max(...p1.map((t) => t.size));
    const title = p1.find((t) => t.text === "Billion")!;
    check(!!title && title.size === maxSize, `title size ${title?.size} != page max ${maxSize}`);
    const words = F.toks.filter((t) => !isSep(t.text));
    const frac = (flag: string) => words.filter((t) => t.flags.includes(flag)).length / words.length;
    console.log(
      `fonts: bold ${frac("B").toFixed(3)} italic ${frac("I").toFixed(3)} sup ${frac("S").toFixed(3)} default ${words.filter((t) => t.font === "default").length}`,
    );
    check(frac("B") > 0.005 && frac("B") < 0.3, "bold fraction");
    // pdfalto italic = "italic" in the font name or the /Flags bit: TeX cmti9 is neither
    check(frac("I") < 0.3, "italic fraction");
    check(frac("S") < 0.05, "superscript fraction");
    check(new Set(words.map((t) => t.font)).size >= 3, "distinct fonts");

    // bitmap figure on page 10
    const bitmaps = F.images.filter((im) => !im.vector);
    console.log(`images: ${bitmaps.map((im) => `p${im.page} ${im.w.toFixed(0)}x${im.h.toFixed(0)}`).join(", ")}`);
    check(
      bitmaps.some((im) => im.page === 10 && im.w > 50 && im.h > 50),
      "bitmap figure on page 10",
    );
    check(
      F.images.some((im) => im.vector && im.w > 0),
      "some page has vector graphics",
    );

    // accents: no letter keeps a spacing accent beside it; math accents on their own line stay
    for (const [pdf, want] of accentCases) {
      const name = pdf.split("/").pop()!.slice(0, 24);
      const [L] = await layout(pdf);
      checkInvariants(name, L);
      const text = L.toks.map((t) => t.text).join("");
      const missing = want.filter((w) => !text.includes(w));
      const glued = L.toks.filter(
        (t, i) =>
          /^\p{L}+$/u.test(t.text) &&
          (spacingAccents.test(L.toks[i - 1]?.text ?? "") || spacingAccents.test(L.toks[i + 1]?.text ?? "")),
      ).length;
      const inWord = L.toks.filter((t) => /\p{L}/u.test(t.text) && spacingAccents.test(t.text) && t.text.length > 1);
      console.log(
        `${name}: accents missing [${missing.join(", ")}], ${inWord.length} in words: ${inWord
          .slice(0, 8)
          .map((t) => t.text)
          .join(" ")}, ${glued} glued`,
      );
      check(missing.length === 0, `${name}: accented words missing: ${missing.join(", ")}`);
    }
    const [K] = await layout(KINETIC_PDF);
    check(
      K.toks.some((t) => t.text === "˜"),
      "standalone tilde over a formula on another line is kept",
    );

    // end to end: names in references and in the header
    const probe = async (action: string, pdf: string) => {
      const r = await client.request(ControlCommand.TestResearchTools, [action, pdf, "", ""]);
      if (r[0] !== 0) throw new Error(`${action} failed for ${pdf}: ${r[1]}`);
      return String(r[1]);
    };
    const refs: { authors: string }[] = JSON.parse(await probe("grobid-refs", SYMPLECTIC_PDF));
    const authors = refs.map((r) => r.authors).join("; ");
    console.log(`refs authors: ${authors.slice(0, 300)}`);
    check(/[Dd]e León, M\./.test(authors), "refs: de León, M.");
    check(authors.includes("José, J. V."), "refs: José, J. V.");
    const head: { authors: string } = JSON.parse(await probe("grobid-header", TWISTS_PDF));
    console.log(`header authors: ${head.authors}`);
    check(head.authors.includes("Louvel"), "header: Louvel");

    // a big document still builds fast
    const [H, ms] = await layout(HUGE_PDF);
    checkInvariants("huge", H);
    // ps2pdf draws rules as 1x1 image masks; pdfalto skips images of <= 8 pixels a side
    const hugeBitmaps = H.images.filter((im) => !im.vector).length;
    check(hugeBitmaps === 0, `huge: ${hugeBitmaps} bitmaps from rule masks`);
    console.log(`huge: ${H.pages.length} pages in ${ms.toFixed(0)} ms`);
    check(ms / H.pages.length < MAX_MS_PER_PAGE, "huge: too slow");
  });
}

if (import.meta.main) {
  await runStandalone(testit);
}
