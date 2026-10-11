import { parseMap } from "../resources/research/paper-map.js";
import { strict as assert } from "node:assert";
import { runStandalone } from "./util";

export async function testit(): Promise<void> {
  const node = { id: "n1", kind: "theorem", label: "A theorem", page: 2, quote: "An exact source passage" };
  const data = {
    coverage: "Two pages",
    nodes: [node, { ...node, id: "n2" }],
    edges: [{ from: "n1", to: "n2", relation: "uses", page: 2, quote: "An exact source passage" }],
  };
  assert.equal(parseMap(JSON.stringify(data)).edges.length, 1);
  assert.equal(parseMap("```json\n" + JSON.stringify(data) + "\n```").nodes.length, 2);
  for (const bad of [
    { ...data, nodes: [node, node] },
    { ...data, nodes: [{ ...node, page: 0 }] },
    { ...data, nodes: [{ ...node, quote: "x" }] },
    { ...data, nodes: [{ ...node, kind: "invented" }] },
    { ...data, edges: [{ ...data.edges[0], to: "missing" }] },
    { ...data, edges: [{ ...data.edges[0], relation: "verified-proof" }] },
    { ...data, edges: [{ ...data.edges[0], quote: "" }] },
    { ...data, nodes: Array.from({ length: 61 }, (_, i) => ({ ...node, id: `n${i}` })) },
  ])
    assert.throws(() => parseMap(JSON.stringify(bad)));
  console.log("PASS KG schema rejects unanchored nodes, edges, duplicates and excess output.");
}
if (import.meta.main) await runStandalone(testit);
