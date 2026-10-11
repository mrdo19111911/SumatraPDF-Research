import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import { runStandalone } from "./util";

export async function testit(): Promise<void> {
  const source = readFileSync("src/PaperLibrary.cpp", "utf8");
  const html = source.slice(source.indexOf("static Str LibraryHtml()")).match(/R"HTML\(([\s\S]*?)\)HTML"/)?.[1];
  if (!html?.includes('<progress id="progress"')) throw new Error("Collection progress bar missing");
  const script = html.match(/<script>([\s\S]*?)<\/script>/)?.[1];
  if (!script) throw new Error("Library script missing");
  const node = () => ({
    hidden: false,
    disabled: false,
    value: "" as string | number,
    max: 1,
    textContent: "",
    append() {},
    replaceChildren() {},
    removeAttribute(name: string) {
      if (name === "value") this.value = "";
    },
  });
  const elements = new Map<string, ReturnType<typeof node>>();
  const element = (id: string) => {
    if (!elements.has(id)) elements.set(id, node());
    return elements.get(id)!;
  };
  const context = {
    document: { getElementById: element, createElement: node },
    window: { __sumatra__: { notify() {} } },
  };
  runInNewContext(script, context);
  const update = (running: boolean, processed: number, total: number) => {
    runInNewContext(`update(${JSON.stringify({ running, processed, total, message: "", rows: [] })})`, context);
  };
  update(true, 0, 0);
  if (element("progress").hidden || element("progress").value !== "")
    throw new Error("Extraction should be indeterminate");
  update(true, 2, 5);
  if (
    element("progress").value !== 2 ||
    element("progress").max !== 5 ||
    !element("counts").textContent.includes("2 / 5")
  )
    throw new Error("Processed reference progress incorrect");
  update(false, 5, 5);
  if (element("progress").value !== 5 || element("progress").hidden) throw new Error("Finished progress missing");
  update(true, 0, 5);
  if (element("progress").value !== 0) throw new Error("Retry must reset progress");
  update(false, 2, 5);
  if (element("progress").value !== 2) throw new Error("Stop must preserve partial progress");
  update(false, 0, 0);
  if (!element("progress").hidden) throw new Error("Idle extraction should hide progress");
  console.log("PASS indeterminate extraction, processed counts, finished, retry and stopped progress");
}
if (import.meta.main) await runStandalone(testit);
