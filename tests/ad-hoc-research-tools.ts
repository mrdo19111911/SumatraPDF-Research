import { mkdirSync, readFileSync, writeFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { enumWindows, getWindowPid, getWindowText, postMessage, sleep } from "./winapi";

export async function testit(): Promise<void> {
  const root = tmpPath(`research-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const pdf = join(root, "source.pdf"),
    source = join(root, "source.txt"),
    downloaded = join(root, "download.pdf");
  const objects = ["<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>"];
  for (let i = 0; i < 2; i++) {
    const content = `BT /F1 16 Tf 70 700 Td (PAGE-${i + 1}-COMPLETE) Tj ET`;
    objects.push(
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 7 0 R >> >> /Contents " +
        (4 + i * 2) +
        " 0 R >>",
      `<< /Length ${content.length} >>\nstream\n${content}\nendstream`,
    );
  }
  objects.push("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  const payload = assemblePdf(objects);
  writeFileSync(pdf, payload, "latin1");
  const mock = join(root, "claude.exe");
  const compile = Bun.spawnSync([
    process.execPath,
    "build",
    "--compile",
    "tests/research-mock-claude.ts",
    "--outfile",
    mock,
  ]);
  if (compile.exitCode) throw new Error(compile.stderr.toString());
  let hits = 0;
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    fetch(req) {
      hits++;
      const path = new URL(req.url).pathname;
      if (path === "/paper") return new Response(`<html><iframe src="/actual.pdf"></iframe></html>`);
      if (path === "/actual.pdf")
        return new Response(Buffer.from(payload, "latin1"), { headers: { "Content-Type": "application/pdf" } });
      if (path === "/captcha") return new Response("<html>CAPTCHA</html>");
      if (path === "/fake.pdf") return new Response("%PDF-pretend but this is not a PDF");
      if (path === "/bad") return new Response('<iframe src="file:///C:/private.pdf"></iframe>');
      if (path === "/too-big") return new Response("X".repeat(1024 * 1024 + 1));
      return new Response("missing", { status: 404 });
    },
  });
  writeFileSync(
    join(root, "SumatraPDF-settings.txt"),
    `CheckForUpdates = false\nUiLanguage = en\nResearchSciHubUrl = http://127.0.0.1:${server.port}/paper\nClaudeCode [\n ExecutablePath = ${mock}\n]\n`,
  );
  try {
    await withControlledSumatra(
      EXE,
      async (client, proc) => {
        const pending = client.request(ControlCommand.TestInvokeCommand, ["CmdResearchTools"]);
        const deadline = Date.now() + 10000;
        let dialog = 0;
        while (!dialog && Date.now() < deadline) {
          enumWindows((hwnd) => {
            if (getWindowPid(hwnd) === proc.pid && getWindowText(hwnd) === "Research tools") dialog = hwnd;
            return true;
          });
          await sleep(50);
        }
        if (!dialog) throw new Error("Research dialog missing");
        postMessage(dialog, 0x0466, 2, 0);
        const invoke = await pending;
        if (invoke[0] !== 0) throw new Error("Research entry point missing");
        async function check(args: string[], success: boolean): Promise<string> {
          const result = await client.request(ControlCommand.TestResearchTools, args);
          if ((result[0] === 0) !== success) throw new Error(`${args.join(" | ")}: ${result}`);
          return String(result[1]);
        }
        const base = `http://127.0.0.1:${server.port}`;
        await check(["extract", pdf, source], true);
        const text = readFileSync(source, "utf8");
        if (!text.includes("PAGE-1-COMPLETE") || !text.includes("PAGE-2-COMPLETE") || !text.includes("--- PAGE 2 ---"))
          throw new Error("Not all pages extracted");
        const narration = join(root, "ideas.vi.txt");
        await check(["narrate", source, narration], true);
        const request = JSON.parse(readFileSync(join(root, "narration-request.json"), "utf8"));
        if (
          !request.prompt.includes("ALL pages") ||
          !request.prompt.includes("NO mathematical notation") ||
          !request.prompt.includes("Vietnamese")
        )
          throw new Error("Narration prompt contract lost");
        if (!readFileSync(narration, "utf8").includes("Bài báo")) throw new Error("Vietnamese narration not saved");
        const voiceCache = process.env.RESEARCH_VOICE_CACHE;
        if (voiceCache) {
          const mp3 = join(root, "ideas.vi.mp3");
          await check(["speech", narration, mp3, voiceCache], true);
          const probe = Bun.spawnSync([
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "stream=codec_name:format=duration",
            "-of",
            "json",
            mp3,
          ]);
          if (probe.exitCode) throw new Error(probe.stderr.toString());
          const audio = JSON.parse(probe.stdout.toString());
          if (audio.streams[0]?.codec_name !== "mp3" || Number(audio.format.duration) < 4)
            throw new Error("Invalid MP3 output");
          const saved = readFileSync(mp3);
          await check(["speech", narration, mp3, voiceCache], false);
          if (!readFileSync(mp3).equals(saved)) throw new Error("MP3 overwritten");
          const formula = join(root, "formula.txt"),
            rejected = join(root, "formula.mp3");
          writeFileSync(formula, "Remove formula $x^2$ before narration.");
          await check(["speech", formula, rejected, voiceCache], false);
          if (existsSync(rejected)) throw new Error("Formula rejection created MP3");
          console.log(
            `PASS: native local Vietnamese TTS, real MP3 ${audio.format.duration}s, no overwrite, formula rejection.`,
          );
        }
        const asyncNarration = join(root, "async.ideas.vi.txt");
        await check(["start-narrate", pdf, asyncNarration], true);
        const responsive = Date.now();
        const state = await check(["running"], true);
        if (state !== "running" || Date.now() - responsive > 1000) throw new Error("Research worker blocks UI");
        const launchedBy = Date.now() + 4000;
        while (!JSON.parse(readFileSync(join(root, "narration-request.json"), "utf8")).source.includes("async.ideas")) {
          if (Date.now() > launchedBy) throw new Error("Async narration never launched the mock CLI");
          await sleep(50);
        }
        await check(["cancel"], true);
        const stoppedBy = Date.now() + 5000;
        while ((await check(["running"], true)) === "running" && Date.now() < stoppedBy) await sleep(50);
        if ((await check(["running"], true)) !== "idle") throw new Error("Cancellation did not finish");
        if (existsSync(asyncNarration)) throw new Error("Cancelled AI narration saved output");
        for (const bad of [
          "file:///C:/secret",
          "javascript:alert(1)",
          "http://example.org/",
          "https://user:password@example.org/",
        ])
          await check(["allow", bad], false);
        await check(["allow", "https://example.org/"], true);
        const resolved = await check(["resolve", `${base}/paper`, `<object data='/actual.pdf'></object>`], true);
        if (resolved !== `${base}/actual.pdf`) throw new Error(`Wrong relative URL: ${resolved}`);
        await check(["resolve", `${base}/paper`, `<iframe src='javascript:alert(1)'></iframe>`], false);
        await check(["fetch", `${base}/paper`, downloaded], true);
        if (readFileSync(downloaded).toString("latin1") !== payload) throw new Error("PDF changed");
        await check(["fetch", `${base}/paper`, downloaded], false);
        if (readFileSync(downloaded).toString("latin1") !== payload) throw new Error("Existing PDF overwritten");
        for (const path of ["captcha", "bad", "too-big", "missing", "fake.pdf"]) {
          const failed = join(root, `${path}.pdf`);
          await check(["fetch", `${base}/${path}`, failed], false);
          if (existsSync(failed)) throw new Error(`Failure left a fake PDF: ${path}`);
        }
        if (hits < 8) throw new Error("HTTP fixtures were not exercised");
        for (const [command, title] of [
          ["CmdSciHub", "Sci-Hub"],
          ["CmdConnectedPapers", "Connected Papers"],
        ]) {
          await client.request(ControlCommand.TestInvokeCommand, [command!]);
          await sleep(1000);
          let browser = 0;
          enumWindows((hwnd) => {
            if (getWindowPid(hwnd) === proc.pid && getWindowText(hwnd).startsWith(title!)) browser = hwnd;
            return true;
          });
          if (!browser) throw new Error(`Embedded browser missing: ${title}`);
          postMessage(browser, 0x0010, 0, 0);
          await sleep(300);
        }
        console.log(
          "PASS: all-page text, URL validation, relative PDF, no overwrite, CAPTCHA/errors/size cap, embedded browser. Local HTTP only.",
        );
      },
      ["-appdata", root, pdf],
    );
  } finally {
    server.stop();
  }
}
if (import.meta.main) await runStandalone(testit);
