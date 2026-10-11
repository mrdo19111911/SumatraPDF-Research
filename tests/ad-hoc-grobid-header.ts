// GROBID header model port (src/GrobidHeader.cpp): feature rows and title / authors / abstract.
import { existsSync } from "node:fs";
import { join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone } from "./util";

// FeaturesVectorHeader.printVector(): 31 features + label "0"
const nColumns = 32;

// PDFs named "NNNN-<Title>.pdf" (title as cited; ':' and '/' became '_', long names cut)
const dirs = [resolve("research/benchmark-2026-10-11/1912.02928v4"), resolve("research/final-2026-10-11/paper2")];

// title hit rate the port must reach over all PDFs above
const minHitRate = 0.7;

type Expect = { prefix: string; author?: string; abstract?: string };

const expects: Expect[] = [
  { prefix: "0006-On symplectic optimization", author: "Betancourt", abstract: "Accelerated gradient methods" },
  {
    prefix: "0004-A conceptual introduction",
    author: "Betancourt",
    abstract: "Hamiltonian Monte Carlo has proven",
  },
  { prefix: "0030-Hamiltonian descent methods", author: "Maddison", abstract: "We propose a family" },
];

type Header = {
  title: string;
  authors: string;
  abstract: string;
  keywords: string;
  affiliation: string;
  email: string;
  date: string;
  doi: string;
  venue: string;
};

function listPdfs(): string[] {
  const out: string[] = [];
  for (const dir of dirs) {
    if (!existsSync(dir)) throw new Error(`Missing ${dir}`);
    for (const name of new Bun.Glob("*.pdf").scanSync(dir)) out.push(join(dir, name));
  }
  return out.sort();
}

function baseName(path: string): string {
  return path.replace(/^.*[\\/]/, "").replace(/\.pdf$/i, "");
}

function fileTitle(path: string): string {
  return baseName(path).replace(/^\d+-/, "");
}

// lowercase letters and digits only: file names replace ':' etc., PDFs split accents
function norm(s: string): string {
  return s.toLowerCase().replace(/[^\p{L}\p{N}]/gu, "");
}

function titleHit(got: string, want: string): boolean {
  const g = norm(got);
  const w = norm(want);
  if (g.length === 0) return false;
  if (g === w) return true;
  // long file names are cut
  return w.length >= 60 && g.startsWith(w);
}

export async function testit(): Promise<void> {
  const failures: string[] = [];
  const pdfs = listPdfs();
  await withControlledSumatra(EXE, async (client) => {
    async function probe(action: string, a = "", b = "", c = "") {
      const result = await client.request(ControlCommand.TestResearchTools, [action, a, b, c]);
      return { code: result[0] as number, text: String(result[1]) };
    }

    // feature rows: one per token, 32 space-separated columns, label column "0"
    const pdf0 = pdfs.find((p) => baseName(p).startsWith(expects[0]!.prefix))!;
    const feat = await probe("grobid-headfeatures", pdf0);
    const rows = feat.text.split("\n").filter((r) => r.length > 0);
    if (feat.code !== 0) failures.push(`headfeatures: code ${feat.code}`);
    if (rows.length < 50) failures.push(`headfeatures: only ${rows.length} rows`);
    for (const row of rows) {
      const cols = row.split(" ");
      if (cols.length !== nColumns || cols[nColumns - 1] !== "0") {
        failures.push(`headfeatures: bad row '${row}'`);
        break;
      }
      const ok =
        /^BLOCK(START|IN|END)$/.test(cols[10]!) &&
        /^LINE(START|IN|END)$/.test(cols[11]!) &&
        /^(ALIGNEDLEFT|LINEINDENT)$/.test(cols[12]!) &&
        /^(NEWFONT|SAMEFONT)$/.test(cols[13]!) &&
        /^(HIGHERFONT|SAMEFONTSIZE|LOWERFONT)$/.test(cols[14]!);
      if (!ok) {
        failures.push(`headfeatures: bad status columns '${row}'`);
        break;
      }
    }
    console.log(`headfeatures: ${rows.length} rows`);

    let hits = 0;
    for (const pdf of pdfs) {
      const res = await probe("grobid-header", pdf);
      let h: Header;
      try {
        h = JSON.parse(res.text);
      } catch {
        failures.push(`${baseName(pdf)}: bad json '${res.text.slice(0, 200)}'`);
        continue;
      }
      const hit = titleHit(h.title, fileTitle(pdf));
      if (hit) hits++;
      console.log(`${hit ? "HIT " : "MISS"} ${baseName(pdf)}`);
      console.log(`     title:    ${h.title}`);
      console.log(`     authors:  ${h.authors}`);
      console.log(`     abstract: ${h.abstract.slice(0, 80)}`);
      if (h.keywords || h.date || h.doi || h.venue)
        console.log(`     kw: ${h.keywords} | date: ${h.date} | doi: ${h.doi} | venue: ${h.venue}`);

      const e = expects.find((x) => baseName(pdf).startsWith(x.prefix));
      if (!e) continue;
      if (res.code !== 0) failures.push(`${e.prefix}: code ${res.code}`);
      if (!hit) failures.push(`${e.prefix}: title '${h.title}'`);
      if (e.author && !h.authors.includes(e.author)) failures.push(`${e.prefix}: authors '${h.authors}'`);
      if (e.abstract && !h.abstract.startsWith(e.abstract))
        failures.push(`${e.prefix}: abstract '${h.abstract.slice(0, 100)}'`);
    }
    const rate = hits / pdfs.length;
    console.log(`title hit rate: ${hits}/${pdfs.length} = ${(rate * 100).toFixed(0)}%`);
    if (rate < minHitRate) failures.push(`title hit rate ${hits}/${pdfs.length} below ${minHitRate}`);
  });
  if (failures.length > 0) throw new Error(failures.join("\n"));
  console.log("PASS");
}

if (import.meta.main) {
  await runStandalone(testit);
}
