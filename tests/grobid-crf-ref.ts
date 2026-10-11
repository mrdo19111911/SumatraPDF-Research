// Independent reference of Wapiti's label mode (model.c mdl_load, reader.c rdr_pattok2seq,
// pattern.c pat_exec, decoder.c tag_expsc + tag_viterbi), used to check src/CrfModel.cpp.
// Strings are handled as latin1 "binary" strings so byte lengths match Wapiti's.
import { readFileSync } from "node:fs";

type PatItem = { lit: string } | { off: number; col: number; abs: boolean; caps: boolean };

export interface RefModel {
  labels: string[];
  patterns: { src: string; items: PatItem[] }[];
  // observation -> weights; only observations with at least one listed weight are kept,
  // others weigh 0 everywhere and cannot change a score
  obs: Map<string, { uni?: Float64Array; bi?: Float64Array }>;
}

class Reader {
  pos = 0;
  constructor(public buf: Buffer) {}
  line(): string {
    const end = this.buf.indexOf(10, this.pos);
    const s = this.buf.toString("latin1", this.pos, end < 0 ? this.buf.length : end);
    this.pos = end < 0 ? this.buf.length : end + 1;
    return s;
  }
  // netstring "<len>:<bytes>,\n" -> [start, len] in buf
  netstr(): [number, number] {
    while (this.buf[this.pos] === 10 || this.buf[this.pos] === 32) this.pos++;
    const colon = this.buf.indexOf(58, this.pos);
    const n = parseInt(this.buf.toString("latin1", this.pos, colon), 10);
    const start = colon + 1;
    if (this.buf[start + n] !== 44) throw new Error(`bad netstring at ${this.pos}`);
    this.pos = start + n + 2;
    return [start, n];
  }
}

// C99 "%a" hex float, e.g. -0x1.b106a6e28d32dp-9
export function parseHexFloat(s: string): number {
  const m = /^([+-]?)0x([0-9a-f]*)(?:\.([0-9a-f]*))?p([+-]?\d+)$/i.exec(s.trim());
  if (!m) return parseFloat(s);
  const digits = (m[2] ?? "") + (m[3] ?? "");
  const mant = Number(BigInt("0x" + (digits || "0")));
  const exp = parseInt(m[4]!, 10) - 4 * (m[3] ?? "").length;
  const v = mant * Math.pow(2, exp);
  return m[1] === "-" ? -v : v;
}

function compilePattern(src: string): PatItem[] {
  const items: PatItem[] = [];
  let pos = 0;
  while (pos < src.length) {
    if (src[pos] !== "%") {
      const next = src.indexOf("%", pos);
      const end = next < 0 ? src.length : next;
      items.push({ lit: src.slice(pos, end) });
      pos = end;
      continue;
    }
    const type = src[pos + 1]!;
    if (type.toLowerCase() !== "x") throw new Error(`unsupported pattern command in ${src}`);
    const m = /^\[(@?)([+-]?\d+),(\d+)\]/.exec(src.slice(pos + 2));
    if (!m) throw new Error(`invalid pattern ${src}`);
    items.push({ off: parseInt(m[2]!, 10), col: parseInt(m[3]!, 10), abs: m[1] === "@", caps: type === "X" });
    pos += 2 + m[0].length;
  }
  return items;
}

export function loadRefModel(path: string): RefModel {
  const r = new Reader(readFileSync(path));
  const mdl = /^#mdl#(\d+)#(\d+)$/.exec(r.line());
  if (!mdl) throw new Error("bad #mdl# header");
  const nact = parseInt(mdl[2]!, 10);
  const rdr = /^#rdr#(\d+)\/(\d+)(?:\/(\d+))?$/.exec(r.line());
  if (!rdr) throw new Error("bad #rdr# header");
  if (rdr[3] && rdr[3] !== "0") throw new Error("autouni models are not supported");
  const patterns: RefModel["patterns"] = [];
  for (let i = 0, n = parseInt(rdr[1]!, 10); i < n; i++) {
    const [s, l] = r.netstr();
    const src = r.buf.toString("latin1", s, s + l);
    patterns.push({ src, items: compilePattern(src) });
  }
  const qrk = () => {
    const m = /^#qrk#(\d+)$/.exec(r.line());
    if (!m) throw new Error("bad #qrk# header");
    return parseInt(m[1]!, 10);
  };
  const labels: string[] = [];
  for (let i = 0, n = qrk(); i < n; i++) {
    const [s, l] = r.netstr();
    labels.push(r.buf.toString("latin1", s, s + l));
  }
  const Y = labels.length;

  // mdl_sync: features are laid out per observation, Y unigram weights then Y*Y bigram weights
  const nobs = qrk();
  const obsStart = new Int32Array(nobs);
  const obsLen = new Int32Array(nobs);
  const featStart = new Float64Array(nobs + 1);
  let F = 0;
  for (let o = 0; o < nobs; o++) {
    const [s, l] = r.netstr();
    obsStart[o] = s;
    obsLen[o] = l;
    featStart[o] = F;
    const kind = r.buf[s] === 0x75 ? 1 : r.buf[s] === 0x62 ? 2 : r.buf[s] === 0x2a ? 3 : 0;
    if (kind & 1) F += Y;
    if (kind & 2) F += Y * Y;
  }
  featStart[nobs] = F;

  const obs: RefModel["obs"] = new Map();
  for (let i = 0; i < nact; i++) {
    const line = r.line();
    const eq = line.indexOf("=");
    const f = parseInt(line.slice(0, eq), 10);
    const v = parseHexFloat(line.slice(eq + 1));
    let lo = 0,
      hi = nobs - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (featStart[mid]! <= f) lo = mid;
      else hi = mid - 1;
    }
    const key = r.buf.toString("latin1", obsStart[lo]!, obsStart[lo]! + obsLen[lo]!);
    let e = obs.get(key);
    if (!e) obs.set(key, (e = {}));
    const kind = key[0] === "u" ? 1 : key[0] === "b" ? 2 : 3;
    let d = f - featStart[lo]!;
    if (kind & 1) {
      if (d < Y) {
        (e.uni ??= new Float64Array(Y))[d] = v;
        continue;
      }
      d -= Y;
    }
    (e.bi ??= new Float64Array(Y * Y))[d] = v;
  }
  return { labels, patterns, obs };
}

