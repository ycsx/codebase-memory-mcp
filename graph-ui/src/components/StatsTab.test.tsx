/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { cleanup, fireEvent, render, screen, waitFor, act } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { StatsTab, IndexProgress } from "./StatsTab";
import { messages } from "../lib/i18n";

function mockProjectsFetch(extra?: (url: string, init?: RequestInit) => Response | undefined) {
  const fetchMock = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = String(input);
    const overridden = extra?.(url, init);
    if (overridden) return overridden;
    if (url === "/rpc") {
      return new Response(JSON.stringify({
        result: { content: [{ text: JSON.stringify({ projects: [] }) }] },
      }), { status: 200, headers: { "Content-Type": "application/json" } });
    }
    if (url.startsWith("/api/ui-config")) {
      return new Response(JSON.stringify({ lang: "en" }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    }
    if (url.startsWith("/api/browse")) {
      return new Response(JSON.stringify({
        path: "/home/dev",
        parent: "/home",
        dirs: ["alpha", "beta"],
        roots: ["/", "D:/"],
      }), { status: 200, headers: { "Content-Type": "application/json" } });
    }
    if (url === "/api/index") {
      return new Response(JSON.stringify({ status: "indexing", slot: 0 }), {
        status: 202,
        headers: { "Content-Type": "application/json" },
      });
    }
    return new Response("{}", { status: 200 });
  });
  vi.stubGlobal("fetch", fetchMock);
  return fetchMock;
}

