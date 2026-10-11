import { mkdirSync, readdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone, tmpPath } from "./util";
import {
  enumChildWindows,
  enumWindows,
  getClassName,
  getControlText,
  getWindowPid,
  getWindowText,
  isWindowVisible,
  postMessage,
  sendMessage,
  sleep,
  WM_CHAR,
  WM_COMMAND,
} from "./winapi";

async function waitFor(what: string, find: () => number): Promise<number> {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const hwnd = find();
    if (hwnd) return hwnd;
    await sleep(50);
  }
  throw new Error(`Timed out waiting for ${what}`);
}

function dialog(pid: number, title: string): number {
  let found = 0;
  enumWindows((hwnd) => {
    if (getWindowPid(hwnd) === pid && getWindowText(hwnd) === title && isWindowVisible(hwnd)) found = hwnd;
    return !found;
  });
  return found;
}

function child(parent: number, className: string, text: string): number {
  let found = 0;
  enumChildWindows(parent, (hwnd) => {
    if (
      getClassName(hwnd) === className &&
      getControlText(hwnd) === text &&
      (className === "Edit" || isWindowVisible(hwnd))
    )
      found = hwnd;
    return !found;
  });
  return found;
}

export async function testit(): Promise<void> {
  const root = join(process.env.USERPROFILE!, ".vscode", "extensions");
  const versions = readdirSync(root).filter((name) => name.startsWith("anthropic.claude-code-"));
  versions.sort((a, b) => a.localeCompare(b, undefined, { numeric: true }));
  const selected = join(root, versions[0]!, "resources", "native-binary", "claude.exe");
  const appdata = tmpPath(`claude-browse-${Date.now()}`);
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), "CheckForUpdates = false\nUiLanguage = en\n");
  let serialized = "";
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const initial = await client.request(ControlCommand.TestAIChatSetup, ["path"]);
      if (initial[0] !== 0 || !String(initial[1]).endsWith("claude.exe"))
        throw new Error(`Auto-detection failed: ${initial}`);

      const cancelled = client.request(ControlCommand.TestAIChatSetup, ["browse"]);
      const first = await waitFor("missing Claude dialog", () => dialog(proc.pid!, "Claude chat"));
      await waitFor("Cancel button", () => child(first, "Button", "Cancel"));
      postMessage(first, 0x0466, 1, 0);
      const cancelResult = await cancelled;
      if (cancelResult[0] !== 1 || cancelResult[1] !== initial[1])
        throw new Error("Cancel changed the selected executable");

      const browse = client.request(ControlCommand.TestAIChatSetup, ["browse"]);
      const prompt = await waitFor("Browse dialog", () => dialog(proc.pid!, "Claude chat"));
      const button = await waitFor("Browse button", () => child(prompt, "Button", "Browse..."));
      postMessage(prompt, 0x0466, 101, 0);
      const picker = await waitFor("Claude executable picker", () =>
        dialog(proc.pid!, "Locate Claude Code (claude.exe)"),
      );
      const edit = await waitFor(
        "file name edit",
        () => child(picker, "Edit", "claude.exe") || child(picker, "Edit", "claude"),
      );
      sendMessage(edit, 0x00b1, 0, -1);
      for (const char of selected) sendMessage(edit, WM_CHAR, char.charCodeAt(0), 0);
      postMessage(picker, WM_COMMAND, 1, 0);
      const result = await browse;
      if (result[0] !== 0 || String(result[1]).toLowerCase() !== selected.toLowerCase())
        throw new Error(`Browse failed: ${result}`);
      serialized = String((await client.request(ControlCommand.TestAIChatSetup, ["settings"]))[1]);
      if (!serialized.includes(`ExecutablePath = ${selected}`)) throw new Error("Selected path was not serialized");
    },
    ["-appdata", appdata],
  );
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), serialized);
  await withControlledSumatra(
    EXE,
    async (client) => {
      const reloaded = await client.request(ControlCommand.TestAIChatSetup, ["path"]);
      if (reloaded[0] !== 0 || String(reloaded[1]).toLowerCase() !== selected.toLowerCase())
        throw new Error("Saved path was not reloaded");
    },
    ["-appdata", appdata],
  );
  console.log("PASS: auto-detection, Cancel, Browse, settings serialization and reload; no AI request sent");
}

if (import.meta.main) {
  await runStandalone(testit);
}
