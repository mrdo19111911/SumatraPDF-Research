import { structure } from "./ad-hoc-title-benchmark";
import { runStandalone } from "./util";

export async function testit(): Promise<void> {
  const cases = [
    ["A. Author, A theorem on compact spaces, Math. Journal 8 (1999), 20-30.", "A theorem on compact spaces"],
    ["Author, A.: A theorem on compact spaces. Math. Journal 8, 20-30 (1999).", "A theorem on compact spaces"],
    [
      "Alice Author and Bob Writer. A theorem on compact spaces. Journal of Geometry, 8:20-30, 1999.",
      "A theorem on compact spaces",
    ],
    ["A. Author (1999) A theorem on compact spaces . Journal of Geometry 8, 20-30.", "A theorem on compact spaces"],
    [
      "A. Author, Density: an almost every result, Journal of Geometry 8 (1999), 20-30.",
      "Density: an almost every result",
    ],
    [
      "A. Author, A theorem on GL(n,F), where F is a local field, Journal of Geometry 8 (1999), 20-30.",
      "A theorem on GL(n,F), where F is a local field",
    ],
    ["K.-H. Author. A theorem on compact spaces. Journal of Geometry, 8:20-30, 1999.", "A theorem on compact spaces"],
    [
      "A. Author (1999) Estimates in small intervals. III . Journal of Geometry 8, 20-30.",
      "Estimates in small intervals. III",
    ],
    [
      "A. Author, Density: a translation-invariant “almost every” result, Journal of Geometry 8 (1999), 20-30.",
      "Density: a translation-invariant “almost every” result",
    ],
  ];
  for (const [raw, expected] of cases) {
    const actual = structure(raw!);
    if (actual !== expected) throw new Error(`Expected ${expected}; got ${actual}; citation ${raw}`);
  }
  console.log(`PASS ${cases.length} synthetic citation formats`);
}
if (import.meta.main) await runStandalone(testit);