describe("StatsTab index modal", () => {
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
  });

  it("submits a custom path and project name", async () => {
    let submitted: unknown = null;
    mockProjectsFetch((url, init) => {
      if (url === "/api/index") {
        submitted = JSON.parse(String(init?.body));
        return new Response(JSON.stringify({ status: "indexing", slot: 0 }), {
          status: 202,
          headers: { "Content-Type": "application/json" },
        });
      }
      return undefined;
    });

    render(<StatsTab onSelectProject={() => {}} />);
    fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));

    fireEvent.change(await screen.findByLabelText("Repository path"), {
      target: { value: "D:\\work\\信租风控通后端" },
    });
    fireEvent.change(screen.getByLabelText("Project ID (optional — permanent, cannot be renamed)"), {
      target: { value: "信租风控通后端" },
    });
    fireEvent.click(screen.getByRole("button", { name: "Index This Folder" }));

    await waitFor(() => {
      expect(submitted).toEqual({
        root_path: "D:\\work\\信租风控通后端",
        project_name: "信租风控通后端",
      });
    });
  });

  it("filters picker rows and exposes quick row indexing", async () => {
    mockProjectsFetch();

    render(<StatsTab onSelectProject={() => {}} />);
    fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));

    fireEvent.change(await screen.findByPlaceholderText("Filter folders"), {
      target: { value: "bet" },
    });

    expect(screen.queryByText("alpha")).not.toBeInTheDocument();
    expect(screen.getByText("beta")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Index beta" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Browse D:/" })).toBeInTheDocument();
  });

  it("navigates Windows breadcrumb segments to real drive paths", async () => {
    const fetchMock = mockProjectsFetch((url) => {
      if (url.startsWith("/api/browse")) {
        return new Response(JSON.stringify({
          path: "C:/Users/rap",
          parent: "C:/Users",
          dirs: ["Documents", "Downloads"],
          roots: ["C:/", "D:/"],
        }), { status: 200, headers: { "Content-Type": "application/json" } });
      }
      return undefined;
    });

    render(<StatsTab onSelectProject={() => {}} />);
    fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));

    /* No bogus unified "/" root crumb on a Windows drive path. */
    await screen.findByRole("button", { name: "C:" });
    expect(screen.queryByRole("button", { name: "/" })).not.toBeInTheDocument();

    /* Clicking the drive crumb browses to "C:/", not "/C:". */
    fireEvent.click(screen.getByRole("button", { name: "C:" }));
    await waitFor(() => {
      expect(fetchMock).toHaveBeenCalledWith("/api/browse?path=C%3A%2F");
    });

    /* Clicking a nested crumb browses to "C:/Users", not "/C:/Users". */
    fireEvent.click(screen.getByRole("button", { name: "Users" }));
    await waitFor(() => {
      expect(fetchMock).toHaveBeenCalledWith("/api/browse?path=C%3A%2FUsers");
    });
  });

  it("refreshes the folder list when a drive is typed into the path field", async () => {
    mockProjectsFetch((url) => {
      if (url.startsWith("/api/browse")) {
        const m = /[?&]path=([^&]*)/.exec(url);
        const path = m ? decodeURIComponent(m[1]) : "C:/Users/rap";
        const onD = path.replace(/\\/g, "/").toUpperCase().startsWith("D:");
        return new Response(JSON.stringify({
          path,
          parent: "C:/",
          dirs: onD ? ["projects", "games"] : ["Documents", "Downloads"],
          roots: ["C:/", "D:/"],
        }), { status: 200, headers: { "Content-Type": "application/json" } });
      }
      return undefined;
    });

    render(<StatsTab onSelectProject={() => {}} />);
    fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));

    /* Initial C: listing is shown. */
    expect(await screen.findByText("Documents")).toBeInTheDocument();

    /* Typing a different drive refreshes the listing to that drive (debounced). */
    fireEvent.change(await screen.findByLabelText("Repository path"), {
      target: { value: "D:/" },
    });

    expect(await screen.findByText("projects")).toBeInTheDocument();
    expect(screen.queryByText("Documents")).not.toBeInTheDocument();
  });

  it("replaces the meaningless '/' root with the drive on Windows", async () => {
    const fetchMock = mockProjectsFetch((url) => {
      if (url.startsWith("/api/browse")) {
        const m = /[?&]path=([^&]*)/.exec(url);
        const path = m ? decodeURIComponent(m[1]) : "C:/Users/rap";
        return new Response(JSON.stringify({
          path,
          parent: "C:/",
          dirs: ["Documents"],
          roots: ["/"], // older backend: no drive enumeration
        }), { status: 200, headers: { "Content-Type": "application/json" } });
      }
      return undefined;
    });

    render(<StatsTab onSelectProject={() => {}} />);
    fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));

    /* The bogus "/" quick-jump is gone; the current drive root is offered. */
    expect(await screen.findByRole("button", { name: "Browse C:/" })).toBeInTheDocument();
    expect(screen.queryByRole("button", { name: "Browse /" })).not.toBeInTheDocument();

    /* Clicking it browses to the drive root, not "/". */
    fireEvent.click(screen.getByRole("button", { name: "Browse C:/" }));
    await waitFor(() => {
      expect(fetchMock).toHaveBeenCalledWith("/api/browse?path=C%3A%2F");
    });
  });

  it("does not auto-refresh on POSIX when a path is typed", async () => {
    const fetchMock = mockProjectsFetch(); // browse returns POSIX path "/home/dev"

    render(<StatsTab onSelectProject={() => {}} />);
    fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));
    await screen.findByText("alpha"); // initial POSIX listing

    const browseCalls = () =>
      fetchMock.mock.calls.filter((c) => String(c[0]).startsWith("/api/browse")).length;
    const before = browseCalls();

    fireEvent.change(screen.getByLabelText("Repository path"), {
      target: { value: "/usr/local" },
    });

    /* Wait past the debounce window; a POSIX path must NOT trigger a re-browse. */
    await new Promise((r) => setTimeout(r, 400));
    expect(browseCalls()).toBe(before);
  });

  it("submits a remote SSH repository with branch and polling settings", async () => {
    let submitted: unknown = null;
    mockProjectsFetch((url, init) => {
      if (url === "/api/remote-index") {
        submitted = JSON.parse(String(init?.body));
        return new Response(JSON.stringify({ status: "indexing", slot: 0 }), {
          status: 202,
          headers: { "Content-Type": "application/json" },
        });
      }
      return undefined;
    });

    render(<StatsTab onSelectProject={() => {}} />);
    fireEvent.click(await screen.findByRole("button", { name: "Index your first repository" }));
    fireEvent.click(screen.getByRole("button", { name: "Git Remote" }));
    fireEvent.change(screen.getByLabelText("Repository URL"), {
      target: { value: "git@github.com:company/repository.git" },
    });
    fireEvent.change(screen.getByLabelText("Project ID (optional — permanent, cannot be renamed)"), {
      target: { value: "repository" },
    });
    fireEvent.change(screen.getByLabelText("Branch"), { target: { value: "main" } });
    fireEvent.change(screen.getByLabelText("Poll interval"), { target: { value: "5" } });
    fireEvent.click(screen.getByRole("button", { name: "Clone and Index" }));

    await waitFor(() => {
      expect(submitted).toEqual({
        remote_url: "git@github.com:company/repository.git",
        branch: "main",
        poll_interval_sec: 300,
        project_name: "repository",
      });
    });
  });

  it("shows the backend error when deleting an index fails", async () => {
    vi.spyOn(window, "confirm").mockReturnValue(true);
    mockProjectsFetch((url, init) => {
      if (url === "/rpc") {
        const request = JSON.parse(String(init?.body));
        const payload = request.params.name === "list_projects"
          ? { projects: [{ name: "locked-project", root_path: "C:/repo" }] }
          : { node_labels: [], edge_types: [] };
        return new Response(JSON.stringify({
          result: { content: [{ text: JSON.stringify(payload) }] },
        }), { status: 200, headers: { "Content-Type": "application/json" } });
      }
      if (url.startsWith("/api/project?")) {
        return new Response(JSON.stringify({ error: "database is in use by another process" }), {
          status: 409,
          headers: { "Content-Type": "application/json" },
        });
      }
      return undefined;
    });

    render(<StatsTab onSelectProject={() => {}} />);
    fireEvent.click(await screen.findByTitle("Delete index"));

    expect(await screen.findByText("database is in use by another process")).toBeInTheDocument();
  });
});

