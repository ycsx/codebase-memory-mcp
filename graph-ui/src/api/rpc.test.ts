import { afterEach, describe, expect, it, vi } from "vitest";
import { callTool, RpcError } from "./rpc";

function respond(body: unknown, options: Partial<Response> = {}) {
  const fetchMock = vi.fn().mockResolvedValue({
    ok: true,
    json: async () => body,
    ...options,
  });
  vi.stubGlobal("fetch", fetchMock);
  return fetchMock;
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe("callTool", () => {
  it("unwraps normal tool results and forwards the optional abort signal", async () => {
    const fetchMock = respond({
      result: { content: [{ type: "text", text: '{"documents":[]}' }] },
    });
    const controller = new AbortController();
    await expect(
      callTool("get_related_documents", { project: "demo" }, controller.signal),
    ).resolves.toEqual({ documents: [] });
    expect(fetchMock).toHaveBeenCalledWith(
      "/rpc",
      expect.objectContaining({
        method: "POST",
        signal: controller.signal,
      }),
    );
    expect(JSON.parse(fetchMock.mock.calls[0][1].body)).toMatchObject({
      jsonrpc: "2.0",
      method: "tools/call",
      params: {
        name: "get_related_documents",
        arguments: { project: "demo" },
      },
    });
  });

  it("supports calls without arguments or cancellation and unwrapped results", async () => {
    respond({ result: { projects: [] } });
    await expect(callTool("list_projects")).resolves.toEqual({ projects: [] });
  });

  it("preserves abort errors", async () => {
    const aborted = new DOMException("Request aborted", "AbortError");
    vi.stubGlobal("fetch", vi.fn().mockRejectedValue(aborted));
    await expect(
      callTool("get_related_documents", {}, new AbortController().signal),
    ).rejects.toBe(aborted);
  });

  it("preserves network failures", async () => {
    const failure = new TypeError("Failed to fetch");
    vi.stubGlobal("fetch", vi.fn().mockRejectedValue(failure));
    await expect(callTool("list_projects")).rejects.toBe(failure);
  });

  it("reports HTTP failures", async () => {
    respond(null, { ok: false, status: 503, statusText: "Unavailable" });
    await expect(callTool("list_projects")).rejects.toMatchObject({
      name: "RpcError",
      code: -1,
      message: "HTTP 503: Unavailable",
    });
  });

  it("preserves protocol error codes and messages", async () => {
    respond({ error: { code: -32602, message: "Missing project" } });
    await expect(callTool("get_related_documents")).rejects.toMatchObject({
      name: "RpcError",
      code: -32602,
      message: "Missing project",
    });
  });

  it.each([
    ["plain text", "Project not indexed", "Project not indexed"],
    ["JSON string", '"Project not indexed"', "Project not indexed"],
    ["JSON error", '{"error":"Project not indexed"}', "Project not indexed"],
    ["JSON message", '{"message":"Project not indexed"}', "Project not indexed"],
    [
      "nested JSON error",
      '{"error":{"code":42,"message":"Project not indexed"}}',
      "Project not indexed",
    ],
    ["unrecognized JSON", '{"reason":"unknown"}', '{"reason":"unknown"}'],
  ])("reports readable MCP errors from %s", async (_label, text, message) => {
    respond({ result: { isError: true, content: [{ type: "text", text }] } });
    await expect(callTool("get_related_documents")).rejects.toEqual(
      new RpcError(-1, message),
    );
  });

  it("includes all textual error blocks and ignores non-text blocks", async () => {
    respond({
      result: {
        isError: true,
        content: [
          { type: "image", data: "ignored" },
          { type: "text", text: "Lookup failed" },
          { type: "text", text: '{"error":"Try indexing again"}' },
        ],
      },
    });
    await expect(callTool("get_related_documents")).rejects.toThrow(
      "Lookup failed\nTry indexing again",
    );
  });

  it("uses a fallback for tool errors without text", async () => {
    respond({ result: { isError: true } });
    await expect(callTool("get_related_documents")).rejects.toThrow(
      "Tool get_related_documents failed",
    );
  });

  it("does not reinterpret successful payloads containing an error field", async () => {
    respond({
      result: {
        isError: false,
        content: [{ text: '{"error":null,"documents":[]}' }],
      },
    });
    await expect(callTool("get_related_documents")).resolves.toEqual({
      error: null,
      documents: [],
    });
  });
});
