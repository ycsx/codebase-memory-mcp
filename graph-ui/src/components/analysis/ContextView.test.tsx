/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { fireEvent, render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";
import type { GraphData } from "../../lib/types";
import { ContextView } from "./ContextView";

const callToolMock = vi.hoisted(() => vi.fn());
vi.mock("../../api/rpc", () => ({ callTool: callToolMock }));

const DATA: GraphData = {
  nodes: [{ id: 1, x: 0, y: 0, z: 0, label: "Function", name: "saveOrder", qualified_name: "demo.saveOrder", file_path: "src/orders.ts", size: 1, color: "#fff" }],
  edges: [], total_nodes: 1,
};

describe("ContextView", () => {
  it("sends build_context options and shows candidates plus budget limits", async () => {
    callToolMock.mockResolvedValueOnce({
      evidence_level: "analysis",
      resolved_target: null,
      candidates: [{ name: "saveOrder", qualified_name: "demo.saveOrder", file_path: "src/orders.ts", rank: 1 }],
      evidence: [],
      budget: { requested_tokens: 900, estimated_tokens: 900, truncated: true },
      limitations: ["请选择唯一目标。"],
    });
    render(<ContextView project="demo" data={DATA} onOpenExplore={() => {}} />);
    fireEvent.change(screen.getByLabelText("上下文任务"), { target: { value: "修复订单校验" } });
    fireEvent.change(screen.getByLabelText("Token 预算"), { target: { value: "900" } });
    fireEvent.click(screen.getByRole("button", { name: "编译上下文" }));
    expect(await screen.findByText("候选目标")).toBeInTheDocument();
    expect(screen.getByText("saveOrder")).toBeInTheDocument();
    expect(screen.getByText("已裁剪")).toBeInTheDocument();
    expect(screen.getByText("请选择唯一目标。")).toBeInTheDocument();
    expect(callToolMock).toHaveBeenCalledWith("build_context", {
      project: "demo", task: "修复订单校验", token_budget: 900, evidence_level: "analysis",
      include_docs: false, include_tests: false,
    });
  });
});
