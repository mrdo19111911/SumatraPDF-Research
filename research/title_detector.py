"""Offline citation-title prototype. Keeps the original reference intact."""

import argparse
import html
import json
import re
import unicodedata
from pathlib import Path

MIN_TITLE_LENGTH = 21
LEGACY_MIN_TITLE_LENGTH = 8
RULE_AND = "and"
RULE_OR = "or"
RULE_VETO = "veto"
DOI = re.compile(r"10\.\d{4,9}/", re.IGNORECASE)
VENUE = re.compile(
    r"(?<!\w)(?:jour(?:nals?)?\.?|j\.|proc(?:e(?:ed(?:ings?)?)?)?\.?|conferences?\.?|conf\.?"
    r"|ann(?:als)?\.?|ieee|trans(?:actions)?\.?|vol(?:ume)?\.?)"
    r"(?=$|[\s.,;:()\[\]{}])",
    re.IGNORECASE,
)


def clean(text: str) -> str:
    return text.strip(" \t\r\n,.;:")


def title_parts(raw: str, separator: str, filter_venues: bool = True,
                min_title_length: int = MIN_TITLE_LENGTH,
                mode: str = RULE_VETO) -> list[str]:
    if mode not in (RULE_AND, RULE_OR, RULE_VETO):
        raise ValueError(f"Unknown rule mode: {mode}")
    if separator not in raw:
        return []
    pieces = re.split(r"\.(?=\s|$)" if separator == "." else ",", raw)
    result = []
    for item in pieces:
        piece = clean(item)
        if not any(c.isalpha() for c in piece) or DOI.search(piece):
            continue
        long_enough = len(piece) >= min_title_length
        venue_free = not (filter_venues and (separator == "," or mode == RULE_VETO) and VENUE.search(piece))
        allowed = (long_enough or venue_free) if mode == RULE_OR else (long_enough and venue_free)
        if allowed:
            result.append(piece)
    return result


def title_candidates(raw: str, filter_venues: bool = True,
                     min_title_length: int = MIN_TITLE_LENGTH,
                     mode: str = RULE_VETO) -> list[str]:
    result = []
    for separator in (",", "."):
        parts = title_parts(raw, separator, filter_venues, min_title_length, mode)
        for part in sorted(parts, key=lambda s: len(s.encode("utf-8")), reverse=True)[:2]:
            if part not in result:
                result.append(part)
    return result


def names_only(text: str) -> bool:
    words = re.findall(r"[^\W\d_]+", text, re.UNICODE)
    particles = {"and", "de", "del", "van", "von", "et", "al", "d"}
    return len(words) >= 2 and all(word[0].isupper() or word.lower() in particles for word in words)


def structured_title(raw: str, extended: bool = True, repaired: bool = True) -> str:
    # Protect initials; use author/title/venue boundaries rather than length alone.
    rest = re.sub(r"\b([A-Z])\.(?=[\sA-Z-])", r"\1§", raw)
    dated = re.match(r"^(.{3,180}?)\s+\((?:18|19|20)\d{2}[a-z]?\)\.?\s+(.+)$", rest)
    found = False
    if dated and names_only(dated[1]):
        rest = dated[2]
        found = True
    else:
        if repaired:
            for boundary in re.finditer(r":\s+|\.\s+", rest):
                prefix = rest[:boundary.start()]
                tail = clean(prefix.rsplit(",", 1)[-1])
                if rest[boundary.start()] == ":" and "," in prefix:
                    if not names_only(tail) and not re.fullmatch(r"(?:[A-Zd][.§]\s*)*[A-Zd][.§]?", tail):
                        continue
                if names_only(prefix):
                    rest = rest[boundary.end():]
                    found = True
                    break
        for boundary in re.finditer(r",\s+", rest):
            if found:
                break
            prefix = rest[:boundary.start()]
            next_piece = re.split(r",\s+", rest[boundary.end():])[0]
            if names_only(prefix) and not names_only(next_piece) and not re.match(r"^[A-Z]§", next_piece):
                rest = rest[boundary.end():]
                found = True
                break
        if not found:
            for separator in (r":\s+", r"\.\s+"):
                boundary = re.search(separator, rest)
                if boundary and names_only(rest[:boundary.start()]):
                    rest = rest[boundary.end():]
                    found = True
                    break
    if not found:
        return ""
    venue = r"J\.|Math\.|Ann\.|Proc\.|Journal|Trans\.|Springer|Cambridge|American|Amer\.|Adv\.|SIAM|arXiv|Arch\.|Bull\.|Canadian|Russian|Princeton|Oxford|EMS\b|vol\b|\d"
    if extended:
        venue += r"|Preprint|IEEE|In\s+Advances|Volume|Grad\.|Lecture\s+Notes|Annals|Proceedings|Transactions"
    if repaired:
        venue += r"|J§|[^,]{0,60}\bJ§"
        venue += r"|Ann\b|Annals\b|Trans\b|Transactions\b"
    boundaries = [m.start() for m in re.finditer(r"(?:,\s+)(?=(?:" + venue + r"))", rest, re.I)]
    sentence = re.search(r"\.\s+(?![IVX]+(?:\s*\.|\s*$))", rest)
    if sentence:
        boundaries.append(sentence.start())
    result = clean(rest[:min(boundaries)] if boundaries else rest).replace("§", ".")
    return result if any(c.isalpha() for c in result) else ""


def normalize(text: str) -> str:
    text = re.sub(r"<[^>]+>", " ", text)
    text = re.sub(r"\\operatorname\{([^}]+)\}", r"\1", text)
    text = re.sub(r"\\([A-Za-z]+)", r"\1", text)
    text = unicodedata.normalize("NFKD", text)
    return re.sub(r"[^a-z0-9]", "", text.lower())


