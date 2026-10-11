import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
const args = process.argv;
const source = (args[args.indexOf("--append-system-prompt") + 1] || "").replace(
  "The user is currently reading the file: ",
  "",
);
const prompt = args.at(-1)!;
const text = readFileSync(source, "utf8");
if (!text.includes("DOCUMENT 1") || !text.includes("PAGE-2-COMPLETE")) throw new Error("Incomplete indexed source");
if (
  (prompt.includes("Compare ALL supplied") || prompt.includes("order is old then new")) &&
  !text.includes("DOCUMENT 2")
)
  throw new Error("Comparison missing second PDF");
if (prompt.includes("supplied actual PNG")) {
  const image = prompt.split("Read the actual image at: ")[1];
  if (!image || readFileSync(image).subarray(0, 8).toString("hex") !== "89504e470d0a1a0a")
    throw new Error("Figure image missing");
}
writeFileSync(join(dirname(source), "assistant-request.json"), JSON.stringify({ source, prompt, text }));
if (source.includes("ResearchReports"))
  await Bun.sleep(existsSync(join(dirname(source), "assistant-hold")) ? 4000 : 400);
console.log(
  JSON.stringify({
    type: "assistant",
    message: {
      content: [
        {
          type: "text",
          text: prompt.includes("knowledge map")
            ? JSON.stringify({
                coverage: "Fixture: 2 pages; AI relationships are unverified.",
                nodes: [
                  { id: "n1", kind: "assumption", label: "Giả định", page: 1, quote: "PAGE-1-COMPLETE" },
                  {
                    id: "n2",
                    kind: "theorem",
                    label: "<img src=x onerror=alert(1)> Kết quả",
                    page: 2,
                    quote: "PAGE-2-COMPLETE",
                  },
                  { id: "n3", kind: "limitation", label: "Nguồn không hợp lệ", page: 2, quote: "NOT-ON-PAGE" },
                ],
                edges: [{ from: "n1", to: "n2", relation: "uses", page: 2, quote: "PAGE-2-COMPLETE" }],
              })
            : "Kết quả giả lập: [trang hai](sumatra-source:1/2#PAGE-2-COMPLETE).\n\nCông thức $x^2$ và $$\\frac{a}{b}$$; \\(\\alpha+1\\) và \\[y=2\\].\n\n`$code$` và `\\(code\\)` phải giữ nguyên.",
        },
      ],
    },
  }),
);
// Claude Code 2.1.29x puts "type" last, after nested usage iterations that carry their own "type".
console.log(
  JSON.stringify({
    duration_api_ms: 1,
    usage: { iterations: [{ type: "message" }] },
    subtype: "success",
    type: "result",
  }),
);
