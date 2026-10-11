// GROBID reference-segmenter port (src/GrobidReferences.cpp): feature rows and entries.
import { existsSync } from "node:fs";
import { join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone } from "./util";

// FeaturesVectorReferenceSegmenter.printVector(): 27 features + label "0"
const nColumns = 28;

const dir = resolve("research/benchmark-2026-10-11/1912.02928v4");

// numbered bibliographies: entry count = highest label number
const numbered: [string, number][] = [
  ["0035-A continuous-time Nesterov", 34],
  ["0024-New contributions", 38],
  ["0007-Contact Hamiltonian dynamics", 64],
  ["0030-Hamiltonian descent methods", 61],
  ["0010-Numerical integration in celestial mechanics", 41],
  ["0012-Contact manifolds and dissipation", 29],
  ["0036-Explicit symplectic approximation", 53],
];

const authorYear = { prefix: "0006-On symplectic optimization", count: 13, first: "Betancourt, M." };

type Ref = { label: string; raw: string };

function findPdf(prefix: string): string {
  const glob = new Bun.Glob("*.pdf");
  for (const name of glob.scanSync(dir)) {
    if (name.startsWith(prefix)) return join(dir, name);
  }
  throw new Error(`No pdf for ${prefix} in ${dir}`);
}

function labelNumber(label: string): number {
  const m = label.match(/^\[?(\d+)[\].]?$/);
  return m ? Number(m[1]) : -1;
}

export async function testit(): Promise<void> {
  if (!existsSync(dir)) throw new Error(`Missing ${dir}`);
  const failures: string[] = [];
  await withControlledSumatra(EXE, async (client) => {
    async function probe(action: string, a = "", b = "", c = "") {
      const result = await client.request(ControlCommand.TestResearchTools, [action, a, b, c]);
      if (result[0] !== 0) throw new Error(`${action} ${a}: ${String(result[1]).slice(0, 300)}`);
      return String(result[1]);
    }

    // feature rows: one per token, 28 space-separated columns, label column "0"
    const pdf0 = findPdf(numbered[0]![0]);
    const rows = (await probe("grobid-reffeatures", pdf0)).split("\n").filter((r) => r.length > 0);
    if (rows.length < 500) failures.push(`reffeatures: only ${rows.length} rows`);
    for (const row of rows) {
      const cols = row.split(" ");
      if (cols.length !== nColumns || cols[nColumns - 1] !== "0") {
        failures.push(`reffeatures: bad row '${row}'`);
        break;
      }
      if (!/^LINE(START|IN|END)$/.test(cols[10]!) || !/^BLOCK(START|IN|END)$/.test(cols[25]!)) {
        failures.push(`reffeatures: bad line/block status '${row}'`);
        break;
      }
    }
    console.log(`reffeatures: ${rows.length} rows`);

    for (const [prefix, want] of numbered) {
      const refs: Ref[] = JSON.parse(await probe("grobid-refs", findPdf(prefix)));
      const nums = refs.map((r) => labelNumber(r.label.trim()));
      const inOrder = nums.every((n, i) => n === i + 1);
      console.log(`${prefix}: ${refs.length} refs (want ${want})${inOrder ? "" : `; labels ${nums.join(",")}`}`);
      if (refs.length !== want) failures.push(`${prefix}: ${refs.length} refs, want ${want}`);
      if (!inOrder) failures.push(`${prefix}: labels not 1..N`);
      for (const r of refs) {
        if (r.raw.length < 20) failures.push(`${prefix}: short entry '${r.label}' '${r.raw}'`);
      }
    }

    const refs: Ref[] = JSON.parse(await probe("grobid-refs", findPdf(authorYear.prefix)));
    console.log(`${authorYear.prefix}: ${refs.length} refs (want ${authorYear.count})`);
    if (refs.length !== authorYear.count)
      failures.push(`${authorYear.prefix}: ${refs.length} refs, want ${authorYear.count}`);
    if (!refs[0]?.raw.startsWith(authorYear.first)) failures.push(`${authorYear.prefix}: first '${refs[0]?.raw}'`);
    for (const r of refs) {
      // surname, possibly with a particle or a split accent: "de Le´on, M.", "Jos´e, J. V."
      if (!/^(?:(?:de|van|von|der|di|da|le|la) )*[A-Z][\p{L}'\-´¨`]+,/u.test(r.raw))
        failures.push(`${authorYear.prefix}: entry not starting with a surname '${r.raw}'`);
    }
  });
  if (failures.length > 0) throw new Error(failures.join("\n"));
  console.log("PASS");
}

if (import.meta.main) {
  await runStandalone(testit);
}