describe("IndexProgress", () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
  });

  it("polls and shows indexing in progress when active", async () => {
    const fetchMock = vi.fn().mockImplementation(() =>
      Promise.resolve({
        json: () => Promise.resolve([
          { slot: 1, status: "indexing", path: "/path/to/project1" }
        ])
      } as unknown as Response)
    );
    vi.stubGlobal("fetch", fetchMock);

    const onComplete = vi.fn();
    const onDismiss = vi.fn();
    render(<IndexProgress onComplete={onComplete} onDismiss={onDismiss} />);

    // Fast-forward initial poll
    await act(async () => {
      await vi.advanceTimersByTimeAsync(2000);
    });

    expect(fetchMock).toHaveBeenCalledWith("/api/index-status");
    expect(screen.getByText(messages.en.projects.indexingInProgress)).toBeInTheDocument();
    expect(screen.getByText("/path/to/project1")).toBeInTheDocument();
    expect(onComplete).not.toHaveBeenCalled();
    expect(onDismiss).not.toHaveBeenCalled();
  });

  it("stops polling and keeps a dismissible success message when indexing finishes", async () => {
    // Backend keeps finished jobs listed with status "done" (src/ui/http_server.c
    // handle_index_status only skips idle slots) — success is a "done" entry,
    // not an empty list.
    let mockData = [
      { slot: 1, status: "indexing", path: "/path/to/project" }
    ];
    const fetchMock = vi.fn().mockImplementation(() =>
      Promise.resolve({
        json: () => Promise.resolve(mockData)
      } as unknown as Response)
    );
    vi.stubGlobal("fetch", fetchMock);

    const onComplete = vi.fn();
    const onDismiss = vi.fn();
    render(<IndexProgress onComplete={onComplete} onDismiss={onDismiss} />);

    // First poll returns active
    await act(async () => {
      await vi.advanceTimersByTimeAsync(2000);
    });
    expect(onComplete).not.toHaveBeenCalled();

    // Indexing finishes
    mockData = [
      { slot: 1, status: "done", path: "/path/to/project" }
    ];

    await act(async () => {
      await vi.advanceTimersByTimeAsync(2000);
    });

    expect(onComplete).toHaveBeenCalledTimes(1);
    expect(screen.getByText(messages.en.projects.indexingComplete)).toBeInTheDocument();
    expect(screen.getByText("/path/to/project")).toBeInTheDocument();

    fireEvent.click(screen.getByRole("button", { name: messages.en.common.dismiss }));
    expect(onDismiss).toHaveBeenCalledTimes(1);
  });

  it("keeps waiting and does NOT complete while the jobs list is empty", async () => {
    // An empty list mid-index means the job is not visible (e.g. transient
    // backend state loss) — it must NOT be treated as successful completion.
    let mockData: { slot: number; status: string; path: string }[] = [];
    const fetchMock = vi.fn().mockImplementation(() =>
      Promise.resolve({
        json: () => Promise.resolve(mockData)
      } as unknown as Response)
    );
    vi.stubGlobal("fetch", fetchMock);

    const onComplete = vi.fn();
    const onDismiss = vi.fn();
    render(<IndexProgress onComplete={onComplete} onDismiss={onDismiss} />);

    // Two empty polls: still waiting, no premature completion
    await act(async () => {
      await vi.advanceTimersByTimeAsync(2000);
    });
    await act(async () => {
      await vi.advanceTimersByTimeAsync(2000);
    });
    expect(onComplete).not.toHaveBeenCalled();
    expect(fetchMock).toHaveBeenCalledTimes(2);

    // Job becomes visible and finishes — now completion fires
    mockData = [{ slot: 1, status: "done", path: "/path/to/project" }];
    await act(async () => {
      await vi.advanceTimersByTimeAsync(2000);
    });
    expect(onComplete).toHaveBeenCalledTimes(1);
    expect(onDismiss).not.toHaveBeenCalled();
  });

  it("renders error banner and does NOT complete when indexing fails with error status", async () => {
    const fetchMock = vi.fn().mockImplementation(() =>
      Promise.resolve({
        json: () => Promise.resolve([
          { slot: 1, status: "error", path: "/path/to/failed-project", error: "OOM Error" }
        ])
      } as unknown as Response)
    );
    vi.stubGlobal("fetch", fetchMock);

    const onComplete = vi.fn();
    const onDismiss = vi.fn();
    render(<IndexProgress onComplete={onComplete} onDismiss={onDismiss} />);

    await act(async () => {
      await vi.advanceTimersByTimeAsync(2000);
    });

    // Error banner should show up
    expect(screen.getByText(messages.en.projects.indexingFailed)).toBeInTheDocument();
    expect(screen.getByText("/path/to/failed-project")).toBeInTheDocument();
    expect(screen.getByText("OOM Error")).toBeInTheDocument();

    // Completion should not be reported for failed jobs.
    expect(onComplete).not.toHaveBeenCalled();

    // Click Dismiss button
    const dismissBtn = screen.getByRole("button", { name: messages.en.common.dismiss });
    expect(dismissBtn).toBeInTheDocument();

    await act(async () => {
      fireEvent.click(dismissBtn);
    });

    // onDismiss should be called after manual dismissal
    expect(onDismiss).toHaveBeenCalled();
  });
});
