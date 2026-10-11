import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
const args = process.argv;
const system = args[args.indexOf("--append-system-prompt") + 1] || "";
const source = system.replace("The user is currently reading the file: ", "");
const prompt = args.at(-1)!;
const text = readFileSync(source, "utf8");
writeFileSync(join(dirname(source), "narration-request.json"), JSON.stringify({ source, prompt, text }));
if (!text.includes("PAGE-1-COMPLETE") || !text.includes("PAGE-2-COMPLETE")) throw new Error("Incomplete source");
if (source.includes("async.ideas")) await Bun.sleep(5000);
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
