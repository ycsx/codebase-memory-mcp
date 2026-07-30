/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { GraphData } from "../../lib/types";
import { HotspotsView } from "./HotspotsView";
import { ImpactView } from "./ImpactView";
import { ImpactQueryView } from "./ImpactQueryView";

const callToolMock = vi.hoisted(() => vi.fn());
vi.mock("../../api/rpc", () => ({ callTool: callToolMock }));

const DATA: GraphData = {
  nodes: [
    {
      id: 1,
      x: 0,
      y: 0,
      z: 0,
      label: "Function",
      name: "saveOrder",
      qualified_name: "demo.src.orders.saveOrder",
      file_path: "src/orders.ts",
      size: 1,
      color: "#fff",
    },
    {
      id: 2,
      x: 0,
      y: 0,
      z: 0,
      label: "Function",
      name: "checkout",
      qualified_name: "demo.src.checkout.checkout",
      file_path: "src/checkout.ts",
      size: 1,
      color: "#fff",
    },
  ],
  edges: [{ source: 2, target: 1, type: "CALLS" }],
  total_nodes: 2,
};

describe("ImpactView", () => {
  afterEach(() => callToolMock.mockReset());

  it("uses HEAD as its branch-agnostic default and accepts a custom comparison", async () => {
    callToolMock.mockResolvedValue({
      changed_files: ["src/orders.ts"],
      changed_count: 1,
      impacted_symbols: [{ name: "saveOrder", label: "Function", file: "src/orders.ts" }],
      depth: 2,
    });

    render(<ImpactView project="demo" data={DATA} onOpenExplore={() => {}} />);

    await waitFor(() => {
      expect(callToolMock).toHaveBeenCalledWith("detect_changes", {
        project: "demo",
        since: "HEAD",
        scope: "impact",
        depth: 2,
      });
    });

    fireEvent.change(screen.getByLabelText("比较"), { target: { value: "custom" } });
    fireEvent.change(screen.getByLabelText("自定义 Git ref"), {
      target: { value: "origin/develop" },
    });
    fireEvent.click(screen.getByRole("button", { name: "分析" }));

    await waitFor(() => {
      expect(callToolMock).toHaveBeenLastCalledWith("detect_changes", {
        project: "demo",
        since: "origin/develop",
        scope: "impact",
        depth: 2,
      });
    });
  });
});

describe("HotspotsView", () => {
  afterEach(() => callToolMock.mockReset());

  it("requests JSON architecture hotspots and renders the fan-in ranking", async () => {
    callToolMock.mockResolvedValue({
      total_nodes: 2,
      total_edges: 1,
      hotspots: [
        { name: "saveOrder", qualified_name: "demo.src.orders.saveOrder", fan_in: 12 },
      ],
    });

    render(<HotspotsView project="demo" data={DATA} onOpenExplore={() => {}} />);

    expect(
      await screen.findByRole("button", { name: "在探索中定位 saveOrder" }),
    ).toBeInTheDocument();
    expect(screen.getAllByText("12").length).toBeGreaterThan(0);
    expect(callToolMock).toHaveBeenCalledWith("get_architecture", {
      project: "demo",
      aspects: ["hotspots"],
      format: "json",
    });
    expect(screen.getByText("12 条调用关系 · 1 个符号")).toBeInTheDocument();
  });
});

describe("ImpactQueryView", () => {
  afterEach(() => callToolMock.mockReset());

  it("disambiguates matches and renders a Chinese evidence-backed impact report", async () => {
    callToolMock
      .mockResolvedValueOnce({
        status: "candidates",
        query: "saveOrder",
        candidates: [
          {
            key: "demo.src.orders.saveOrder",
            name: "saveOrder",
            qualified_name: "demo.src.orders.saveOrder",
            label: "Function",
            file_path: "src/orders.ts",
            start_line: 10,
            end_line: 20,
            target_type: "symbol",
          },
        ],
      })
      .mockResolvedValueOnce({
        status: "analysis",
        query: "saveOrder",
        depth: 2,
        truncated: false,
        selected: {
          key: "demo.src.orders.saveOrder",
          name: "saveOrder",
          qualified_name: "demo.src.orders.saveOrder",
          label: "Function",
          file_path: "src/orders.ts",
          start_line: 10,
          end_line: 20,
          target_type: "symbol",
        },
        summary: {
          risk: "high",
          risk_label_zh: "高",
          text_zh: "高风险：修改「saveOrder」可能直接影响 1 个节点，并继续传导到 0 个间接节点，涉及 1 个文件。",
          affected_nodes: 1,
          direct_count: 1,
          indirect_count: 0,
          affected_files: 1,
          entry_points: 0,
          tests: 0,
          cross_service: false,
        },
        impacts: [
          {
            id: 2,
            name: "checkout",
            qualified_name: "demo.src.checkout.checkout",
            label: "Function",
            file_path: "src/checkout.ts",
            start_line: 3,
            end_line: 9,
            hop: 1,
            risk: "critical",
            is_test: false,
            is_entry_point: false,
            evidence: {
              relationship: "CALLS",
              source: "checkout",
              target: "saveOrder",
              confidence: 1,
            },
          },
        ],
        limitations: ["结果只覆盖已建立索引的静态关系。"],
      });

    render(<ImpactQueryView project="demo" data={DATA} onOpenExplore={() => {}} />);
    fireEvent.change(screen.getByLabelText("影响查询目标"), {
      target: { value: "saveOrder" },
    });
    fireEvent.click(screen.getByRole("button", { name: "查询影响" }));

    expect(
      await screen.findByRole("button", { name: /saveOrder/ }),
    ).toBeInTheDocument();
    expect(callToolMock).toHaveBeenLastCalledWith("explain_impact", {
      project: "demo",
      query: "saveOrder",
      depth: 2,
    });

    fireEvent.click(screen.getByRole("button", { name: /saveOrder/ }));

    expect(await screen.findByText(/修改「saveOrder」可能直接影响 1 个节点/)).toBeInTheDocument();
    expect(screen.getAllByText("调用").length).toBeGreaterThan(0);
    expect(screen.getAllByText("checkout").length).toBeGreaterThan(0);
    expect(callToolMock).toHaveBeenLastCalledWith("explain_impact", {
      project: "demo",
      query: "saveOrder",
      depth: 2,
      target: "demo.src.orders.saveOrder",
    });
  });
});