def benchmark(folder: Path, output: Path) -> dict:
    sample = json.loads((folder / "sample.json").read_text(encoding="utf-8"))
    gold = {row["key"]: row for row in json.loads((folder / "gold.json").read_text(encoding="utf-8"))}
    rows = []
    for paper in sample["papers"]:
        for reference in paper["references"]:
            raw = reference["raw"]
            truth = gold[reference["key"]]
            expected = {normalize(title) for title in [truth["title"], *truth.get("aliases", [])]}
            versions = {}
            for name, mode in (("before", RULE_AND), ("after", RULE_OR)):
                candidates = title_candidates(raw, mode=mode)
                versions[name] = {
                    "title": candidates[0] if candidates else "",
                    "candidates": candidates,
                    "correct": bool(candidates and normalize(candidates[0]) in expected),
                    "candidate_hit": any(normalize(title) in expected for title in candidates),
                }
            rejected = []
            for separator in (",", "."):
                for piece in title_parts(raw, separator, filter_venues=False,
                                         min_title_length=LEGACY_MIN_TITLE_LENGTH):
                    reasons = []
                    if len(piece) < MIN_TITLE_LENGTH:
                        reasons.append(f"length {len(piece)} <= 20")
                    if separator == "," and (marker := VENUE.search(piece)):
                        reasons.append(f"venue marker: {marker.group()}")
                    if len(reasons) == 2:
                        rejected.append({"segment": piece, "separator": separator,
                                         "marker": "; ".join(reasons)})
            rows.append({
                "key": reference["key"], "paper": paper["id"], "label": reference["label"],
                "raw": raw, "gold": truth["title"], "rejected": rejected, **versions,
            })
    summary = {
        name: {
            "correct": sum(row[name]["correct"] for row in rows),
            "candidate_hit": sum(row[name]["candidate_hit"] for row in rows),
            "total": len(rows),
        }
        for name in ("before", "after")
    }
    summary["improved"] = sum(not row["before"]["correct"] and row["after"]["correct"] for row in rows)
    summary["regressed"] = sum(row["before"]["correct"] and not row["after"]["correct"] for row in rows)
    summary["changed"] = sum(row["before"]["title"] != row["after"]["title"] for row in rows)
    report = {"seed": sample["seed"], "rule": VENUE.pattern,
              "scope": "length >20 OR venue-free; venue filter on commas as before; dot chunks are venue-free",
              "min_title_length": MIN_TITLE_LENGTH, "summary": summary, "rows": rows}
    output.mkdir(parents=True, exist_ok=True)
    (output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    escape = html.escape
    parts = [
        '<!doctype html><html lang="vi"><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src \'none\';style-src \'unsafe-inline\'">',
        '<title>Python: OR giữa các rule</title><style>body{font:15px Segoe UI,sans-serif;margin:24px}table{border-collapse:collapse;width:100%}td,th{border:1px solid #ccc;padding:10px;text-align:left;vertical-align:top}th{background:#eef2f7}pre{white-space:pre-wrap}article{border-top:1px solid #ccc;margin:24px 0}.ok{color:#17643a}.bad{color:#a00}</style>',
        '<h1>Thử OR bằng Python</h1><p>Cùng mẫu 10 paper, 30 reference, seed 20261011. Before: dài >20 AND không có marker venue; after: dài >20 OR không có marker venue. Tách riêng theo dấu phẩy hoặc chấm. Giữ phạm vi venue cũ: chỉ đoạn dấu phẩy; do đó OR giữ mọi đoạn dấu chấm có chữ, kể cả ngắn. DOI và đoạn toàn số luôn bị loại. Tạp chí dài >20 cũng được giữ bởi OR.</p>',
        '<p>Không gọi mạng, tải PDF, build hay mở Sumatra. Input là bibliography HTML chuyển thành text, chưa kiểm tra OCR/PDF.</p>',
        f'<pre>{escape(json.dumps(summary, ensure_ascii=False, indent=2))}</pre>',
    ]
    for row in rows:
        parts.append(f'<article><h2>{escape(row["paper"])} {escape(row["label"])}</h2><p><b>Reference:</b> {escape(row["raw"])}</p><p><b>Tên chuẩn:</b> {escape(row["gold"])}</p><table><tr><th>Phiên bản</th><th>Tên chọn</th><th>Đúng</th></tr>')
        for version in ("before", "after"):
            value = row[version]
            parts.append(f'<tr><td>{version}</td><td>{escape(value["title"])}</td><td class="{"ok" if value["correct"] else "bad"}">{value["correct"]}</td></tr>')
        parts.append('</table><p><b>Đoạn đã loại:</b></p><ul>')
        for rejected in row["rejected"]:
            parts.append(f'<li>{escape(rejected["segment"])} — separator: {escape(rejected["separator"])}; {escape(rejected["marker"])}</li>')
        parts.append('</ul></article>')
    (output / "report.html").write_text("".join(parts) + "</html>", encoding="utf-8")
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--citation", help="Print title candidates for one original reference")
    parser.add_argument("--benchmark", type=Path, help="Folder containing sample.json and gold.json")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.citation is not None:
        print(json.dumps(title_candidates(args.citation), ensure_ascii=False, indent=2))
    elif args.benchmark:
        print(json.dumps(benchmark(args.benchmark, args.out or args.benchmark / "python-or"), indent=2))
    else:
        parser.error("Specify --citation or --benchmark")


if __name__ == "__main__":
    main()
