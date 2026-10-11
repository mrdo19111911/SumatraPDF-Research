"""Compare a fixed rule grid on the frozen citation sample; no network calls."""

import argparse
import html
import itertools
import json
import re
from pathlib import Path

from title_detector import DOI, VENUE, clean, normalize, structured_title, title_candidates

LENGTHS = (8, 15, 21)
MODES = ("and", "or")
SCOPES = ("none", "comma", "both")
SPLITS = ("comma", "dot", "union", "mixed")
RANKS = ("length", "penalty")
METADATA = re.compile(
    r"\b(?:journal|journals|annals|proceedings|transactions|press|university|"
    r"springer|volume|vol\.|pages|preprint|arxiv|external links|cited by|"
    r"mathreview|issn)\b|https?://|\b(?:18|19|20)\d{2}\b|\bMR\d+", re.I
)
AUTHOR_INITIAL = re.compile(r"\b[A-Z]\.\s*[A-Z][a-z]")
METADATA_PENALTY = 1000
AUTHOR_PENALTY = 100
SHORT_PENALTY = 40


def rank_value(text: str, ranking: str) -> int:
    value = len(text.encode("utf-8"))
    if ranking == "length":
        return value
    value -= METADATA_PENALTY * len(METADATA.findall(text))
    value -= AUTHOR_PENALTY * len(AUTHOR_INITIAL.findall(text))
    if len(text.split()) <= 2:
        value -= SHORT_PENALTY
    return value


def grid_candidates(raw: str, minimum: int, mode: str, scope: str,
                    split: str, ranking: str) -> list[str]:
    separators = (",", ".") if split == "union" else (split,)
    candidates = []
    for separator in separators:
        pattern = {",": ",", ".": r"\.(?=\s|$)", "comma": ",",
                   "dot": r"\.(?=\s|$)", "mixed": r",|\.(?=\s|$)"}[separator]
        parts = []
        for item in re.split(pattern, raw):
            part = clean(item)
            if not any(c.isalpha() for c in part) or DOI.search(part):
                continue
            filter_venue = scope == "both" or scope == "comma" and separator in (",", "comma")
            length_ok = len(part) >= minimum
            venue_ok = not (filter_venue and VENUE.search(part))
            allowed = (length_ok or venue_ok) if mode == "or" else (length_ok and venue_ok)
            if allowed:
                parts.append(part)
        candidates.extend(sorted(parts, key=lambda s: rank_value(s, ranking), reverse=True)[:2])
    return sorted(dict.fromkeys(candidates), key=lambda s: rank_value(s, ranking), reverse=True)


def evaluate(refs: list[dict], candidates: list[list[str]], method: str) -> dict:
    rows = []
    for ref, titles in zip(refs, candidates, strict=True):
        expected = {normalize(t) for t in [ref["gold"], *ref["aliases"]]}
        chosen = titles[0] if titles else ""
        rows.append({**ref, "prediction": chosen, "candidates": titles,
                     "correct": bool(chosen and normalize(chosen) in expected),
                     "candidate_hit": any(normalize(t) in expected for t in titles)})
    return {"method": method, "correct": sum(r["correct"] for r in rows),
            "candidate_hit": sum(r["candidate_hit"] for r in rows),
            "unresolved": sum(not r["prediction"] for r in rows), "total": len(rows), "rows": rows}


