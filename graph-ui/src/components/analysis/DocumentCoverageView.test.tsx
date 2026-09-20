/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { act, cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { DocumentCoverageView } from "./DocumentCoverageView";

const callToolMock = vi.hoisted(() => vi.fn());
vi.mock("../../api/rpc", () => ({ callTool: callToolMock }));
const EMPTY = { total: 0, offset: 0, returned: 0, has_more: false, items: [] };
afterEach(() => { cleanup(); callToolMock.mockReset(); });

describe("DocumentCoverageView", () => {
  it("renders loading, conservative labels and paginated code references", async () => {
    callToolMock.mockResolvedValueOnce({ ...EMPTY, total: 26, returned: 25, has_more: true, next_offset: 25,
      items: [{ file_path: "src/core.ts", status: "not_referenced", reference_count: 0 }],
    }).mockResolvedValueOnce({ ...EMPTY, total: 26, offset: 25, returned: 1,
      items: [{ file_path: "src/last.ts", status: "referenced", reference_count: 2 }],
    });
    render(<DocumentCoverageView project="demo" />);
    expect(screen.getByRole("status")).toHaveTextContent("正在加载");
    expect(await screen.findByText("src/core.ts")).toBeInTheDocument();
    expect(screen.getByText(/未被引用不等于缺少文档/)).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "上一页" })).toBeDisabled();
    fireEvent.click(screen.getByRole("button", { name: "下一页" }));
    expect(await screen.findByText("src/last.ts")).toBeInTheDocument();
    expect(callToolMock).toHaveBeenLastCalledWith("get_document_coverage", { project: "demo", view: "code", offset: 25, limit: 25 }, expect.any(AbortSignal));
    expect(screen.getByRole("button", { name: "下一页" })).toBeDisabled();
  });

  it("filters server-side and resets pagination when changing range", async () => {
    callToolMock.mockResolvedValue(EMPTY);
    render(<DocumentCoverageView project="demo" />);
    await screen.findByText("没有符合条件的已索引文件。");
    fireEvent.change(screen.getByLabelText("覆盖状态"), { target: { value: "not_referenced" } });
    await waitFor(() => expect(callToolMock).toHaveBeenLastCalledWith("get_document_coverage", {
      project: "demo", view: "code", offset: 0, limit: 25, status: "not_referenced",
    }, expect.any(AbortSignal)));
    fireEvent.change(screen.getByLabelText("覆盖范围"), { target: { value: "documents" } });
    await waitFor(() => expect(callToolMock).toHaveBeenLastCalledWith("get_document_coverage", {
      project: "demo", view: "documents", offset: 0, limit: 25,
    }, expect.any(AbortSignal)));
    fireEvent.change(screen.getByLabelText("覆盖状态"), { target: { value: "limited" } });
    await waitFor(() => expect(callToolMock).toHaveBeenLastCalledWith("get_document_coverage", {
      project: "demo", view: "documents", offset: 0, limit: 25, status: "limited",
    }, expect.any(AbortSignal)));
  });

  it("retries errors and drills into related references without assuming graph nodes", async () => {
    callToolMock.mockRejectedValueOnce(new Error("offline")).mockResolvedValueOnce({
      ...EMPTY, total: 1, returned: 1, items: [{ file_path: "src/core.ts", status: "referenced", reference_count: 1 }],
    }).mockResolvedValueOnce({ references: [] });
    render(<DocumentCoverageView project="demo" />);
    expect(await screen.findByRole("alert")).toHaveTextContent("offline");
    fireEvent.click(screen.getByRole("button", { name: "重试覆盖" }));
    fireEvent.click(await screen.findByRole("button", { name: "查看引用 src/core.ts" }));
    expect(await screen.findByText("未发现已索引的文档引用。")).toBeInTheDocument();
    expect(callToolMock).toHaveBeenLastCalledWith("get_related_documents", { project: "demo", target: "file:src/core.ts", limit: 20 }, expect.any(AbortSignal));
  });

  it("cancels stale requests on project change and unmount", async () => {
    let resolve!: (value: unknown) => void;
    callToolMock.mockImplementationOnce(() => new Promise((done) => { resolve = done; })).mockResolvedValueOnce(EMPTY);
    const { rerender, unmount } = render(<DocumentCoverageView project="old" />);
    const signal = callToolMock.mock.calls[0][2] as AbortSignal;
    rerender(<DocumentCoverageView project="new" />);
    await screen.findByText("没有符合条件的已索引文件。");
    await act(async () => resolve({ ...EMPTY, items: [{ file_path: "OLD", status: "referenced" }] }));
    expect(signal.aborted).toBe(true);
    expect(screen.queryByText("OLD")).not.toBeInTheDocument();
    const current = callToolMock.mock.calls[1][2] as AbortSignal;
    unmount();
    expect(current.aborted).toBe(true);
  });

  it("reads a limited document directly by exact indexed name", async () => {
    callToolMock.mockResolvedValueOnce(EMPTY).mockResolvedValueOnce({
      ...EMPTY, total: 1, returned: 1, items: [{ file_path: "docs/guide.md", qualified_name: "demo.docs.guide", status: "limited", reasons: ["unsupported_multiline_code"] }],
    }).mockResolvedValueOnce({ source: "# Current Guide", start_line: 1 });
    render(<DocumentCoverageView project="demo" />);
    await screen.findByText("没有符合条件的已索引文件。");
    fireEvent.change(screen.getByLabelText("覆盖范围"), { target: { value: "documents" } });
    fireEvent.click(await screen.findByRole("button", { name: "阅读文档 docs/guide.md" }));
    expect(await screen.findByText("# Current Guide", { exact: false })).toBeInTheDocument();
    expect(callToolMock).toHaveBeenLastCalledWith("get_code_snippet", { project: "demo", qualified_name: "demo.docs.guide" }, expect.any(AbortSignal));
    fireEvent.click(screen.getByRole("button", { name: "关闭文档原文" }));
    expect(screen.queryByLabelText("文档原文")).not.toBeInTheDocument();
  });

  it("refreshes from the first page after the indexed dataset shrinks", async () => {
    callToolMock.mockResolvedValueOnce({
      ...EMPTY, total: 26, returned: 25, has_more: true, next_offset: 25,
      items: [{ file_path: "src/first.ts", status: "referenced", reference_count: 1 }],
    }).mockResolvedValueOnce({
      ...EMPTY, total: 26, offset: 25, returned: 1,
      items: [{ file_path: "src/last.ts", status: "referenced", reference_count: 1 }],
    }).mockResolvedValueOnce({
      ...EMPTY, total: 25, returned: 25,
      items: [{ file_path: "src/first.ts", status: "referenced", reference_count: 1 }],
    });
    render(<DocumentCoverageView project="demo" />);
    await screen.findByText("src/first.ts");
    fireEvent.click(screen.getByRole("button", { name: "下一页" }));
    await screen.findByText("26–26 / 26");
    fireEvent.click(screen.getByRole("button", { name: "刷新覆盖" }));
    expect(await screen.findByText("1–25 / 25")).toBeInTheDocument();
    expect(callToolMock).toHaveBeenLastCalledWith("get_document_coverage", {
      project: "demo", view: "code", offset: 0, limit: 25,
    }, expect.any(AbortSignal));
    expect(screen.getByRole("button", { name: "上一页" })).toBeDisabled();
    expect(screen.queryByText("26–25 / 25")).not.toBeInTheDocument();
  });
});
