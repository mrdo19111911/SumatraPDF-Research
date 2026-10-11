// Generate research/grobid/: GROBID's CRF models and lexicons, compacted for the
// native port of its paper segmentation and reference parsing (src/Grobid*.cpp).
//
//   bun cmd/gen-grobid-data.ts                 # sparse-clones GROBID into .work/grobid
//   bun cmd/gen-grobid-data.ts -grobid <dir>   # use an existing checkout
//
// Wapiti models are trained with L1, so almost all weights are 0. Dropping the
// observations whose weights are all 0 is lossless: tagging gives the same labels.
//
//   segmentation  62 MB -> ~0.7 MB     reference-segmenter  12 MB -> ~60 KB
//
// Citation gazetteers (lex-*.txt) keep FastMatcher's token form, drop terms their own
// sub-terms already cover and are front-coded:   location.txt  10.9 MB -> ~3.2 MB

import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const kGrobidUrl = "https://github.com/kermitt2/grobid.git";
const kGrobidCommit = "c71ebff";
const root = resolve(import.meta.dir, "..");
const outDir = join(root, "research", "grobid");

// name in research/grobid -> path under grobid-home/models
const kModels: [string, string][] = [
  ["segmentation", "segmentation"],
  ["reference-segmenter", "reference-segmenter"],
  ["citation", "citation"],
  ["name-citation", "name/citation"],
  ["header", "header"],
  ["name-header", "name/header"],
  ["fulltext", "fulltext"],
];

const kFirstNames = ["names.female", "names.male", "firstname.5k"];
const kLastNames = ["names.family", "lastname.5k"];
// FastMatcher gazetteers of the citation and name models (Lexicon.initJournals() etc.)
// research/grobid name -> path under grobid-home/lexicon
const kMatchers: [string, string][] = [
  ["lex-journals.txt", "journals/journals.txt"],
  ["lex-abbrev-journals.txt", "journals/abbrev_journals.txt"],
  ["lex-conferences.txt", "journals/proceedings.txt"],
  ["lex-publishers.txt", "publishers/publishers.txt"],
  ["lex-locations.txt", "places/location.txt"],
  ["lex-collaborations.txt", "organisations/inspire_collaborations.txt"],
  ["lex-person-titles.txt", "names/VincentNgPeopleTitles.txt"],
  ["lex-person-suffixes.txt", "names/suffix.txt"],
];

const kLexicons = [
  "grobid-home/lexicon/wordforms/english.wf",
  ...[...kFirstNames, ...kLastNames].map((f) => `grobid-home/lexicon/names/${f}`),
  ...kMatchers.map(([, p]) => `grobid-home/lexicon/${p}`),
];

function run(cmd: string[], cwd?: string): string {
  const r = Bun.spawnSync(cmd, { cwd, stdout: "pipe", stderr: "pipe" });
  if (r.exitCode !== 0) {
    console.error(`failed: ${cmd.join(" ")}\n${r.stderr.toString()}`);
    process.exit(1);
  }
  return r.stdout.toString().trim();
}

function cloneGrobid(dir: string) {
  if (!existsSync(dir)) {
    run(["git", "clone", "-q", "--filter=blob:none", "--no-checkout", kGrobidUrl, dir]);
  }
  const paths = ["LICENSE", ...kModels.map(([, p]) => `grobid-home/models/${p}/model.wapiti`), ...kLexicons];
  run(["git", "-c", "core.autocrlf=false", "checkout", "-q", kGrobidCommit, "--", ...paths], dir);
}

// Wapiti strings are "<byte len>:<bytes>,"
class Reader {
  pos = 0;
  constructor(public buf: Buffer) {}
  line(): string {
    const end = this.buf.indexOf(10, this.pos);
    const s = this.buf.toString("latin1", this.pos, end);
    this.pos = end + 1;
    return s;
  }
  str(): Buffer {
    const colon = this.buf.indexOf(58, this.pos);
    const n = parseInt(this.buf.toString("latin1", this.pos, colon));
    const s = this.buf.subarray(colon + 1, colon + 1 + n);
    this.pos = colon + 1 + n + 2; // ",\n"
    return s;
  }
}

function writeStr(out: Buffer[], s: Buffer) {
  out.push(Buffer.from(`${s.length}:`), s, Buffer.from(",\n"));
}