const kBefore = ["_x-1", "_x-2", "_x-3", "_x-4", "_x-#"];
const kAfter = ["_x+1", "_x+2", "_x+3", "_x+4", "_x+#"];

// rdr_raw2seq tokenization: skip leading whitespace, split on whitespace runs
function splitRow(row: string): string[] {
  return row
    .replace(/^[ \t\n\v\f\r]+/, "")
    .split(/[ \t\n\v\f\r]+/)
    .filter((t, i, a) => !(t === "" && i === a.length - 1));
}

// Labels (indexes) for one sequence, or null when a pattern needs a missing column.
export function refTag(m: RefModel, rows: string[]): number[] | null {
  const toks = rows.map(splitRow);
  const T = toks.length;
  const Y = m.labels.length;
  if (T === 0) return [];
  const uobs: Float64Array[][] = [];
  const bobs: Float64Array[][] = [];
  for (let t = 0; t < T; t++) {
    const u: Float64Array[] = [];
    const b: Float64Array[] = [];
    for (const p of m.patterns) {
      let s = "";
      for (const it of p.items) {
        if ("lit" in it) {
          s += it.lit;
          continue;
        }
        let pos = it.off;
        if (it.abs) pos = it.off < 0 ? pos + T : pos - 1;
        else pos += t;
        let v: string;
        if (pos < 0) v = kBefore[Math.min(-pos - 1, 4)]!;
        else if (pos >= T) v = kAfter[Math.min(pos - T, 4)]!;
        else if (it.col >= toks[pos]!.length) return null;
        else v = toks[pos]![it.col]!;
        if (it.caps) v = v.replace(/[A-Z]/g, (c) => c.toLowerCase());
        s += v;
      }
      const e = m.obs.get(s);
      if (!e) continue;
      const kind = s[0] === "u" ? 1 : s[0] === "b" ? 2 : s[0] === "*" ? 3 : 0;
      if (kind & 1) u.push(e.uni ?? new Float64Array(Y));
      if (kind & 2) b.push(e.bi ?? new Float64Array(Y * Y));
    }
    uobs.push(u);
    bobs.push(b);
  }

  // tag_expsc: psi[t][yp][y] = sum of unigram weights (+ bigram weights for t > 0)
  const psi = (t: number): Float64Array => {
    const res = new Float64Array(Y * Y);
    for (let y = 0; y < Y; y++) {
      let sum = 0.0;
      for (const w of uobs[t]!) sum += w[y]!;
      for (let yp = 0; yp < Y; yp++) res[yp * Y + y] = sum;
    }
    if (t === 0) return res;
    for (let d = 0; d < Y * Y; d++) {
      let sum = 0.0;
      for (const w of bobs[t]!) sum += w[d]!;
      res[d] += sum;
    }
    return res;
  };

  // tag_viterbi
  const back: Int32Array[] = [new Int32Array(Y)];
  const p0 = psi(0);
  let cur = new Float64Array(Y);
  for (let y = 0; y < Y; y++) cur[y] = p0[y]!;
  for (let t = 1; t < T; t++) {
    const old = cur;
    cur = new Float64Array(Y);
    const pt = psi(t);
    const bk = new Int32Array(Y);
    for (let y = 0; y < Y; y++) {
      let bst = -Infinity;
      let idx = 0;
      for (let yp = 0; yp < Y; yp++) {
        const val = old[yp]! + pt[yp * Y + y]!;
        if (val > bst) {
          bst = val;
          idx = yp;
        }
      }
      bk[y] = idx;
      cur[y] = bst;
    }
    back.push(bk);
  }
  let bst = 0;
  for (let y = 1; y < Y; y++) if (cur[y]! > cur[bst]!) bst = y;
  const out = new Array<number>(T);
  for (let t = T; t > 0; t--) {
    out[t - 1] = bst;
    bst = t !== 1 ? back[t - 1]![bst]! : 0;
  }
  return out;
}

// Same output format as the grobid-crf test action: one label per row, a blank line
// between sequences, "#fail" for a sequence Wapiti rejects.
export function refTagText(m: RefModel, features: string): string {
  const out: string[] = [];
  for (const seq of splitSequences(features)) {
    const labels = refTag(m, seq);
    out.push(labels ? labels.map((l) => m.labels[l]).join("\n") : "#fail");
  }
  return out.join("\n\n");
}

// rdr_readraw: sequences separated by (runs of) blank or whitespace-only lines
export function splitSequences(text: string): string[][] {
  const seqs: string[][] = [];
  let cur: string[] = [];
  for (const line of text.split("\n")) {
    const l = line.replace(/\r$/, "");
    if (/^[ \t\n\v\f\r]*$/.test(l)) {
      if (cur.length) seqs.push(cur);
      cur = [];
      continue;
    }
    cur.push(l);
  }
  if (cur.length) seqs.push(cur);
  return seqs;
}
