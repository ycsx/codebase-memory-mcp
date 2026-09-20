/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { act, cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { RelatedDocuments, RelatedDocumentsPanel, type RelatedDocumentsResult } from "./RelatedDocuments";

const callToolMock = vi.hoisted(() => vi.fn());
vi.mock("../api/rpc", () => ({ callTool: callToolMock }));

const RESULT: RelatedDocumentsResult = {
  status: "ok", total: 1, returned: 1,
  references: [{
    source: { qualified_name: "demo.docs.guide.section", name: "Guide section", file_path: "docs/guide.md", start_line: 10 },
    target: { qualified_name: "demo.src.core.run", file_path: "src/core.ts" },
    properties: { matched_text: "src/core.ts", document_span: { start_line: 12, end_line: 12 } },
  }],
};

afterEach(() => { cleanup(); callToolMock.mockReset(); });

describe("RelatedDocuments", () => {
  it("reads exact source without graph nodes and highlights absolute evidence lines as plain text", async () => {
    callToolMock.mockResolvedValue({ source: "# Guide\n\n<script>window.bad = true</script> src/core.ts\nlast", start_line: 10 });
    const { container } = render(<RelatedDocuments project="demo" result={RESULT} />);
    fireEvent.click(screen.getByRole("button", { name: "阅读文档 docs/guide.md 第 12 行" }));
    await screen.findByText("<script>window.bad = true</script>", { exact: false });
    expect(callToolMock).toHaveBeenCalledWith("get_code_snippet", {
      project: "demo", qualified_name: "demo.docs.guide.section",
    }, expect.any(AbortSignal));
    expect(container.querySelector('[data-source-line="12"]')).toHaveAttribute("data-highlighted", "true");
    expect(container.querySelector("script")).toBeNull();
    fireEvent.click(screen.getByRole("button", { name: "关闭文档原文" }));
    expect(screen.queryByLabelText("文档原文")).not.toBeInTheDocument();
  });

  it("renders review basis and conservative reminders, limited and budget states", () => {
    render(<RelatedDocuments project="demo" result={{
      ...RESULT, status: "limited", reason: "document_analysis_limited", truncated: true, budget_truncated: true,
      references: [{ ...RESULT.references![0], review: { changed_file: "src/core.ts", document_changed: true, message_zh: "建议核对说明，并非已经过期。" } }],
      review_basis: { requested_ref: "main", comparison: "merge_base_to_head_plus_worktree", reference_snapshot: "current_index", freshness: "stale", limitation: "Current indexed references only." },
    }} />);
    expect(screen.getByText("建议复核")).toBeInTheDocument();
    expect(screen.getByText("分析受限")).toBeInTheDocument();
    expect(screen.getByText(/文档证据已按预算截断/)).toBeInTheDocument();
    expect(screen.getByText(/文档也在本次变更中/)).toBeInTheDocument();
    expect(screen.getByLabelText("文档复核依据")).toHaveTextContent("stale");
  });

  it("shows unknown/empty and count truncation without asserting completeness", () => {
    render(<RelatedDocuments project="demo" result={{ status: "unknown", reason: "reindex_required", references: [], truncated: true }} />);
    expect(screen.getByText("分析状态未知")).toBeInTheDocument();
    expect(screen.getByText("未发现已索引的文档引用。")).toBeInTheDocument();
    expect(screen.getByText(/已按数量截断/)).toBeInTheDocument();
  });

  it("allows retry after source errors and reports an unavailable evidence line", async () => {
    callToolMock.mockRejectedValueOnce(new Error("read failed")).mockResolvedValueOnce({ source: "# Guide", start_line: 1 });
    render(<RelatedDocuments project="demo" result={RESULT} />);
    fireEvent.click(screen.getByRole("button", { name: /阅读文档/ }));
    expect(await screen.findByRole("alert")).toHaveTextContent("read failed");
    fireEvent.click(screen.getByRole("button", { name: "重试读取" }));
    expect(await screen.findByText(/当前原文范围不包含证据行/)).toBeInTheDocument();
  });

  it("does not highlight an in-range line whose current source no longer matches indexed evidence", async () => {
    callToolMock.mockResolvedValue({ source: "# Guide\n\nunrelated text", start_line: 10 });
    const { container } = render(<RelatedDocuments project="demo" result={RESULT} />);
    fireEvent.click(screen.getByRole("button", { name: /阅读文档/ }));
    expect(await screen.findByText("当前原文与索引证据不一致，建议重新索引。")).toBeInTheDocument();
    expect(container.querySelector('[data-source-line="12"]')).not.toHaveAttribute("data-highlighted");
  });

  it("aborts a source read on project change and never renders the old source", async () => {
    let resolve!: (value: unknown) => void;
    callToolMock.mockImplementation(() => new Promise((done) => { resolve = done; }));
    const { rerender } = render(<RelatedDocuments project="demo" result={RESULT} />);
    fireEvent.click(screen.getByRole("button", { name: /阅读文档/ }));
    const signal = callToolMock.mock.calls[0][2] as AbortSignal;
    rerender(<RelatedDocuments project="other" result={RESULT} />);
    await act(async () => resolve({ source: "OLD SOURCE" }));
    expect(signal.aborted).toBe(true);
    expect(screen.queryByText("OLD SOURCE")).not.toBeInTheDocument();
  });
});

describe("RelatedDocumentsPanel", () => {
  it("shows loading, retries request errors, then renders references", async () => {
    callToolMock.mockRejectedValueOnce(new Error("offline")).mockResolvedValueOnce(RESULT);
    render(<RelatedDocumentsPanel project="demo" target="file:src/core.ts" />);
    expect(screen.getByRole("status")).toHaveTextContent("正在加载");
    expect(await screen.findByRole("alert")).toHaveTextContent("offline");
    fireEvent.click(screen.getByRole("button", { name: "重试文档引用" }));
    expect(await screen.findByText("Guide section")).toBeInTheDocument();
    expect(callToolMock).toHaveBeenLastCalledWith("get_related_documents", { project: "demo", target: "file:src/core.ts", limit: 20 }, expect.any(AbortSignal));
  });

  it("ignores a late response after target/project changes", async () => {
    let resolve!: (value: unknown) => void;
    callToolMock.mockImplementationOnce(() => new Promise((done) => { resolve = done; }))
      .mockResolvedValueOnce({ status: "ok", references: [] });
    const { rerender } = render(<RelatedDocumentsPanel project="demo" target="old" />);
    const signal = callToolMock.mock.calls[0][2] as AbortSignal;
    rerender(<RelatedDocumentsPanel project="other" target="new" />);
    await waitFor(() => expect(screen.getByText("未发现已索引的文档引用。")).toBeInTheDocument());
    await act(async () => resolve(RESULT));
    expect(signal.aborted).toBe(true);
    expect(screen.queryByText("Guide section")).not.toBeInTheDocument();
  });
});
