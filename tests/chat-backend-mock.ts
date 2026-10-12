const args = process.argv;
if (args.includes("app-server")) {
  console.log(JSON.stringify({ id: 2, result: { data: [{ model: "mock-codex" }] } }));
} else if (args.includes("exec")) {
  if (!args.at(-1)?.includes("FALLBACK-QUESTION")) throw new Error("Question lost during fallback");
  console.log(JSON.stringify({ type: "thread.started", thread_id: "MOCK-CODEX-THREAD" }));
  console.log(JSON.stringify({ type: "item.completed", item: { type: "agent_message", text: "CODEX-FALLBACK-OK" } }));
  console.log(JSON.stringify({ type: "turn.completed" }));
} else {
  console.log(
    JSON.stringify({
      type: "result",
      subtype: "success",
      is_error: true,
      result: "You've hit your weekly limit",
      api_error_status: 429,
    }),
  );
}