// Keeps the observations with a non-zero weight and renumbers the weights.
function compactModel(src: Buffer): { data: Buffer; obs: number; kept: number } {
  const r = new Reader(src);
  const mdl = r.line();
  const rdr = r.line();
  const npats = parseInt(rdr.split("#")[2].split("/")[0]);
  const pats: Buffer[] = [];
  for (let i = 0; i < npats; i++) pats.push(r.str());
  const nlbl = parseInt(r.line().split("#")[2]);
  const lbls: Buffer[] = [];
  for (let i = 0; i < nlbl; i++) lbls.push(r.str());
  const nobs = parseInt(r.line().split("#")[2]);
  const obs: Buffer[] = [];
  for (let i = 0; i < nobs; i++) obs.push(r.str());

  const weights = new Map<number, string>();
  for (const l of src.toString("latin1", r.pos).split("\n")) {
    const eq = l.indexOf("=");
    if (eq > 0) weights.set(parseInt(l.slice(0, eq)), l.slice(eq + 1));
  }

  // feature offsets as in Wapiti's mdl_sync()
  const Y = nlbl;
  const size = (o: Buffer) => (o[0] === 0x75 ? Y : o[0] === 0x62 ? Y * Y : Y + Y * Y); // u, b, *
  const off: number[] = [];
  let F = 0;
  for (const o of obs) {
    off.push(F);
    F += size(o);
  }

  const keptObs: number[] = [];
  for (let i = 0; i < nobs; i++) {
    for (let f = off[i]; f < off[i] + size(obs[i]); f++) {
      if (weights.has(f)) {
        keptObs.push(i);
        break;
      }
    }
  }

  const out: Buffer[] = [];
  out.push(Buffer.from(`${mdl.split("#").slice(0, 3).join("#")}#${weights.size}\n${rdr}\n`));
  for (const p of pats) writeStr(out, p);
  out.push(Buffer.from(`#qrk#${nlbl}\n`));
  for (const l of lbls) writeStr(out, l);
  out.push(Buffer.from(`#qrk#${keptObs.length}\n`));
  for (const i of keptObs) writeStr(out, obs[i]);
  let newOff = 0;
  const lines: string[] = [];
  for (const i of keptObs) {
    const n = size(obs[i]);
    for (let d = 0; d < n; d++) {
      const w = weights.get(off[i] + d);
      if (w !== undefined) lines.push(`${newOff + d}=${w}`);
    }
    newOff += n;
  }
  out.push(Buffer.from(lines.join("\n") + "\n"));
  return { data: Buffer.concat(out), obs: nobs, kept: keptObs.length };
}

// Lexicon.addDictionary(EN): first tab-separated column of the multext word forms.
function englishWords(path: string): string[] {
  const words = new Set<string>();
  for (const l of readFileSync(path, "utf8").split("\n")) {
    if (l.trim() === "") continue;
    const w = l.split("\t")[0];
    if (w) words.add(w);
  }
  return [...words].sort();
}

// Lexicon.addFirstNames / addLastNames: first token split on tab and '-', lowercased.
function names(paths: string[]): string[] {
  const res = new Set<string>();
  for (const p of paths) {
    for (const l of readFileSync(p, "utf8").split("\n")) {
      const w = l
        .split(/[\t\n-]/)
        .find((t) => t !== "")
        ?.toLowerCase()
        .trim();
      if (w) res.add(w);
    }
  }
  return [...res].sort();
}

// TextUtilities.delimiters: GrobidDefaultAnalyzer splits on these and keeps them as tokens
const kDelimiters =
  "\n\r\t\f\u00A0\u200C" +
  "(\uFF08[ \u2022*,:;?.!/)\uFF09-\u2212\u2013\u2010\u00AB\u00BB\u201E\"\u201C\u201D\u2018\u2019'`$#@]*" +
  "\u2666\u2665\u2663\u2660\u00A0\u3002\u3001\uFF0C\u30FB";

// UnicodeUtil.normaliseText()
function normaliseText(s: string): string {
  const word =
    "(?:(?![\\p{Script=Han}\\p{Script=Hiragana}\\p{Script=Katakana}\\p{Script=Hangul}])[\\p{L}\\p{N}\\p{M}])";
  return s
    .replace(/[\t \u00A0\u1680\u180E\u2000-\u200A\u2028\u2029\u202F\u205F\u3000]/gu, " ")
    .replaceAll("\r\n", "\n")
    .replace(/[\f\n\r\v\u0085\u2029\u2028]/gu, "\n")
    .replace(/[\p{Pd}\u2212]/gu, "-")
    .replace(/[\u005F\u203F\u2040\u2054\uFE4D\uFE4E\uFE4F\uFF3F\uFE33\uFE34]/gu, "_")
    .replace(/[\u007C\u01C0\u05C0\u2223\u2758]/gu, "|")
    .replace(/[\u2022\u2023\u25E6\u2043\u204C\u204D\u2219\u25D8\u29BE\u29BF\u23FA\u25CF\u26AB\u2B24]/gu, "\u2022")
    .replace(new RegExp(`(?<!${word})\\u00B7|\\u00B7(?!${word})`, "gu"), "\u2022")
    .replace(/[\u0028\uFF08\u27EE\u2985\u2768\u276A\u27EC]/gu, "(")
    .replace(/[\u0029\uFF09\u27EF\u2986\u2769\u276B\u27ED]/gu, ")");
}

