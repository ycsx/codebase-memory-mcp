/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { act, cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { DocumentReviewAction } from "./DocumentReviewAction";

const callToolMock = vi.hoisted(() => vi.fn());
vi.mock("../api/rpc", () => ({ callTool: callToolMock }));
afterEach(() => { cleanup(); callToolMock.mockReset(); });
const props = { project: "demo", source: "demo.guide", target: "demo.core", review: { state: "pending" as const, token: "server-token" } };

describe("DocumentReviewAction", () => {
  it("confirms and reopens only with returned server state and token", async () => {
    callToolMock.mockResolvedValueOnce({ state: "confirmed", token: "next-token", reviewed_at: "2026-09-20" })
      .mockResolvedValueOnce({ state: "pending", token: "third-token", reviewed_at: null });
    render(<DocumentReviewAction {...props} />);
    fireEvent.click(screen.getByRole("button", { name: "确认已复核" }));
    expect(await screen.findByText("已确认")).toBeInTheDocument();
    expect(callToolMock).toHaveBeenCalledWith("update_document_review", {
      project: "demo", source_qualified_name: "demo.guide", target_qualified_name: "demo.core", token: "server-token", action: "confirm",
    }, expect.any(AbortSignal));
    fireEvent.click(screen.getByRole("button", { name: "重新复核" }));
    expect(await screen.findByText("待复核")).toBeInTheDocument();
    expect(callToolMock).toHaveBeenLastCalledWith("update_document_review", {
      project: "demo", source_qualified_name: "demo.guide", target_qualified_name: "demo.core", token: "next-token", action: "reopen",
    }, expect.any(AbortSignal));
  });

  it("requires refresh after rejected writes and never claims success", async () => {
    const refresh = vi.fn();
    callToolMock.mockRejectedValue(new Error("stale_review_token"));
    render(<DocumentReviewAction {...props} onRefresh={refresh} />);
    fireEvent.click(screen.getByRole("button", { name: "确认已复核" }));
    expect(await screen.findByRole("alert")).toHaveTextContent("stale_review_token");
    expect(screen.getByRole("button", { name: "确认已复核" })).toBeDisabled();
    expect(screen.queryByText("已确认")).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "刷新引用" }));
    expect(refresh).toHaveBeenCalledOnce();
  });

  it("does not expose confirmation when server has no current token", () => {
    render(<DocumentReviewAction {...props} review={{ state: "unavailable", token: null }} />);
    expect(screen.getByText("暂不可确认")).toBeInTheDocument();
    expect(screen.queryByRole("button")).not.toBeInTheDocument();
  });

  it("cancels pending confirmation when switching projects", async () => {
    let resolve!: (value: unknown) => void;
    callToolMock.mockImplementation(() => new Promise((done) => { resolve = done; }));
    const { rerender } = render(<DocumentReviewAction {...props} />);
    fireEvent.click(screen.getByRole("button", { name: "确认已复核" }));
    const signal = callToolMock.mock.calls[0][2] as AbortSignal;
    expect(screen.getByRole("button", { name: "正在保存..." })).toBeDisabled();
    rerender(<DocumentReviewAction {...props} project="other" />);
    await act(async () => resolve({ state: "confirmed", token: "old-project" }));
    expect(signal.aborted).toBe(true);
    expect(screen.queryByText("已确认")).not.toBeInTheDocument();
    expect(screen.getByText("待复核")).toBeInTheDocument();
  });
});
