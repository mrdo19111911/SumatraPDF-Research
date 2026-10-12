import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone } from "./util";
import {
  enumWindows,
  getClassName,
  getWindowPid,
  getWindowText,
  getProductVersion,
  postMessage,
  sleep,
} from "./winapi";

export async function testit(): Promise<void> {
  const expected = "4.8.1";
  const actual = getProductVersion(EXE);
  if (actual !== expected) throw new Error(`Windows version is ${actual}, expected ${expected}`);
  await withControlledSumatra(EXE, async (client, proc) => {
    await client.request(ControlCommand.TestInvokeCommand, ["CmdHelpAbout"]);
    let title = "",
      about = 0;
    const deadline = Date.now() + 5000;
    while (!about && Date.now() < deadline) {
      enumWindows((hwnd) => {
        if (getWindowPid(hwnd) === proc.pid && getClassName(hwnd) === "SUMATRA_PDF_ABOUT") {
          about = hwnd;
          title = getWindowText(hwnd);
        }
        return true;
      });
      if (!about) await sleep(30);
    }
    if (!title.includes("SumatraPDF Research") || !title.includes(expected))
      throw new Error(`About does not identify the build: ${title}`);
    postMessage(about, 0x0010, 0, 0);
    console.log(`PASS Windows metadata and Help/About identify Research ${actual}`);
  });
}
if (import.meta.main) await runStandalone(testit);
