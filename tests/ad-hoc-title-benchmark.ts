import { mkdirSync, readFileSync, writeFileSync, existsSync } from "node:fs";
import { join, resolve } from "node:path";
import { runStandalone } from "./util";

const ROOT = resolve("research/title-benchmark-2026-10-11");
const SEED = 20261011;
const PAPER_COUNT = 10;
const REFERENCES_PER_PAPER = 3;
const TIMEOUT_MS = 30000;
const METADATA_INTERVAL_MS = 1250;
type Reference = { key: string; label: string; raw: string; html: string; links: string[] };
type Paper = { id: string; title: string; references: Reference[]; count: number };

function rng(seed: number) {
  return () => {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    return seed / 4294967296;
  };
}
function shuffle<T>(items: T[], random: () => number) {
  const out = [...items];
  for (let i = out.length - 1; i > 0; i--) {
    const j = Math.floor(random() * (i + 1));
    [out[i], out[j]] = [out[j]!, out[i]!];
  }
  return out;
}
function decode(s: string): string {
  return s
    .replace(/&#(x[\da-f]+|\d+);/gi, (_, n) =>
      String.fromCodePoint(n[0] === "x" ? parseInt(n.slice(1), 16) : Number(n)),
    )
    .replace(
      /&(?:amp|lt|gt|quot|apos|nbsp);/g,
      (s) => ({ "&amp;": "&", "&lt;": "<", "&gt;": ">", "&quot;": '"', "&apos;": "'", "&nbsp;": " " })[s]!,
    );
}
function text(s: string): string {
  return decode(s.replace(/<math\b[^>]*alttext="([^"]*)"[^>]*>[\s\S]*?<\/math>/g, "$1").replace(/<[^>]+>/g, " "))
    .replace(/\s+/g, " ")
    .trim();
}
function clean(s: string): string {
  return s.replace(/^[\s,.;:]+|[\s,.;:]+$/g, "");
}
function parts(raw: string, separator: string): string[] {
  if (!raw.includes(separator)) return [];
  return raw
    .split(separator === "." ? /\.(?=\s|$)/ : /,/)
    .map(clean)
    .filter((s) => s.length >= 8 && /\p{L}/u.test(s) && !/10\.\d{4,9}\//.test(s));
}
function ranked(raw: string): string[] {
  const rank = (s: string) =>
    parts(raw, s)
      .sort((a, b) => Buffer.byteLength(b) - Buffer.byteLength(a))
      .slice(0, 2);
  return [...new Set([...rank(","), ...rank(".")])];
}
const JOURNAL =
  /\b(?:journal|annals|ann\.|proc\.|proceedings|transactions|trans\.|springer|press|lecture notes|vol\.|volume|arxiv|preprint|university|cambridge|oxford|mathscinet|https?:|doi:|MR\d|pp\.)\b/i;
function candidates(raw: string): string[] {
  const out = [...ranked(raw)];
  const delimiters = [...raw.matchAll(/(?:\.\s+|,\s+|:\s+|;\s+)/g)].map((m) => m.index! + m[0].length);
  for (const start of [0, ...delimiters]) {
    for (const end of [...delimiters.map((x) => x - 2), raw.length]) {
      if (end <= start) continue;
      const s = clean(raw.slice(start, end));
      if (s.length >= 8 && s.length <= 220 && /\p{L}/u.test(s)) out.push(s);
    }
  }
  return [...new Set(out)];
}
function authorLike(s: string): boolean {
  return /(?:\b[A-Z]\.\s*){1,3}[\p{L}-]+/u.test(s) || /[\p{L}-]+,\s*(?:[A-Z]\.\s*){1,3}/u.test(s);
}
export function structure(raw: string): string {
  const isNames = (s: string) => {
    const ws = s.match(/[\p{L}]+/gu) || [];
    return ws.length >= 2 && ws.every((w) => /^\p{Lu}/u.test(w) || /^(?:and|de|del|van|von|et|al|d)$/i.test(w));
  };
  const protectedRaw = raw.replace(/\b(\p{Lu})\.(?=[\s\p{Lu}-])/gu, "$1§");
  let rest = protectedRaw;
  const dated = rest.match(/^(.{3,180}?)\s+\((?:18|19|20)\d{2}[a-z]?\)\.?\s+(.+)$/);
  if (dated && isNames(dated[1]!)) {
    rest = dated[2]!;
  } else {
    const commas = [...rest.matchAll(/,\s+/g)];
    const boundary = commas.find((m) => {
      const prefix = rest.slice(0, m.index);
      const next = rest.slice(m.index! + m[0].length).split(/,\s+/)[0]!;
      return isNames(prefix) && !isNames(next) && !/^[A-Z]§/.test(next);
    });
    const colon = rest.match(/^(.{3,180}?):\s+(.+)$/);
    const dot = rest.match(/^(.{3,180}?)\.\s+(.+)$/);
    if (boundary) rest = rest.slice(boundary.index! + boundary[0].length);
    else if (colon && isNames(colon[1]!)) rest = colon[2]!;
    else if (dot && isNames(dot[1]!)) rest = dot[2]!;
  }
  const venue =
    /(?:,\s+)(?=(?:J\.|Math\.|Ann\.|Proc\.|Journal|Trans\.|Springer|Cambridge|American|Amer\.|Adv\.|SIAM|arXiv|Arch\.|Bull\.|Canadian|Russian|Princeton|Oxford|EMS\b|vol\b|\d))/i;
  const comma = rest.search(venue);
  const sentence = rest.search(/\.\s+(?![IVX]+(?:\s*\.|\s*$))/);
  const boundaries = [comma, sentence].filter((n) => n >= 0);
  const cut = boundaries.length ? Math.min(...boundaries) : rest.length;
  return clean(rest.slice(0, cut).replace(/§/g, "."));
}
export function scored(raw: string): string {
  const cs = candidates(raw);
  function score(s: string) {
    const words = s.match(/\p{L}+/gu) || [];
    let value = Math.min(words.length, 15) * 2;
    value += /\b(?:of|on|for|in|and|with|via|the|a|an|sur|de|des|du|and|to)\b/i.test(s) ? 8 : 0;
    value +=
      /\b(?:geometry|algebra|theorem|convergence|equations|optimization|manifolds|functions|groups|spaces|algorithm|metric|analysis|problem|curvature|methods|bounds|proof)\b/i.test(
        s,
      )
        ? 7
        : 0;
    value -= JOURNAL.test(s) ? 35 : 0;
    value -= authorLike(s) ? 25 : 0;
    value -= /\b(?:18|19|20)\d{2}\b/.test(s) ? 25 : 0;
    value -= /\b\d+\s*[–-]\s*\d+\b/.test(s) ? 15 : 0;
    value -= /10\.\d{4,9}\/|https?:/.test(s) ? 40 : 0;
    value -= (s.match(/[,:;]/g) || []).length * 3;
    return value;
  }
  return cs.sort((a, b) => score(b) - score(a) || a.length - b.length)[0] || "";
}
async function request(url: string, name: string): Promise<{ status: number; text: string; url: string }> {
  const file = join(ROOT, "cache", name + ".json");
  if (existsSync(file)) {
    const cached = JSON.parse(readFileSync(file, "utf8"));
    if (cached.status !== 429) return cached;
  }
  if (url.includes("api.crossref.org")) await Bun.sleep(METADATA_INTERVAL_MS);
  let result;
  try {
    const r = await fetch(url, {
      signal: AbortSignal.timeout(TIMEOUT_MS),
      headers: { "User-Agent": "SumatraPDF-reference-evaluation/1.0" },
    });
    result = { status: r.status, text: await r.text(), url };
  } catch (e) {
    result = { status: 0, text: String(e), url };
  }
  writeFileSync(file, JSON.stringify(result, null, 2));
  return result;
}
export async function testit(): Promise<void> {
  mkdirSync(join(ROOT, "cache"), { recursive: true });
  const random = rng(SEED);
  const poolFile = join(ROOT, "pool.json");
  let pool: { id: string; title: string }[];
  if (existsSync(poolFile)) {
    pool = JSON.parse(readFileSync(poolFile, "utf8"));
  } else {
    const response = await request(
      "https://export.arxiv.org/api/query?search_query=cat:math.OC+OR+cat:math.AG+OR+cat:math.NT+OR+cat:math.DG&start=0&max_results=100&sortBy=submittedDate&sortOrder=descending",
      "arxiv-pool",
    );
    if (response.status !== 200) throw new Error("Cannot fetch arXiv sampling pool: HTTP " + response.status);
    pool = shuffle(
      [...response.text.matchAll(/<entry>([\s\S]*?)<\/entry>/g)]
        .map((m) => ({
          id: m[1]!.match(/<id>[^<]*\/abs\/([^<]+)<\/id>/)?.[1] || "",
          title: text(m[1]!.match(/<title>([\s\S]*?)<\/title>/)?.[1] || ""),
        }))
        .filter((p) => p.id && p.title),
      rng(SEED),
    );
  }
  writeFileSync(join(ROOT, "pool.json"), JSON.stringify(pool, null, 2));
  const papers: Paper[] = [];
  const skipped: unknown[] = [];
  for (const paper of pool) {
    if (papers.length === PAPER_COUNT) break;
    const response = await request("https://arxiv.org/html/" + paper.id, paper.id);
    const refs = [
      ...response.text.matchAll(/<li\b[^>]*id="([^"]+)"[^>]*class="[^"]*ltx_bibitem[^"]*"[^>]*>([\s\S]*?)<\/li>/g),
    ]
      .map((m) => {
        const label = text(m[2]!.match(/<span[^>]*ltx_tag_bibitem[^>]*>([\s\S]*?)<\/span>/)?.[1] || m[1]!);
        const html = m[2]!.replace(/<span[^>]*ltx_tag_bibitem[^>]*>[\s\S]*?<\/span>/, "");
        return {
          key: paper.id + "-" + m[1],
          label,
          raw: text(html),
          html,
          links: [...html.matchAll(/href="([^"]+)"/g)].map((x) => decode(x[1]!)),
        };
      })
      .filter((r) => r.raw.length > 20);
    if (response.status !== 200 || refs.length < REFERENCES_PER_PAPER) {
      skipped.push({ id: paper.id, status: response.status, references: refs.length });
      continue;
    }
    papers.push({ ...paper, count: refs.length, references: shuffle(refs, random).slice(0, REFERENCES_PER_PAPER) });
    console.log(
      `SAMPLE ${paper.id}: ${refs.length} references; ${papers
        .at(-1)!
        .references.map((r) => r.label)
        .join(", ")}`,
    );
  }
  writeFileSync(
    join(ROOT, "sample.json"),
    JSON.stringify(
      {
        seed: SEED,
        papers,
        skipped,
        input: "arXiv HTML bibliography flattened to plain text; not PDF extraction",
        pool: "100 most recent math.OC OR math.AG OR math.NT OR math.DG, shuffled",
      },
      null,
      2,
    ),
  );
  if (papers.length !== PAPER_COUNT) throw new Error("Not enough eligible papers");
  const results = [];
  for (const paper of papers)
    for (const ref of paper.references) {
      const started = performance.now();
      const local = {
        delimiter: { title: ranked(ref.raw)[0] || ref.raw, candidates: ranked(ref.raw) },
        structure: { title: structure(ref.raw) },
        scoring: { title: scored(ref.raw) },
      };
      const localMs = performance.now() - started;
      const response = await request(
        "https://api.crossref.org/works?rows=5&query.bibliographic=" + encodeURIComponent(ref.raw),
        ref.key + "-crossref",
      );
      const metadata = response.status === 200 ? JSON.parse(response.text).message.items : [];
      results.push({
        paper: paper.id,
        ...ref,
        local,
        localMs,
        metadataStatus: response.status,
        metadata: metadata.map((m: any) => ({
          title: m.title?.[0] || "",
          doi: m.DOI,
          score: m.score,
          authors: m.author,
          year: m.issued?.["date-parts"]?.[0]?.[0],
          url: m.URL,
        })),
      });
      writeFileSync(join(ROOT, "predictions.json"), JSON.stringify(results, null, 2));
      console.log(`PREDICT ${ref.key}; Crossref HTTP ${response.status}; ${metadata[0]?.title?.[0] || "no candidate"}`);
    }
}
if (import.meta.main) await runStandalone(testit);
