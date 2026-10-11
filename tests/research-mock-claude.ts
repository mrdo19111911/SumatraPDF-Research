import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
const args = process.argv;
const system = args[args.indexOf("--append-system-prompt") + 1] || "";
const source = system.replace("The user is currently reading the file: ", "");
const prompt = args.at(-1)!;
const limit = "You've hit your weekly limit · resets Oct 15, 11am (Asia/Bangkok)";
function printLimit(): never {
  // what Claude Code 2.1.295 prints at a usage limit: synthetic text, "success" subtype, is_error, exit 1
  console.log(
    JSON.stringify({ type: "assistant", message: { model: "<synthetic>", content: [{ type: "text", text: limit }] } }),
  );
  console.log(
    JSON.stringify({ type: "result", subtype: "success", is_error: true, api_error_status: 429, result: limit }),
  );
  process.exit(1);
}
// selection translation passes no source file, only the prompt
if (!args.includes("--append-system-prompt")) {
  if (prompt.includes("MOCK-RATE-LIMIT")) printLimit();
  console.log(JSON.stringify({ type: "assistant", message: { content: [{ type: "text", text: "W lipcu" }] } }));
  console.log(JSON.stringify({ type: "result", subtype: "success", result: "W lipcu" }));
  process.exit(0);
}
const text = readFileSync(source, "utf8");
writeFileSync(join(dirname(source), "narration-request.json"), JSON.stringify({ source, prompt, text }));
if (!text.includes("PAGE-1-COMPLETE") || !text.includes("PAGE-2-COMPLETE")) throw new Error("Incomplete source");
if (source.includes("async.ideas")) await Bun.sleep(5000);
if (text.includes("MOCK-RATE-LIMIT")) printLimit();
console.log(
  JSON.stringify({
    type: "assistant",
    message: {
      content: [
        {
          type: "text",
          text: "Đây là bản diễn giải giả lập để kiểm tra luồng xuất âm thanh. Bài báo có hai trang. Phần đầu trình bày câu hỏi nghiên cứu. Phần sau giải thích cách tiếp cận và giới hạn của kết quả.",
        },
      ],
    },
  }),
);
console.log(JSON.stringify({ type: "result", subtype: "success" }));
