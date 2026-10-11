// src/CrfModel.cpp (grobid-crf action) against the Wapiti reference in tests/grobid-crf-ref.ts
// on sampled feature rows, and the compacted research/grobid models against the original
// GROBID models (compaction must not change a label). Originals are read from
// $GROBID_ORIG_DIR (refseg/seg/cit/name.wapiti); that part is skipped when they are missing.
import { existsSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone, tmpPath } from "./util";
import { loadRefModel, refTagText, type RefModel } from "./grobid-crf-ref";

const root = resolve(import.meta.dir, "..");
const kOrigDir =
  process.env.GROBID_ORIG_DIR ??
  "C:/Users/Tyzen/AppData/Local/Temp/claude/E--Work-SumatraPdf/f87920bb-373a-40ca-b355-7383190142ae/scratchpad/grobid-src";
const kModels: { name: string; orig: string; maxLen: number }[] = [
  { name: "reference-segmenter", orig: "refseg", maxLen: 120 },
  { name: "segmentation", orig: "seg", maxLen: 200 },
  { name: "citation", orig: "cit", maxLen: 60 },
  { name: "name-citation", orig: "name", maxLen: 12 },
];
const kSeqsPerModel = 40;

function mulberry32(seed: number): () => number {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Feature rows whose values come from the model's own observations, so features fire.
export function sampleFeatures(m: RefModel, seed: number, maxLen: number): string {
  const rnd = mulberry32(seed);
  const pick = <T>(a: T[]): T => a[Math.floor(rnd() * a.length)]!;
  const colsOfName = new Map<string, number[]>();
  let maxCol = 0;
  for (const p of m.patterns) {
    const xs = p.items.filter((it) => !("lit" in it)) as { col: number }[];
    for (const x of xs) maxCol = Math.max(maxCol, x.col);
    const first = p.items[0];
    if (xs.length !== 1 || !first || !("lit" in first)) continue;
    colsOfName.set(first.lit, [...(colsOfName.get(first.lit) ?? []), xs[0]!.col]);
  }
  const pools: string[][] = Array.from({ length: maxCol + 1 }, () => []);
  for (const key of m.obs.keys()) {
    const colon = key.indexOf(":");
    const cols = colsOfName.get(key.slice(0, colon + 1));
    const v = key.slice(colon + 1);
    if (!cols || v.startsWith("_x") || v === "") continue;
    for (const c of cols) pools[c]!.push(v);
  }
  const seqs: string[] = [];
  for (let s = 0; s < kSeqsPerModel; s++) {
    const n = s === 0 ? 1 : 1 + Math.floor(rnd() * maxLen);
    const rows: string[] = [];
    for (let t = 0; t < n; t++) {
      const cols = pools.map((pool, c) =>
        pool.length && rnd() < 0.85 ? pick(pool) : c === 0 ? `zz${Math.floor(rnd() * 1000)}` : pick(["0", "1"]),
      );
      rows.push([...cols, "0"].join(" "));
    }
    seqs.push(rows.join("\n"));
  }
  // a row missing columns makes Wapiti reject the sequence
  seqs.push("a 1\nb 2");
  return seqs.join("\n\n") + "\n";
}

export async function testit(): Promise<void> {
  const failures: string[] = [];
  const cases: { name: string; features: string; file: string; ref: string; origPath: string }[] = [];
  for (const [i, md] of kModels.entries()) {
    const ref = loadRefModel(join(root, "research", "grobid", `${md.name}.wapiti`));
    const features = sampleFeatures(ref, 1234 + i, md.maxLen);
    const file = tmpPath(`grobid-crf-${md.name}.txt`);
    writeFileSync(file, features, "latin1");
    const expected = refTagText(ref, features);
    const nLabels = new Set(expected.split("\n").filter((l) => l && l !== "#fail")).size;
    console.log(`${md.name}: ${features.split("\n").filter((l) => l).length} rows, ${nLabels} distinct labels`);
    if (nLabels < 2) failures.push(`${md.name}: sampled features hit only ${nLabels} label(s)`);
    cases.push({ name: md.name, features, file, ref: expected, origPath: join(kOrigDir, `${md.orig}.wapiti`) });
  }

  await withControlledSumatra(EXE, async (client) => {
    const crf = async (model: string, file: string) =>
      String((await client.request(ControlCommand.TestResearchTools, ["grobid-crf", model, file]))[1]);
    for (const c of cases) {
      const got = await crf(c.name, c.file);
      if (got.trim() !== c.ref.trim()) failures.push(`${c.name}: C++ differs from reference\n${firstDiff(got, c.ref)}`);
      const byPath = await crf(join(root, "research", "grobid", `${c.name}.wapiti`), c.file);
      if (byPath.trim() !== c.ref.trim())
        failures.push(`${c.name}: model by path differs\n${firstDiff(byPath, c.ref)}`);
      if (!existsSync(c.origPath)) continue;
      const t0 = Date.now();
      const orig = await crf(c.origPath, c.file);
      console.log(`${c.name}: C++ on original model ${Date.now() - t0} ms`);
      if (orig.trim() !== c.ref.trim())
        failures.push(`${c.name}: C++ on original model differs\n${firstDiff(orig, c.ref)}`);
    }
  });

  // compaction is lossless: the reference gives the same labels with the original model
  for (const c of cases) {
    if (!existsSync(c.origPath)) {
      console.log(`skip ${c.name}: no original model at ${c.origPath}`);
      continue;
    }
    const t0 = Date.now();
    const orig = refTagText(loadRefModel(c.origPath), c.features);
    console.log(`${c.name}: reference on original model ${Date.now() - t0} ms`);
    if (orig !== c.ref) failures.push(`${c.name}: original model differs from compacted\n${firstDiff(orig, c.ref)}`);
  }
  if (failures.length) throw new Error(failures.join("\n"));
}

function firstDiff(a: string, b: string): string {
  const la = a.trim().split("\n"),
    lb = b.trim().split("\n");
  for (let i = 0; i < Math.max(la.length, lb.length); i++) {
    if (la[i] !== lb[i]) return `  line ${i + 1}: got '${la[i]}' want '${lb[i]}' (${la.length} vs ${lb.length} lines)`;
  }
  return "  (equal after trim)";
}

if (import.meta.main) {
  await runStandalone(testit);
}
