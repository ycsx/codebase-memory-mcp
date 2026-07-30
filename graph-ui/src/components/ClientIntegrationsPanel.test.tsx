/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { ClientIntegrationsPanel } from "./ClientIntegrationsPanel";
import type { IntegrationPlan } from "../api/integrations";

const plan: IntegrationPlan = {
  type: "agent.install.plan.v1",
  agents_detected: ["codex", "cursor"],
  config_files_planned: ["C:/Users/dev/.codex/config.toml", "C:/Users/dev/.cursor/mcp.json"],
  instruction_files_planned: ["C:/Users/dev/.codex/AGENTS.md"],
  skill_files_planned: [],
  agent_files_planned: [],
  prompt_files_planned: [],
  hooks_planned: [{ agent: "Codex", path: "C:/Users/dev/.codex/hooks/session.ps1" }],
  writes_started: false,
  network_after_install: false,
  next_safe_command: "codebase-memory-mcp install -y",
};

function mockFetch() {
  return vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = String(input);
    if (url === "/api/ui-config") {
      return new Response(JSON.stringify({ lang: "en" }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    }
    if (url === "/api/integrations/apply") {
      return new Response(JSON.stringify({ status: "applied", plan }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    }
    if (url === "/api/integrations") {
      return new Response(JSON.stringify(plan), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    }
    return new Response(JSON.stringify({ init }), { status: 404 });
  });
}

describe("ClientIntegrationsPanel", () => {
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
  });

  it("shows detected clients and expands the read-only plan", async () => {
    vi.stubGlobal("fetch", mockFetch());
    render(<ClientIntegrationsPanel />);

    expect(await screen.findByText("Codex")).toBeInTheDocument();
    expect(screen.getByText("Cursor")).toBeInTheDocument();
    expect(screen.queryByText("C:/Users/dev/.codex/config.toml")).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole("button", { name: "Review planned changes" }));

    expect(screen.getByText("C:/Users/dev/.codex/config.toml")).toBeInTheDocument();
    expect(screen.getByText("C:/Users/dev/.codex/hooks/session.ps1")).toBeInTheDocument();
    expect(screen.getByText("Dry-run preview. No files have been changed.")).toBeInTheDocument();
  });

  it("requires an in-page confirmation before applying", async () => {
    const fetchMock = mockFetch();
    vi.stubGlobal("fetch", fetchMock);
    render(<ClientIntegrationsPanel />);

    fireEvent.click(await screen.findByRole("button", { name: "Apply integrations" }));
    expect(screen.getByRole("dialog")).toBeInTheDocument();
    expect(fetchMock.mock.calls.some(([url]) => String(url) === "/api/integrations/apply")).toBe(false);

    fireEvent.click(screen.getByRole("button", { name: "Confirm and apply" }));

    await waitFor(() => {
      const call = fetchMock.mock.calls.find(([url]) => String(url) === "/api/integrations/apply");
      expect(call?.[1]).toMatchObject({
        method: "POST",
        body: JSON.stringify({ confirm: true }),
      });
    });
    expect(await screen.findByText("Client integrations applied")).toBeInTheDocument();
  });
});
