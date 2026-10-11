import { mkdirSync } from "node:fs";
import { basename, join, resolve } from "node:path";
import { currentDesktopName, runOnHiddenDesktop } from "./winapi";

const root = resolve(import.meta.dir, "..");
const args = process.argv.slice(2);
if (args[0] === "--child") {
  const [, desktop, script, log, ...testArgs] = args;
  if (currentDesktopName() !== desktop) throw new Error("Refusing to run test on the visible desktop");
  const test = Bun.spawn([process.execPath, script!, ...testArgs], {
    cwd: root,
    stdout: "pipe",
    stderr: "pipe",
  });
  const [out, err, code] = await Promise.all([
    new Response(test.stdout).text(),
    new Response(test.stderr).text(),
    test.exited,
  ]);
  await Bun.write(log!, `Desktop: ${desktop}\n${out}\n${err}`);
  process.exit(code);
}

if (!args[0]) throw new Error("Usage: bun tests/run-hidden.ts tests/<test>.ts [test arguments]");
const script = resolve(root, args[0]);
const logDir = join(root, "out", "hidden-tests");
mkdirSync(logDir, { recursive: true });
const log = join(logDir, `${basename(script, ".ts")}-${Date.now()}.log`);
const desktop = `SumatraTest-${process.pid}-${Date.now()}`;
const code = await runOnHiddenDesktop(
  process.execPath,
  [import.meta.path, "--child", desktop, script, log, ...args.slice(1)],
  desktop,
);
console.log(`Hidden test exit ${code}; log: ${log}`);
if (await Bun.file(log).exists()) console.log((await Bun.file(log).text()).slice(-2400));
process.exit(code);