def sweep(folder: Path, output: Path) -> dict:
    sample = json.loads((folder / "sample.json").read_text(encoding="utf-8"))
    gold = {r["key"]: r for r in json.loads((folder / "gold.json").read_text(encoding="utf-8"))}
    refs = [{"key": r["key"], "paper": p["id"], "label": r["label"], "raw": r["raw"],
             "gold": gold[r["key"]]["title"], "aliases": gold[r["key"]].get("aliases", [])}
            for p in sample["papers"] for r in p["references"]]
    methods = []
    for mode in (*MODES, "veto"):
        methods.append(evaluate(refs, [title_candidates(r["raw"], mode=mode) for r in refs], f"baseline-{mode}-21"))
    for minimum, mode, scope, split, ranking in itertools.product(LENGTHS, MODES, SCOPES, SPLITS, RANKS):
        name = f"{mode}-{minimum}-{scope}-{split}-{ranking}"
        choices = [grid_candidates(r["raw"], minimum, mode, scope, split, ranking) for r in refs]
        methods.append(evaluate(refs, choices, name))
    for repaired in (False, True):
        choices = [[title] if (title := structured_title(r["raw"], repaired=repaired)) else [] for r in refs]
        methods.append(evaluate(refs, choices, "structure-repaired" if repaired else "structure-old"))
    choices = []
    for ref in refs:
        title = structured_title(ref["raw"])
        fallback = grid_candidates(ref["raw"], 8, "and", "both", "union", "penalty")
        choices.append(list(dict.fromkeys(([title] if title else []) + fallback)))
    methods.append(evaluate(refs, choices, "structure-with-fallback"))
    leaderboard = sorted(methods, key=lambda m: (-m["correct"], m["unresolved"], m["method"]))
    best = leaderboard[0]
    summary = {"seed": sample["seed"], "paper_count": len(sample["papers"]), "reference_count": len(refs),
               "grid_count": len(LENGTHS) * len(MODES) * len(SCOPES) * len(SPLITS) * len(RANKS),
               "method_count": len(methods), "best": {k: best[k] for k in ("method", "correct", "total", "candidate_hit")},
               "venue_pattern": VENUE.pattern,
               "limits": "Development sample already inspected; no held-out accuracy claim. HTML bibliography text, not PDF/OCR. No download-rate measurement.",
               "grid": {"minimum": LENGTHS, "mode": MODES, "venue_scope": SCOPES, "split": SPLITS, "ranking": RANKS}}
    output.mkdir(parents=True, exist_ok=True)
    (output / "results.json").write_text(json.dumps({**summary, "methods": leaderboard}, ensure_ascii=False, indent=2), encoding="utf-8")
    escape = html.escape
    parts = [
        '<!doctype html><html lang="vi"><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src \'none\';style-src \'unsafe-inline\'">',
        '<title>So sánh rule tách tên reference</title><style>body{font:15px Segoe UI,sans-serif;max-width:1500px;margin:24px auto;padding:16px}table{border-collapse:collapse;width:100%}td,th{border:1px solid #ccc;padding:8px;text-align:left}article{border-top:1px solid #bbb;margin-top:24px}.bad{color:#a00}pre{white-space:pre-wrap}summary{cursor:pointer}</style>',
        '<h1>So sánh rule tách tên reference</h1>',
        '<p>144 tổ hợp: AND/OR × ngưỡng 8/15/21 ký tự × lọc venue không/phẩy/cả hai × tách phẩy/chấm/hợp/tách hỗn hợp × xếp độ dài/phạt metadata. Thêm 3 baseline và 3 cách nhận cấu trúc.</p>',
        '<p><b>Rule mới (baseline-veto-21): loại đoạn chứa Jour/Journal, J., Proc/Proce/Proceed/Proceeding(s), Conf/Conference, Ann/Annals, IEEE, Trans/Transactions, Vol/Volume dù dài hơn 20 ký tự. Áp dụng cả phẩy và chấm, không phân biệt hoa thường. Khớp nguyên từ, không loại Processes, Procedures, Journey, Conformal, Annular, Transport, Transversality hay Volterra.</b></p>',
        '<p>OR = dài đủ HOẶC không có marker venue. Vì vậy tên tạp chí dài vẫn lọt. Phạt metadata dùng dấu hiệu năm, publisher, URL và initials; không dùng tên chuẩn. Structure bảo vệ initials, nhận hết danh sách tác giả, phân biệt dấu hai chấm của tên với của tác giả, giữ dấu phẩy/công thức/phụ đề trong tên.</p>',
        '<p><b>Đây là mẫu phát triển đã xem: 30 reference của 10 paper gần đây, không phải kiểm định độc lập. Không gọi mạng, không build Sumatra; chưa đo tỷ lệ tải PDF hay OCR. “Trong ứng viên” không đồng nghĩa đã chọn đúng.</b></p>',
        f'<pre>{escape(json.dumps(summary, ensure_ascii=False, indent=2))}</pre>',
        '<h2>Tất cả kết quả</h2><table><tr><th>Rule</th><th>Chọn đúng</th><th>Trong ứng viên</th><th>Không đoán</th></tr>',
    ]
    for method in leaderboard:
        parts.append(f'<tr><td>{escape(method["method"])}</td><td>{method["correct"]}/{method["total"]}</td><td>{method["candidate_hit"]}/{method["total"]}</td><td>{method["unresolved"]}</td></tr>')
    parts.append('</table><h2>Thay đổi do rule veto mới, so với OR</h2>')
    baseline_or = next(m for m in methods if m["method"] == "baseline-or-21")
    baseline_veto = next(m for m in methods if m["method"] == "baseline-veto-21")
    for old, new in zip(baseline_or["rows"], baseline_veto["rows"], strict=True):
        if old["prediction"] != new["prediction"]:
            parts.append(f'<article><b>{escape(new["key"])}</b><p>Reference: {escape(new["raw"])}</p><p>OR: {escape(old["prediction"])}</p><p>Veto: {escape(new["prediction"])} — đúng: {new["correct"]}</p><p>Chuẩn: {escape(new["gold"])}</p></article>')
    parts.append('<h2>Reference chưa đúng với cách tốt nhất</h2>')
    for row in best["rows"]:
        if not row["correct"]:
            parts.append(f'<article><h3>{escape(row["paper"])} {escape(row["label"])}</h3><p><b>Toàn bộ dữ liệu:</b> {escape(row["raw"])}</p><p><b>Tên chuẩn:</b> {escape(row["gold"])}</p><p class="bad"><b>Dự đoán:</b> {escape(row["prediction"])}</p></article>')
    parts.append('<h2>Từng reference của cách tốt nhất</h2>')
    for row in best["rows"]:
        parts.append(f'<article><b>{escape(row["paper"])} {escape(row["label"])}</b><p>{escape(row["raw"])}</p><p>Tên chuẩn: {escape(row["gold"])}</p><p>Dự đoán: {escape(row["prediction"])} — đúng: {row["correct"]}</p></article>')
    parts.append('<h2>Chi tiết các rule khác</h2>')
    for method in leaderboard:
        parts.append(f'<details><summary>{escape(method["method"])} — {method["correct"]}/{method["total"]}</summary>')
        for row in method["rows"]:
            if not row["correct"]:
                parts.append(f'<p><b>{escape(row["key"])}</b><br>Reference: {escape(row["raw"])}<br>Chuẩn: {escape(row["gold"])}<br>Đoán: {escape(row["prediction"])}</p>')
        parts.append('</details>')
    (output / "report.html").write_text("".join(parts) + "</html>", encoding="utf-8")
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    print(json.dumps(sweep(args.benchmark, args.out or args.benchmark / "python-sweep"), ensure_ascii=False, indent=2))