// FastMatcher.loadTerms(): normalise, lowercase, tokenize on delimiters, drop the
// delimiters. Terms are token lists joined by a space.
function matcherTerms(path: string): Set<string> {
  const terms = new Set<string>();
  for (const line of readFileSync(path, "utf8").split(/\r\n|\r|\n/)) {
    if (line.length === 0) continue;
    const text = normaliseText(line).trim().toLowerCase();
    const toks: string[] = [];
    let cur = "";
    for (const c of text) {
      if (!kDelimiters.includes(c)) {
        cur += c;
        continue;
      }
      if (cur) toks.push(cur);
      cur = "";
    }
    if (cur) toks.push(cur);
    if (toks.length > 0) terms.add(toks.join(" "));
  }
  return terms;
}

// A term whose tokens are all covered by its own shorter sub-terms (e.g. "new york"
// next to "new" and "york") marks no token they don't, so dropping it is lossless.
function isCoveredTerm(term: string, terms: Set<string>): boolean {
  const toks = term.split(" ");
  const n = toks.length;
  const covered = new Array(n).fill(false);
  for (let i = 0; i < n; i++) {
    for (let j = i + 1; j <= n; j++) {
      if (j - i === n || !terms.has(toks.slice(i, j).join(" "))) continue;
      for (let k = i; k < j; k++) covered[k] = true;
    }
  }
  return n > 1 && covered.every((c) => c);
}

// Sorted by UTF-8 bytes (the C++ matcher binary-searches token prefixes) and
// front-coded: each line is chr(33 + bytes shared with the previous term) + the rest.
const kMaxShared = 93;
function encodeMatcher(terms: Set<string>): Buffer {
  const sorted = [...terms]
    .filter((t) => !isCoveredTerm(t, terms))
    .map((t) => Buffer.from(t))
    .sort(Buffer.compare);
  const out: Buffer[] = [];
  let prev = Buffer.alloc(0);
  for (const t of sorted) {
    let n = 0;
    while (n < kMaxShared && n < prev.length && n < t.length && prev[n] === t[n]) n++;
    out.push(Buffer.from([33 + n]), t.subarray(n), Buffer.from("\n"));
    prev = t;
  }
  return Buffer.concat(out);
}

function main() {
  let grobidDir = join(root, ".work", "grobid");
  const args = process.argv.slice(2);
  for (let i = 0; i < args.length; i++) {
    if (args[i] === "-grobid") grobidDir = resolve(args[++i]);
  }
  cloneGrobid(grobidDir);
  const home = join(grobidDir, "grobid-home");
  mkdirSync(outDir, { recursive: true });

  for (const [name, path] of kModels) {
    const src = readFileSync(join(home, "models", path, "model.wapiti"));
    const { data, obs, kept } = compactModel(src);
    writeFileSync(join(outDir, `${name}.wapiti`), data);
    console.log(`${name}: ${src.length} -> ${data.length} bytes, ${kept} of ${obs} observations`);
  }

  const lex = join(home, "lexicon");
  const lists: [string, string[]][] = [
    ["words-en.txt", englishWords(join(lex, "wordforms", "english.wf"))],
    ["names-first.txt", names(kFirstNames.map((f) => join(lex, "names", f)))],
    ["names-last.txt", names(kLastNames.map((f) => join(lex, "names", f)))],
  ];
  for (const [file, words] of lists) {
    writeFileSync(join(outDir, file), words.join("\n") + "\n");
    console.log(`${file}: ${words.length} entries`);
  }

  for (const [file, path] of kMatchers) {
    const terms = matcherTerms(join(lex, path));
    const data = encodeMatcher(terms);
    writeFileSync(join(outDir, file), data);
    console.log(`${file}: ${terms.size} terms, ${data.length} bytes`);
  }

  const license = readFileSync(join(grobidDir, "LICENSE"), "utf8");
  writeFileSync(join(outDir, "LICENSE.txt"), license);
  writeFileSync(
    join(outDir, "VERSION.txt"),
    `GROBID ${kGrobidUrl} commit ${kGrobidCommit}\nGenerated by cmd/gen-grobid-data.ts; do not edit.\n`,
  );
}

main();
