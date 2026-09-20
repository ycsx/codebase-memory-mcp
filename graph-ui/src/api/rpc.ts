/* JSON-RPC client — speaks the same protocol as MCP clients via POST /rpc */

let _nextId = 1;

export class RpcError extends Error {
  constructor(
    public code: number,
    message: string,
  ) {
    super(message);
    this.name = "RpcError";
  }
}

export async function callTool<T = unknown>(
  name: string,
  args: Record<string, unknown> = {},
  signal?: AbortSignal,
): Promise<T> {
  const res = await fetch("/rpc", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    signal,
    body: JSON.stringify({
      jsonrpc: "2.0",
      id: _nextId++,
      method: "tools/call",
      params: { name, arguments: args },
    }),
  });

  if (!res.ok) {
    throw new RpcError(-1, `HTTP ${res.status}: ${res.statusText}`);
  }

  const json = await res.json();

  if (json.error) {
    throw new RpcError(json.error.code ?? -1, json.error.message ?? "unknown");
  }

  /* MCP tool results are wrapped: { result: { content: [{ text: "..." }] } } */
  const text = json?.result?.content?.[0]?.text;
  if (json?.result?.isError) {
    const messages = Array.isArray(json.result.content)
      ? json.result.content
          .map((item: { text?: unknown }) =>
            typeof item?.text === "string" ? toolErrorMessage(item.text) : "",
          )
          .filter(Boolean)
      : [];
    throw new RpcError(-1, messages.join("\n") || `Tool ${name} failed`);
  }
  if (text === undefined) {
    return json.result as T;
  }

  return JSON.parse(text) as T;
}

function toolErrorMessage(text: string): string {
  let payload: unknown;
  try {
    payload = JSON.parse(text);
  } catch {
    return text;
  }
  if (typeof payload === "string") return payload;
  if (payload && typeof payload === "object") {
    const error = payload as { message?: unknown; error?: unknown };
    if (typeof error.message === "string") return error.message;
    if (typeof error.error === "string") return error.error;
    if (error.error && typeof error.error === "object") {
      const nested = error.error as { message?: unknown };
      if (typeof nested.message === "string") return nested.message;
    }
  }
  return text;
}
