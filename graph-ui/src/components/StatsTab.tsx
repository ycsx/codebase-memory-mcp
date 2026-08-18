import { useMemo, useState, useCallback, useEffect, useRef } from "react";
import { ScrollArea } from "@/components/ui/scroll-area";
import { Checkbox } from "@/components/ui/checkbox";
import {
  AlertCircle,
  ArrowUpRight,
  CheckCircle2,
  Clock3,
  Database,
  FolderGit2,
  FolderOpen,
  GitMerge,
  Layers3,
  LoaderCircle,
  Network,
  Plus,
  RefreshCw,
  Trash2,
} from "lucide-react";
import { callTool } from "../api/rpc";
import { useProjects } from "../hooks/useProjects";
import { colorForLabel } from "../lib/colors";
import { useUiMessages } from "../lib/i18n";
import { compareNames } from "../lib/sort";

interface StatsTabProps {
  onSelectProject: (project: string) => void;
}

function formatIndexedAt(value?: string): string | null {
  if (!value) return null;
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return null;
  return date.toLocaleString([], {
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    hour12: false,
  });
}

function IndexedAt({
  value,
  label,
  missingLabel,
}: {
  value?: string;
  label: string;
  missingLabel: string;
}) {
  const formatted = formatIndexedAt(value);
  return (
    <div
      className="inline-flex min-w-0 items-center gap-2 whitespace-nowrap text-[12px] text-foreground/45"
      title={value || missingLabel}
    >
      <Clock3 className="h-4 w-4 shrink-0 text-foreground/35" aria-hidden="true" />
      <span>{label}</span>
      {formatted ? (
        <time dateTime={value} className="font-mono tabular-nums text-foreground/65">
          {formatted}
        </time>
      ) : (
        <span className="text-foreground/30">{missingLabel}</span>
      )}
    </div>
  );
}

/* ── Glowy health dot ───────────────────────────────────── */

function HealthDot({ name }: { name: string }) {
  const t = useUiMessages();
  const [status, setStatus] = useState<"loading" | "healthy" | "corrupt" | "missing">("loading");
  const [info, setInfo] = useState("");

  useEffect(() => {
    fetch(`/api/project-health?name=${encodeURIComponent(name)}`)
      .then((r) => r.json())
      .then((d) => {
        setStatus(d.status ?? "corrupt");
        if (d.nodes !== undefined) {
          const sizeMB = ((d.size_bytes ?? 0) / 1024 / 1024).toFixed(1);
          setInfo(`${d.nodes.toLocaleString()} nodes, ${d.edges.toLocaleString()} edges, ${sizeMB} MB`);
        } else if (d.reason) {
          setInfo(d.reason);
        }
      })
      .catch(() => setStatus("corrupt"));
  }, [name]);

  const dotColor =
    status === "healthy" ? "#34d399" :
    status === "missing" ? "#fbbf24" :
    status === "corrupt" ? "#f87171" : "#555";

  const label =
    status === "healthy" ? t.projects.healthHealthy :
    status === "missing" ? t.projects.healthMissing :
    status === "corrupt" ? t.projects.healthCorrupt : t.projects.healthChecking;

  return (
    <div className="group relative inline-flex items-center">
      {/* Glow layer */}
      <span
        className="absolute w-3 h-3 rounded-full animate-pulse opacity-40 blur-[3px]"
        style={{ backgroundColor: dotColor }}
      />
      {/* Dot */}
      <span
        className="relative w-[8px] h-[8px] rounded-full"
        style={{ backgroundColor: dotColor, boxShadow: `0 0 6px ${dotColor}80` }}
      />
      {/* Tooltip */}
      <div className="absolute bottom-full left-1/2 -translate-x-1/2 mb-3 hidden group-hover:block z-20 pointer-events-none">
        <div className="bg-[#0b1920] border border-border/50 rounded-lg px-3 py-2 text-[11px] whitespace-nowrap shadow-xl">
          <p className="font-medium" style={{ color: dotColor }}>{label}</p>
          {info && <p className="text-foreground/35 text-[10px] mt-0.5">{info}</p>}
        </div>
      </div>
    </div>
  );
}

interface RemoteProjectInfo {
  managed: boolean;
  remote_url?: string;
  branch?: string;
  poll_interval_sec?: number;
}

function RemoteProjectControls({ project }: { project: string }) {
  const t = useUiMessages();
  const [info, setInfo] = useState<RemoteProjectInfo | null>(null);

  useEffect(() => {
    let cancelled = false;
    fetch(`/api/remote-info?project=${encodeURIComponent(project)}`)
      .then(async (res) => {
        const data = await res.json();
        if (!res.ok) throw new Error(data.error ?? "Failed");
        return data as RemoteProjectInfo;
      })
      .then((data) => { if (!cancelled) setInfo(data); })
      .catch(() => { if (!cancelled) setInfo({ managed: false }); });
    return () => { cancelled = true; };
  }, [project]);

  if (!info) return null;
  if (!info.managed) {
    return (
      <div className="mt-4 flex items-center gap-2 border-t border-white/[0.06] pt-3 text-[12px] text-foreground/55">
        <FolderOpen className="h-4 w-4 text-amber-300/80" aria-hidden="true" />
        <span className="font-medium">{t.projects.localSource}</span>
      </div>
    );
  }

  const pollMinutes = Math.max(1, Math.round((info.poll_interval_sec ?? 300) / 60));
  return (
    <div className="mt-4 flex flex-wrap items-center gap-x-3 gap-y-2 border-t border-white/[0.06] pt-3 text-[12px]">
      <span
        className="inline-flex min-w-0 items-center gap-2 text-foreground/55"
        title={info.remote_url}
      >
        <FolderGit2 className="h-4 w-4 shrink-0 text-primary" aria-hidden="true" />
        <span className="font-medium text-primary">{t.projects.gitRemote}</span>
        <span className="max-w-[240px] truncate font-mono text-foreground/65">{info.branch}</span>
        <span className="text-foreground/35">{t.projects.pollEvery(pollMinutes)}</span>
      </span>
    </div>
  );
}

/* ── ADR button + modal ─────────────────────────────────── */

function AdrButton({ project }: { project: string }) {
  const t = useUiMessages();
  const [hasAdr, setHasAdr] = useState<boolean | null>(null);
  const [open, setOpen] = useState(false);
  const [content, setContent] = useState("");
  const [saving, setSaving] = useState(false);
  const [updatedAt, setUpdatedAt] = useState("");

  const fetchAdr = useCallback(async () => {
    try {
      const res = await fetch(`/api/adr?project=${encodeURIComponent(project)}`);
      const data = await res.json();
      setHasAdr(data.has_adr ?? false);
      if (data.content) setContent(data.content);
      if (data.updated_at) setUpdatedAt(data.updated_at);
    } catch { setHasAdr(false); }
  }, [project]);

  useEffect(() => { fetchAdr(); }, [fetchAdr]);

  const save = async () => {
    setSaving(true);
    try {
      await fetch("/api/adr", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ project, content }),
      });
      await fetchAdr();
      setOpen(false);
    } catch { /* ignore */ }
    finally { setSaving(false); }
  };

  if (hasAdr === null) return null;

  return (
    <>
      <button
        onClick={() => { setOpen(true); fetchAdr(); }}
        className={`inline-flex h-9 items-center rounded-lg px-3 text-[12px] font-semibold transition-all ${
          hasAdr
            ? "bg-accent/15 text-accent hover:bg-accent/25"
            : "bg-white/[0.03] text-foreground/25 hover:text-foreground/40 hover:bg-white/[0.06]"
        }`}
      >
        {hasAdr ? "ADR" : "+ ADR"}
      </button>

      {open && (
        <div className="fixed inset-0 z-50 flex items-center justify-center" onClick={() => setOpen(false)}>
          <div className="absolute inset-0 bg-black/60 backdrop-blur-sm" />
          <div className="relative bg-[#0e2028] border border-border/40 rounded-2xl p-6 w-full max-w-2xl shadow-2xl max-h-[80vh] flex flex-col" onClick={(e) => e.stopPropagation()}>
            <div className="flex items-center justify-between mb-4">
              <div>
                <h3 className="text-[15px] font-semibold text-foreground/90">{t.adr.title}</h3>
                <p className="text-[11px] text-foreground/30 font-mono mt-0.5">{project}</p>
              </div>
              <button onClick={() => setOpen(false)} className="text-foreground/20 hover:text-foreground/50 text-[16px] p-1">×</button>
            </div>
            {updatedAt && (
              <p className="text-[10px] text-foreground/20 mb-3">{t.adr.lastUpdated}: {updatedAt}</p>
            )}
            <textarea
              value={content}
              onChange={(e) => setContent(e.target.value)}
              placeholder={"# Architecture Decision Record\n\n## Context\n...\n\n## Decision\n...\n\n## Consequences\n..."}
              className="flex-1 min-h-[300px] bg-white/[0.03] border border-white/[0.06] rounded-xl px-4 py-3 text-[12px] text-foreground font-mono placeholder-foreground/15 outline-none focus:border-primary/30 resize-none leading-relaxed"
            />
            <div className="flex justify-end gap-2 mt-4">
              {hasAdr && (
                <button
                  onClick={async () => {
                    setContent(""); await save();
                  }}
                  className="px-3 py-2 rounded-lg text-[12px] text-destructive/60 hover:text-destructive hover:bg-destructive/10 font-medium transition-all"
                >
                  {t.common.delete}
                </button>
              )}
              <button onClick={() => setOpen(false)} className="px-4 py-2 rounded-lg text-[12px] text-foreground/40 hover:bg-white/[0.04] font-medium transition-all">{t.common.cancel}</button>
              <button onClick={save} disabled={saving} className="px-4 py-2 rounded-lg bg-primary/20 hover:bg-primary/30 text-primary text-[12px] font-medium transition-all disabled:opacity-30">
                {saving ? t.common.saving : t.common.save}
              </button>
            </div>
          </div>
        </div>
      )}
    </>
  );
}

/* ── Create Index Modal ─────────────────────────────────── */

function joinPath(base: string, dir: string): string {
  if (!base || base === "/") return `/${dir}`;
  if (/^[A-Za-z]:[\\/]?$/.test(base)) return `${base[0]}:/${dir}`;
  const slash = base.includes("\\") && !base.includes("/") ? "\\" : "/";
  return `${base.replace(/[\\/]+$/, "")}${slash}${dir}`;
}

function CreateIndexModal({ onClose, onCreated }: { onClose: () => void; onCreated: () => void }) {
  const t = useUiMessages();
  const [sourceMode, setSourceMode] = useState<"local" | "remote">("local");
  const [currentPath, setCurrentPath] = useState("");
  const [dirs, setDirs] = useState<string[]>([]);
  const [roots, setRoots] = useState<string[]>(["/"]);
  const [parentPath, setParentPath] = useState("");
  const [projectName, setProjectName] = useState("");
  const [remoteUrl, setRemoteUrl] = useState("");
  const [remoteBranch, setRemoteBranch] = useState("main");
  const [pollMinutes, setPollMinutes] = useState(5);
  const [filter, setFilter] = useState("");
  const [activeIndex, setActiveIndex] = useState(0);
  const [loading, setLoading] = useState(false);
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const filterRef = useRef<HTMLInputElement>(null);
  /* Path whose listing is currently shown. Lets the typed-path effect skip a
   * redundant re-fetch after browse() sets currentPath itself. */
  const lastBrowsedRef = useRef<string>("");

  const browse = useCallback(async (path?: string, opts?: { silent?: boolean }) => {
    const silent = opts?.silent ?? false;
    if (!silent) setLoading(true);
    setError(null);
    try {
      const q = path ? `?path=${encodeURIComponent(path)}` : "";
      const res = await fetch(`/api/browse${q}`);
      const data = await res.json();
      if (data.error) throw new Error(data.error);
      lastBrowsedRef.current = data.path ?? "";
      setCurrentPath(data.path ?? "");
      setDirs((data.dirs ?? []).sort(compareNames));
      setRoots(data.roots ?? ["/"]);
      setParentPath(data.parent ?? "/");
    } catch (e) {
      /* Silent (typed-path) refreshes keep the last good listing instead of
       * flashing an error while the user is still typing a path. */
      if (!silent) setError(e instanceof Error ? e.message : "Browse failed");
    }
    finally { if (!silent) setLoading(false); }
  }, []);

  useEffect(() => { if (sourceMode === "local") void browse(); }, [browse, sourceMode]);
  useEffect(() => { if (sourceMode === "local") filterRef.current?.focus(); }, [sourceMode]);

  /* Windows only: when the user types a drive path into the Repository path
   * field, refresh the folder listing to match (debounced). On Windows, typing
   * is the way to switch drives, and without this the breadcrumb and path box
   * updated but the directory list stayed stale (e.g. typing "D:/" still showed
   * the previous drive's folders). POSIX navigation is left unchanged. */
  useEffect(() => {
    if (!currentPath || currentPath === lastBrowsedRef.current) return;
    if (!/^[A-Za-z]:/.test(currentPath.replace(/\\/g, "/"))) return;
    const id = setTimeout(() => { void browse(currentPath, { silent: true }); }, 350);
    return () => clearTimeout(id);
  }, [currentPath, browse]);

  const filteredDirs = useMemo(() => {
    const q = filter.trim().toLowerCase();
    if (!q) return dirs;
    return dirs.filter((d) => d.toLowerCase().includes(q));
  }, [dirs, filter]);

  useEffect(() => { setActiveIndex(0); }, [filter, currentPath]);

  const submitLocal = async (path = currentPath) => {
    if (!path) return;
    setSubmitting(true); setError(null);
    try {
      const body: { root_path: string; project_name?: string } = { root_path: path };
      if (projectName.trim()) body.project_name = projectName.trim();
      const res = await fetch("/api/index", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error ?? "Failed");
      onCreated(); onClose();
    } catch (e) { setError(e instanceof Error ? e.message : "Failed"); }
    finally { setSubmitting(false); }
  };

  const submitRemote = async () => {
    if (!remoteUrl.trim() || !remoteBranch.trim()) return;
    setSubmitting(true); setError(null);
    try {
      const body: {
        remote_url: string;
        branch: string;
        poll_interval_sec: number;
        project_name?: string;
      } = {
        remote_url: remoteUrl.trim(),
        branch: remoteBranch.trim(),
        poll_interval_sec: pollMinutes * 60,
      };
      if (projectName.trim()) body.project_name = projectName.trim();
      const res = await fetch("/api/remote-index", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error ?? "Failed");
      onCreated(); onClose();
    } catch (e) { setError(e instanceof Error ? e.message : "Failed"); }
    finally { setSubmitting(false); }
  };

  const onFilterKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === "ArrowDown") {
      e.preventDefault();
      setActiveIndex((i) => Math.min(i + 1, Math.max(filteredDirs.length - 1, 0)));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      setActiveIndex((i) => Math.max(i - 1, 0));
    } else if (e.key === "Enter" && filteredDirs.length > 0) {
      e.preventDefault();
      const dir = filteredDirs.length === 1 ? filteredDirs[0] : filteredDirs[activeIndex];
      if (filteredDirs.length === 1) void submitLocal(joinPath(currentPath, dir));
      else void browse(joinPath(currentPath, dir));
    }
  };

  /* Breadcrumb segments */
  const displayPath = currentPath.replace(/\\/g, "/");
  const segments = displayPath.split("/").filter(Boolean);
  /* A Windows drive path ("C:/Users/rap") has no unified "/" root — its first
   * segment is the drive letter. Build crumb targets accordingly so clicking a
   * segment navigates to a real directory instead of a bogus "/C:/..." path
   * that the backend rejects as "not a directory". */
  const isWinPath = /^[A-Za-z]:$/.test(segments[0] ?? "");
  const crumbPath = (i: number): string => {
    const parts = segments.slice(0, i + 1);
    if (isWinPath) return parts.length === 1 ? `${parts[0]}/` : parts.join("/");
    return "/" + parts.join("/");
  };

  /* Root/drive quick-jump buttons. On Windows the POSIX "/" root is meaningless
   * — browsing it returns an empty listing — so drop it and offer drive roots
   * instead. An older backend may not enumerate drives, so always include the
   * current drive; other drives stay reachable by typing a path. */
  const displayRoots = (() => {
    if (!isWinPath) return roots;
    const drives = Array.from(new Set(
      roots.filter((r) => /^[A-Za-z]:[\\/]?$/.test(r)).map((r) => `${r[0].toUpperCase()}:/`),
    ));
    const curRoot = `${displayPath[0].toUpperCase()}:/`;
    if (!drives.includes(curRoot)) drives.unshift(curRoot);
    return drives;
  })();

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center" onClick={onClose}>
      <div className="absolute inset-0 bg-black/60 backdrop-blur-sm" />
      <div
        className="relative bg-[#0e2028] border border-border/40 rounded-2xl w-full max-w-2xl shadow-2xl flex flex-col overflow-hidden"
        style={sourceMode === "local" ? { height: "min(82vh, 680px)" } : { maxHeight: "min(82vh, 520px)" }}
        onClick={(e) => e.stopPropagation()}
      >
        {/* Header */}
        <div className="px-5 pt-5 pb-3 shrink-0">
          <h3 className="text-[15px] font-semibold text-foreground/90 mb-1">{t.index.createIndex}</h3>
          <p className="text-[12px] text-foreground/30">
            {sourceMode === "local" ? t.index.instructions : t.index.remoteInstructions}
          </p>
          <div className="inline-flex mt-3 rounded-lg bg-white/[0.04] p-1">
            <button
              onClick={() => setSourceMode("local")}
              aria-pressed={sourceMode === "local"}
              className={`px-3 py-1.5 rounded-md text-[11px] font-medium transition-all ${sourceMode === "local" ? "bg-primary/20 text-primary" : "text-foreground/35 hover:text-foreground/60"}`}
            >
              {t.index.localSource}
            </button>
            <button
              onClick={() => setSourceMode("remote")}
              aria-pressed={sourceMode === "remote"}
              className={`px-3 py-1.5 rounded-md text-[11px] font-medium transition-all ${sourceMode === "remote" ? "bg-primary/20 text-primary" : "text-foreground/35 hover:text-foreground/60"}`}
            >
              {t.index.remoteSource}
            </button>
          </div>
        </div>

        <div className="px-5 pb-3 grid grid-cols-[1fr_220px] gap-3 shrink-0">
          <label className="block">
            <span className="block text-[10px] uppercase tracking-widest text-foreground/25 mb-1">
              {sourceMode === "local" ? t.index.repositoryPath : t.index.repositoryUrl}
            </span>
            {sourceMode === "local" ? (
              <input
                aria-label={t.index.repositoryPath}
                value={currentPath}
                onChange={(e) => setCurrentPath(e.target.value)}
                onKeyDown={(e) => { if (e.key === "Enter" && /^[A-Za-z]:/.test(currentPath.replace(/\\/g, "/"))) { e.preventDefault(); void browse(currentPath); } }}
                className="w-full bg-white/[0.04] border border-white/[0.06] rounded-lg px-3 py-2 text-[12px] text-foreground font-mono outline-none focus:border-primary/40"
              />
            ) : (
              <input
                aria-label={t.index.repositoryUrl}
                value={remoteUrl}
                placeholder={t.index.repositoryUrlPlaceholder}
                onChange={(e) => setRemoteUrl(e.target.value)}
                className="w-full bg-white/[0.04] border border-white/[0.06] rounded-lg px-3 py-2 text-[12px] text-foreground font-mono outline-none focus:border-primary/40 placeholder:text-foreground/20"
              />
            )}
          </label>
          <label className="block">
            <span className="block text-[10px] uppercase tracking-widest text-foreground/25 mb-1">{t.index.projectName}</span>
            <input
              aria-label={t.index.projectName}
              value={projectName}
              placeholder={t.index.projectNamePlaceholder}
              onChange={(e) => setProjectName(e.target.value)}
              className="w-full bg-white/[0.04] border border-white/[0.06] rounded-lg px-3 py-2 text-[12px] text-foreground outline-none focus:border-primary/40 placeholder:text-foreground/20"
            />
            <span className="block text-[10px] text-foreground/25 mt-1">{t.index.projectNameHelp}</span>
          </label>
        </div>

        {sourceMode === "remote" && (
          <div className="px-5 pb-4 grid grid-cols-2 gap-3 shrink-0">
            <label className="block">
              <span className="block text-[10px] uppercase tracking-widest text-foreground/25 mb-1">{t.index.branch}</span>
              <input
                aria-label={t.index.branch}
                value={remoteBranch}
                onChange={(e) => setRemoteBranch(e.target.value)}
                className="w-full bg-white/[0.04] border border-white/[0.06] rounded-lg px-3 py-2 text-[12px] text-foreground font-mono outline-none focus:border-primary/40"
              />
            </label>
            <label className="block">
              <span className="block text-[10px] uppercase tracking-widest text-foreground/25 mb-1">{t.index.pollInterval}</span>
              <div className="flex items-center gap-2">
                <input
                  aria-label={t.index.pollInterval}
                  type="number"
                  min={1}
                  max={60}
                  value={pollMinutes}
                  onChange={(e) => setPollMinutes(Math.max(1, Math.min(60, Number(e.target.value) || 1)))}
                  className="w-full bg-white/[0.04] border border-white/[0.06] rounded-lg px-3 py-2 text-[12px] text-foreground outline-none focus:border-primary/40"
                />
                <span className="text-[11px] text-foreground/25 whitespace-nowrap">{t.index.minutes}</span>
              </div>
            </label>
          </div>
        )}

        {sourceMode === "local" && <div className="px-5 pb-3 flex items-center gap-2 shrink-0">
          <input
            ref={filterRef}
            value={filter}
            placeholder={t.index.filterFolders}
            onChange={(e) => setFilter(e.target.value)}
            onKeyDown={onFilterKeyDown}
            className="flex-1 bg-white/[0.04] border border-white/[0.06] rounded-lg px-3 py-2 text-[12px] text-foreground outline-none focus:border-primary/40 placeholder:text-foreground/20"
          />
          <div className="flex items-center gap-1">
            {displayRoots.map((root) => (
              <button
                key={root}
                aria-label={t.index.browseRoot(root)}
                onClick={() => browse(root)}
                className="px-2.5 py-2 rounded-lg bg-white/[0.04] hover:bg-white/[0.07] text-[11px] text-foreground/45 font-mono transition-all"
              >
                {root}
              </button>
            ))}
          </div>
        </div>}

        {/* Breadcrumb */}
        {sourceMode === "local" && <div className="px-5 py-2 border-y border-border/20 flex items-center gap-0.5 overflow-x-auto text-[11px] shrink-0">
          {!isWinPath && (
            <button onClick={() => browse("/")} className="text-primary/60 hover:text-primary shrink-0 transition-colors">/</button>
          )}
          {segments.map((seg, i) => (
            <span key={i} className="flex items-center gap-0.5 shrink-0">
              {(i > 0 || !isWinPath) && <span className="text-foreground/15">/</span>}
              <button
                onClick={() => browse(crumbPath(i))}
                className={`transition-colors ${i === segments.length - 1 ? "text-foreground/70 font-medium" : "text-primary/50 hover:text-primary"}`}
              >
                {seg}
              </button>
            </span>
          ))}
        </div>}

        {/* Directory list */}
        {sourceMode === "local" ? <ScrollArea className="flex-1 min-h-0">
          <div className="px-2 py-1">
            {/* Go up */}
            {currentPath !== "/" && (
              <button
                onClick={() => browse(parentPath)}
                className="flex items-center gap-2 w-full text-left px-3 py-2 rounded-lg hover:bg-white/[0.04] text-[12px] text-foreground/40 transition-colors"
              >
                <span className="text-foreground/20">↑</span>
                <span>..</span>
              </button>
            )}
            {loading ? (
              <p className="text-foreground/20 text-[12px] text-center py-8">{t.common.loading}</p>
            ) : filteredDirs.length === 0 ? (
              <p className="text-foreground/15 text-[12px] text-center py-8">{t.index.noSubdirectories}</p>
            ) : (
              filteredDirs.map((d, i) => (
                <div
                  key={d}
                  className={`flex items-center gap-2 rounded-lg px-3 py-1.5 text-[12px] transition-colors group ${
                    i === activeIndex ? "bg-white/[0.05]" : "hover:bg-white/[0.04]"
                  }`}
                >
                  <button
                    aria-label={t.index.browseRoot(d)}
                    onClick={() => browse(joinPath(currentPath, d))}
                    className="flex min-w-0 flex-1 items-center gap-2 text-left text-foreground/60"
                  >
                    <span className="text-foreground/20 group-hover:text-foreground/40">/</span>
                    <span className="truncate">{d}</span>
                  </button>
                  <button
                    aria-label={t.index.indexDirectory(d)}
                    onClick={() => submitLocal(joinPath(currentPath, d))}
                    disabled={submitting}
                    className="opacity-100 sm:opacity-0 sm:group-hover:opacity-100 px-2 py-1 rounded-md bg-primary/15 hover:bg-primary/25 text-primary text-[10px] font-medium transition-all disabled:opacity-30"
                  >
                    {t.index.indexThisFolder}
                  </button>
                </div>
              ))
            )}
          </div>
        </ScrollArea> : <div className="px-5 py-4 border-y border-border/20">
          <div className="py-1">
            <p className="text-[12px] text-foreground/55 font-medium">{remoteBranch || "main"}</p>
            <p className="text-[11px] text-foreground/25 font-mono mt-1 break-all">{remoteUrl || t.index.repositoryUrlPlaceholder}</p>
            <p className="text-[10px] text-foreground/20 mt-3">{t.index.pollInterval}: {pollMinutes} {t.index.minutes}</p>
          </div>
        </div>}

        {/* Footer */}
        <div className="px-5 py-4 border-t border-border/20 shrink-0">
          {error && <div className="rounded-lg bg-destructive/10 border border-destructive/20 px-3 py-2 mb-3"><p className="text-destructive text-[11px]">{error}</p></div>}
          <div className="flex items-center justify-between">
            <p className="text-[11px] text-foreground/25 font-mono truncate max-w-[250px]">{sourceMode === "local" ? currentPath : remoteUrl}</p>
            <div className="flex gap-2 shrink-0">
              <button onClick={onClose} className="px-3 py-2 rounded-lg text-[12px] text-foreground/40 hover:bg-white/[0.04] font-medium transition-all">{t.common.cancel}</button>
              <button onClick={() => sourceMode === "local" ? submitLocal() : submitRemote()} disabled={submitting || (sourceMode === "local" ? !currentPath : !remoteUrl.trim() || !remoteBranch.trim())} className="px-4 py-2 rounded-lg bg-primary/20 hover:bg-primary/30 text-primary text-[12px] font-medium transition-all disabled:opacity-30">
                {submitting ? t.index.starting : sourceMode === "local" ? t.index.indexThisFolder : t.index.cloneAndIndex}
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}

/* ── Index Progress ─────────────────────────────────────── */

export function IndexProgress({
  onComplete,
  onDismiss,
}: {
  onComplete: () => void;
  onDismiss: () => void;
}) {
  const t = useUiMessages();
  const [jobs, setJobs] = useState<{ slot: number; status: string; path: string; error?: string }[]>([]);
  const [hasActive, setHasActive] = useState(true);
  const [statusError, setStatusError] = useState<string | null>(null);
  useEffect(() => {
    if (!hasActive) return;
    const poll = setInterval(async () => {
      try {
        const response = await fetch("/api/index-status");
        if (response.ok === false) {
          throw new Error(`Index status request failed (${response.status})`);
        }
        const data: unknown = await response.json();
        if (!Array.isArray(data)) {
          throw new Error("Index status response was invalid");
        }
        setJobs(data);
        const stillIndexing = data.some((j: { status: string }) => j.status === "indexing");
        /* Empty list = job not visible: the backend keeps finished jobs listed
           as "done"/"error", so [] mid-index only happens on transient state
           loss (e.g. server restart) — keep polling, don't treat as done. */
        if (data.length > 0 && !stillIndexing) {
          setHasActive(false);
          const hasErrors = data.some((j: { status: string }) => j.status === "error");
          if (!hasErrors) {
            onComplete();
          }
        }
      } catch (error) {
        console.error("[IndexProgress] Poll failed:", error);
        setStatusError(error instanceof Error ? error.message : "Index status request failed");
        setHasActive(false);
      }
    }, 2000);
    return () => clearInterval(poll);
  }, [onComplete, hasActive]);

  const active = statusError ? [] : jobs.filter((j) => j.status === "indexing");
  const completed = jobs.filter((j) => j.status === "done");
  const errors = jobs.filter((j) => j.status === "error");

  if (!statusError && active.length === 0 && completed.length === 0 && errors.length === 0) return null;

  return (
    <div className="rounded-xl border border-primary/20 bg-primary/5 p-4 mb-6">
      {active.map((j) => (
        <div key={j.slot} className="flex items-center gap-3">
          <div className="w-4 h-4 border-2 border-primary/30 border-t-primary rounded-full animate-spin shrink-0" />
          <div>
            <p className="text-[12px] text-primary font-medium">{t.projects.indexingInProgress}</p>
            <p className="text-[11px] text-foreground/30 font-mono">{j.path}</p>
          </div>
        </div>
      ))}
      {completed.map((j) => (
        <div key={j.slot} className="flex items-start gap-3 mt-3 first:mt-0 p-3 border border-emerald-500/20 bg-emerald-500/5 text-emerald-600">
          <CheckCircle2 className="w-4 h-4 mt-0.5 shrink-0" aria-hidden="true" />
          <div className="flex-1 min-w-0">
            <p className="text-[12px] font-semibold">{t.projects.indexingComplete}</p>
            <p className="text-[11px] font-mono truncate">{j.path}</p>
          </div>
        </div>
      ))}
      {errors.map((j) => (
        <div key={j.slot} className="flex items-start gap-3 mt-3 first:mt-0 p-3 rounded-lg border border-destructive/20 bg-destructive/5 text-destructive">
          <span className="text-[14px]">⚠️</span>
          <div className="flex-1 min-w-0">
            <p className="text-[12px] font-semibold">{t.projects.indexingFailed}</p>
            <p className="text-[11px] font-mono truncate">{j.path}</p>
            {j.error && <p className="text-[10px] opacity-75 mt-1 font-mono">{j.error}</p>}
          </div>
        </div>
      ))}
      {statusError && (
        <div className="flex items-start gap-3 mt-3 rounded-lg border border-destructive/20 bg-destructive/5 p-3 text-destructive" role="alert">
          <AlertCircle className="w-4 h-4 mt-0.5 shrink-0" aria-hidden="true" />
          <div className="min-w-0">
            <p className="text-[12px] font-semibold">{t.projects.indexingStatusFailed}</p>
            <p className="text-[10px] opacity-75 mt-1 font-mono">{statusError}</p>
          </div>
        </div>
      )}
      {(completed.length > 0 || errors.length > 0 || statusError) && (
        <div className="flex justify-end mt-3">
          <button
            onClick={onDismiss}
            className="px-3 py-1 rounded bg-foreground/5 hover:bg-foreground/10 text-foreground/60 text-[11px] font-medium transition-all"
          >
            {t.common.dismiss}
          </button>
        </div>
      )}
    </div>
  );
}

interface CrossRepoResult {
  status: string;
  project: string;
  projects_scanned: number;
  cross_http_calls: number;
  cross_async_calls: number;
  cross_channel: number;
  cross_grpc_calls: number;
  cross_graphql_calls: number;
  cross_trpc_calls: number;
  total_cross_edges: number;
  elapsed_ms: number;
}

interface CrossRepoProject {
  project: {
    name: string;
    root_path: string;
  };
}

export function CrossRepositoryPanel({
  projects,
  onComplete,
}: {
  projects: CrossRepoProject[];
  onComplete: () => void;
}) {
  const t = useUiMessages();
  const [sourceName, setSourceName] = useState("");
  const [targetNames, setTargetNames] = useState<string[]>([]);
  const [running, setRunning] = useState(false);
  const [result, setResult] = useState<CrossRepoResult | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!projects.some((item) => item.project.name === sourceName)) {
      setSourceName(projects[0]?.project.name ?? "");
    }
  }, [projects, sourceName]);

  useEffect(() => {
    const eligible = projects
      .map((item) => item.project.name)
      .filter((name) => name !== sourceName);
    setTargetNames((current) => {
      const kept = current.filter((name) => eligible.includes(name));
      return kept.length > 0 ? kept : eligible;
    });
  }, [projects, sourceName]);

  const source = projects.find((item) => item.project.name === sourceName)?.project;
  const eligibleTargets = projects.filter((item) => item.project.name !== sourceName);
  const canRun = Boolean(source && targetNames.length > 0 && !running);

  const selectSource = (name: string) => {
    setSourceName(name);
    setTargetNames(projects.map((item) => item.project.name).filter((project) => project !== name));
    setResult(null);
    setError(null);
  };

  const toggleTarget = (name: string, checked: boolean) => {
    setTargetNames((current) =>
      checked ? Array.from(new Set([...current, name])) : current.filter((item) => item !== name),
    );
    setResult(null);
    setError(null);
  };

  const run = async () => {
    if (!source || targetNames.length === 0) return;
    setRunning(true);
    setResult(null);
    setError(null);
    try {
      const next = await callTool<CrossRepoResult>("index_repository", {
        repo_path: source.root_path,
        name: source.name,
        mode: "cross-repo-intelligence",
        target_projects: targetNames,
      });
      if (next.status !== "success") {
        throw new Error(t.crossRepo.failed);
      }
      setResult(next);
      onComplete();
    } catch (e) {
      setError(e instanceof Error ? e.message : t.crossRepo.failed);
    } finally {
      setRunning(false);
    }
  };

  const metrics = result ? [
    { label: t.crossRepo.http, value: result.cross_http_calls },
    { label: t.crossRepo.async, value: result.cross_async_calls },
    { label: t.crossRepo.channels, value: result.cross_channel },
    { label: t.crossRepo.grpc, value: result.cross_grpc_calls },
    { label: t.crossRepo.graphql, value: result.cross_graphql_calls },
    { label: t.crossRepo.trpc, value: result.cross_trpc_calls },
  ] : [];

  return (
    <section className="border-y border-border/25 py-5 mb-7" aria-labelledby="cross-repo-title">
      <div className="flex items-center gap-2 mb-4">
        <GitMerge className="w-4 h-4 text-primary" aria-hidden="true" />
        <h2 id="cross-repo-title" className="text-[13px] font-semibold text-foreground/75">
          {t.crossRepo.title}
        </h2>
      </div>

      {projects.length < 2 ? (
        <p className="text-[11px] text-foreground/30">{t.crossRepo.needTwoProjects}</p>
      ) : (
        <div className="grid grid-cols-1 md:grid-cols-[minmax(0,1fr)_minmax(0,1.35fr)] gap-5">
          <div className="min-w-0">
            <label htmlFor="cross-repo-source" className="block text-[10px] uppercase text-foreground/30 mb-2">
              {t.crossRepo.source}
            </label>
            <select
              id="cross-repo-source"
              value={sourceName}
              onChange={(event) => selectSource(event.target.value)}
              disabled={running}
              className="w-full h-9 rounded-md border border-border/40 bg-[#0b1920] px-2.5 text-[11px] font-mono text-foreground/70 outline-none focus:border-primary/50 disabled:opacity-40"
            >
              {projects.map((item) => (
                <option key={item.project.name} value={item.project.name}>{item.project.name}</option>
              ))}
            </select>
            {source && <p className="mt-2 text-[10px] text-foreground/20 font-mono truncate">{source.root_path}</p>}
          </div>

          <div className="min-w-0">
            <div className="flex items-center justify-between gap-2 mb-2">
              <span className="text-[10px] uppercase text-foreground/30">{t.crossRepo.targets}</span>
              <div className="flex items-center gap-2 text-[10px]">
                <button
                  type="button"
                  onClick={() => setTargetNames(eligibleTargets.map((item) => item.project.name))}
                  disabled={running}
                  className="text-primary/70 hover:text-primary disabled:opacity-30"
                >
                  {t.crossRepo.selectAll}
                </button>
                <button
                  type="button"
                  onClick={() => setTargetNames([])}
                  disabled={running}
                  className="text-foreground/30 hover:text-foreground/60 disabled:opacity-30"
                >
                  {t.crossRepo.clear}
                </button>
              </div>
            </div>
            <div className="max-h-28 overflow-y-auto rounded-md border border-border/25 divide-y divide-border/15">
              {eligibleTargets.map((item) => {
                const checked = targetNames.includes(item.project.name);
                const id = `cross-target-${item.project.name}`;
                return (
                  <label key={item.project.name} htmlFor={id} className="flex items-center gap-2.5 px-3 h-9 text-[11px] font-mono text-foreground/55 hover:bg-white/[0.025] cursor-pointer">
                    <Checkbox
                      id={id}
                      checked={checked}
                      onCheckedChange={(value) => toggleTarget(item.project.name, value === true)}
                      disabled={running}
                      aria-label={item.project.name}
                    />
                    <span className="truncate">{item.project.name}</span>
                  </label>
                );
              })}
            </div>
          </div>
        </div>
      )}

      {projects.length >= 2 && (
        <div className="flex flex-wrap items-center justify-between gap-3 mt-4">
          <div className="min-h-5 flex-1" aria-live="polite">
            {running && (
              <span className="inline-flex items-center gap-2 text-[11px] text-primary">
                <LoaderCircle className="w-3.5 h-3.5 animate-spin" aria-hidden="true" />
                {t.crossRepo.running}
              </span>
            )}
            {error && <p className="text-[11px] text-destructive">{error}</p>}
            {!running && !error && targetNames.length === 0 && (
              <p className="text-[11px] text-foreground/25">{t.crossRepo.selectTarget}</p>
            )}
          </div>
          <button
            type="button"
            onClick={run}
            disabled={!canRun}
            className="inline-flex items-center gap-1.5 h-8 px-3 rounded-md bg-primary/15 hover:bg-primary/25 text-primary text-[11px] font-medium transition-all disabled:opacity-30 disabled:cursor-not-allowed"
          >
            <GitMerge className="w-3.5 h-3.5" aria-hidden="true" />
            {running ? t.crossRepo.running : t.crossRepo.run}
          </button>
        </div>
      )}

      {result && (
        <div className="mt-4 pt-4 border-t border-emerald-500/15" aria-live="polite">
          <div className="flex flex-wrap items-baseline gap-x-4 gap-y-1 mb-3">
            <span className="inline-flex items-center gap-1.5 text-[11px] font-semibold text-emerald-400">
              <CheckCircle2 className="w-3.5 h-3.5" aria-hidden="true" />
              {t.crossRepo.complete}
            </span>
            <span className="text-[11px] text-foreground/35">
              <strong className="text-foreground/70 tabular-nums">{result.total_cross_edges.toLocaleString()}</strong> {t.crossRepo.totalLinks}
            </span>
            <span className="text-[10px] text-foreground/20">
              {t.crossRepo.projectsScanned}: {result.projects_scanned} · {t.crossRepo.elapsed}: {Math.round(result.elapsed_ms)} ms
            </span>
          </div>
          <div className="flex flex-wrap gap-1.5">
            {metrics.map((metric) => (
              <span key={metric.label} className="inline-flex items-center gap-1.5 rounded px-2 py-1 bg-white/[0.03] text-[10px] text-foreground/35">
                {metric.label}
                <strong className="text-foreground/65 tabular-nums">{metric.value.toLocaleString()}</strong>
              </span>
            ))}
          </div>
        </div>
      )}
    </section>
  );
}

/* ── Main Stats Tab ─────────────────────────────────────── */

interface IndexJobStatus {
  job_id?: number;
  slot: number;
  status: "indexing" | "done" | "error";
  path: string;
  project?: string;
  source?: "local" | "remote";
  error?: string;
}

interface ProjectUpdateResponse {
  job_id: number;
  slot: number;
  project: string;
  source: "local" | "remote";
}

const wait = (milliseconds: number) => new Promise((resolve) => setTimeout(resolve, milliseconds));

async function waitForProjectUpdate(job: ProjectUpdateResponse): Promise<void> {
  const deadline = Date.now() + 60 * 60 * 1000;
  while (Date.now() < deadline) {
    const res = await fetch("/api/index-status");
    if (!res.ok) throw new Error(`Status request failed (${res.status})`);
    const jobs = await res.json() as IndexJobStatus[];
    const current = jobs.find((candidate) =>
      candidate.job_id === job.job_id ||
      (candidate.job_id === undefined && candidate.slot === job.slot),
    );
    if (current?.status === "done") return;
    if (current?.status === "error") {
      throw new Error(current.error || "Indexing failed");
    }
    await wait(1000);
  }
  throw new Error("Index update timed out");
}

export function StatsTab({ onSelectProject }: StatsTabProps) {
  const t = useUiMessages();
  const { projects, loading, error, refresh } = useProjects();
  const [showModal, setShowModal] = useState(false);
  const [indexing, setIndexing] = useState(false);
  const [deleteError, setDeleteError] = useState<string | null>(null);
  const [updateError, setUpdateError] = useState<string | null>(null);
  const [updateNotice, setUpdateNotice] = useState<string | null>(null);
  const [updatingProjects, setUpdatingProjects] = useState<Set<string>>(() => new Set());
  const [activeIndexProjects, setActiveIndexProjects] = useState<Set<string>>(() => new Set());
  const [hasActiveIndexJobs, setHasActiveIndexJobs] = useState(false);
  const [batchProgress, setBatchProgress] = useState<{
    current: number;
    total: number;
    project: string;
  } | null>(null);

  useEffect(() => {
    let cancelled = false;
    const poll = async () => {
      try {
        const res = await fetch("/api/index-status");
        if (!res.ok) return;
        const payload: unknown = await res.json();
        if (!Array.isArray(payload) || cancelled) return;
        const activeJobs = (payload as IndexJobStatus[]).filter((job) => job.status === "indexing");
        setHasActiveIndexJobs(activeJobs.length > 0);
        setActiveIndexProjects(new Set(
          activeJobs
            .map((job) => job.project)
            .filter((project): project is string => Boolean(project)),
        ));
      } catch {
        /* Keep the last known state during a transient status failure. */
      }
    };
    void poll();
    const timer = window.setInterval(() => void poll(), 1500);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, []);

  const aggregate = useMemo(() => {
    let totalNodes = 0, totalEdges = 0;
    for (const p of projects) {
      totalNodes += p.schema?.node_labels?.reduce((s, l) => s + l.count, 0) ?? 0;
      totalEdges += p.schema?.edge_types?.reduce((s, t) => s + t.count, 0) ?? 0;
    }
    return { projects: projects.length, nodes: totalNodes, edges: totalEdges };
  }, [projects]);

  const latestIndexedAt = useMemo(() => {
    let latest: string | undefined;
    let latestMs = Number.NEGATIVE_INFINITY;
    for (const item of projects) {
      const value = item.project.indexed_at;
      const milliseconds = value ? Date.parse(value) : Number.NaN;
      if (!Number.isNaN(milliseconds) && milliseconds > latestMs) {
        latest = value;
        latestMs = milliseconds;
      }
    }
    return latest;
  }, [projects]);

  const deleteProject = useCallback(async (name: string) => {
    if (!confirm(t.projects.deleteConfirm(name))) return;
    setDeleteError(null);
    try {
      const res = await fetch(`/api/project?name=${encodeURIComponent(name)}`, { method: "DELETE" });
      const data = await res.json().catch(() => ({}));
      if (!res.ok) throw new Error(data.error ?? `Delete failed (${res.status})`);
      refresh();
    } catch (e) {
      setDeleteError(e instanceof Error ? e.message : "Delete failed");
    }
  }, [refresh, t.projects]);

  const executeProjectUpdate = useCallback(async (name: string) => {
    setUpdatingProjects((current) => new Set(current).add(name));
    try {
      const res = await fetch(`/api/project-update?name=${encodeURIComponent(name)}`, {
        method: "POST",
      });
      const data = await res.json().catch(() => ({}));
      if (!res.ok) throw new Error(data.error ?? `Update failed (${res.status})`);
      await waitForProjectUpdate(data as ProjectUpdateResponse);
    } finally {
      setUpdatingProjects((current) => {
        const next = new Set(current);
        next.delete(name);
        return next;
      });
    }
  }, []);

  const updateProject = useCallback(async (name: string) => {
    setUpdateError(null);
    setUpdateNotice(null);
    try {
      await executeProjectUpdate(name);
      await refresh();
      setUpdateNotice(t.projects.updateComplete(name));
    } catch (e) {
      const detail = e instanceof Error ? e.message : "Update failed";
      setUpdateError(`${t.projects.updateFailed(name)}: ${detail}`);
    }
  }, [executeProjectUpdate, refresh, t.projects]);

  const updateAllProjects = useCallback(async () => {
    if (projects.length === 0 || batchProgress) return;
    setUpdateError(null);
    setUpdateNotice(null);
    const failures: string[] = [];
    for (let index = 0; index < projects.length; index++) {
      const name = projects[index].project.name;
      setBatchProgress({ current: index + 1, total: projects.length, project: name });
      try {
        await executeProjectUpdate(name);
      } catch (e) {
        const detail = e instanceof Error ? e.message : "Update failed";
        failures.push(`${name}: ${detail}`);
      }
    }
    setBatchProgress(null);
    await refresh();
    const completed = projects.length - failures.length;
    setUpdateNotice(t.projects.updateAllComplete(completed, projects.length));
    if (failures.length > 0) setUpdateError(failures.join(" · "));
  }, [batchProgress, executeProjectUpdate, projects, refresh, t.projects]);

  return (
    <ScrollArea className="h-full">
      <div className="index-workspace mx-auto max-w-6xl px-4 py-6 sm:px-6 lg:px-8 lg:py-8">
        <div className="mb-6 flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
          <div>
            <h1 className="text-[24px] font-semibold text-foreground">{t.projects.workspaceTitle}</h1>
            <p className="mt-1 text-[14px] font-medium text-foreground/55">
              {t.projects.indexedProjects}
              <span className="ml-2 tabular-nums text-primary">{aggregate.projects}</span>
            </p>
          </div>
          <button
            onClick={refresh}
            disabled={loading}
            aria-label={t.common.refresh}
            title={t.common.refresh}
            className="inline-flex h-9 w-9 items-center justify-center self-start rounded-lg border border-white/[0.08] bg-white/[0.04] text-foreground/55 transition-colors hover:bg-white/[0.08] hover:text-foreground disabled:opacity-30 sm:self-auto"
          >
            <RefreshCw className={`h-4 w-4 ${loading ? "animate-spin" : ""}`} aria-hidden="true" />
          </button>
        </div>

        {projects.length > 0 && (
          <div className="mb-7 grid grid-cols-1 gap-3 sm:grid-cols-3">
            {[
              { label: t.tabs.projects, value: aggregate.projects, color: "text-primary", icon: Database },
              { label: t.projects.nodes, value: aggregate.nodes, color: "text-sky-300", icon: Layers3 },
              { label: t.projects.edges, value: aggregate.edges, color: "text-amber-300", icon: Network },
            ].map(({ label, value, color, icon: Icon }) => (
              <div key={label} className="flex min-h-[92px] items-center gap-4 rounded-lg border border-white/[0.08] bg-card/75 px-5 py-4 shadow-sm">
                <div className={`flex h-10 w-10 shrink-0 items-center justify-center rounded-lg bg-white/[0.05] ${color}`}>
                  <Icon className="h-5 w-5" aria-hidden="true" />
                </div>
                <div>
                  <p className="text-[13px] font-medium text-foreground/55">{label}</p>
                  <p className={`mt-0.5 text-[24px] font-semibold tabular-nums ${color}`}>{value.toLocaleString()}</p>
                </div>
              </div>
            ))}
          </div>
        )}

        {indexing && (
          <IndexProgress onComplete={refresh} onDismiss={() => setIndexing(false)} />
        )}

        {batchProgress && (
          <div className="mb-5 rounded-lg border border-primary/25 bg-primary/[0.07] p-4" role="status">
            <div className="flex items-center gap-3">
              <RefreshCw className="h-5 w-5 shrink-0 animate-spin text-primary" aria-hidden="true" />
              <div className="min-w-0 flex-1">
                <div className="flex items-center justify-between gap-3 text-[13px] font-medium">
                  <span className="text-primary">{t.projects.updateAllProgress(batchProgress.current, batchProgress.total)}</span>
                  <span className="truncate font-mono text-foreground/65">{batchProgress.project}</span>
                </div>
                <div className="mt-3 h-1.5 overflow-hidden rounded-full bg-white/[0.08]">
                  <div
                    className="h-full rounded-full bg-primary transition-[width] duration-300"
                    style={{ width: `${(batchProgress.current / batchProgress.total) * 100}%` }}
                  />
                </div>
              </div>
            </div>
          </div>
        )}

        {updateNotice && (
          <div className="mb-5 flex items-center gap-2 rounded-lg border border-emerald-400/20 bg-emerald-400/[0.07] px-4 py-3 text-[13px] text-emerald-300" role="status">
            <CheckCircle2 className="h-4 w-4 shrink-0" aria-hidden="true" />
            <span>{updateNotice}</span>
          </div>
        )}

        {projects.length > 0 && (
          <CrossRepositoryPanel projects={projects} onComplete={refresh} />
        )}

        <div className="mb-5 mt-7 flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
          <h2 className="text-[18px] font-semibold text-foreground/90">{t.projects.indexedProjects}</h2>
          <div className="flex flex-wrap items-center gap-2">
            {projects.length > 0 && (
              <IndexedAt
                value={latestIndexedAt}
                label={t.projects.latestReindex}
                missingLabel={t.projects.noReindexRecord}
              />
            )}
            <button
              onClick={() => void updateAllProjects()}
              disabled={projects.length === 0 || Boolean(batchProgress) || updatingProjects.size > 0 || hasActiveIndexJobs}
              className="inline-flex h-9 items-center gap-2 rounded-lg border border-amber-300/20 bg-amber-300/[0.08] px-3.5 text-[13px] font-semibold text-amber-200 transition-colors hover:bg-amber-300/[0.14] disabled:opacity-35"
            >
              <RefreshCw className={`h-4 w-4 ${batchProgress ? "animate-spin" : ""}`} aria-hidden="true" />
              {t.projects.updateAll}
            </button>
            <button
              onClick={() => setShowModal(true)}
              className="inline-flex h-9 items-center gap-2 rounded-lg bg-primary px-3.5 text-[13px] font-semibold text-primary-foreground transition-colors hover:bg-primary/90"
            >
              <Plus className="h-4 w-4" aria-hidden="true" />
              {t.index.newIndex}
            </button>
          </div>
        </div>

        {(error || deleteError || updateError) && (
          <div className="mb-6 flex items-start gap-3 rounded-lg border border-destructive/25 bg-destructive/[0.07] p-4 text-destructive">
            <AlertCircle className="mt-0.5 h-4 w-4 shrink-0" aria-hidden="true" />
            <p className="text-[13px] leading-5">{updateError ?? deleteError ?? error}</p>
          </div>
        )}

        {!loading && projects.length === 0 && !error && (
          <div className="rounded-lg border border-dashed border-white/[0.12] bg-card/40 py-20 text-center">
            <Database className="mx-auto mb-4 h-9 w-9 text-foreground/25" aria-hidden="true" />
            <p className="mb-4 text-[14px] text-foreground/55">{t.projects.noIndexedProjects}</p>
            <button onClick={() => setShowModal(true)} className="inline-flex h-10 items-center gap-2 rounded-lg bg-primary px-4 text-[13px] font-semibold text-primary-foreground transition-colors hover:bg-primary/90">
              <Plus className="h-4 w-4" aria-hidden="true" />
              {t.projects.indexFirstRepository}
            </button>
          </div>
        )}

        <div className="space-y-3.5">
          {projects.map((p) => {
            const totalNodes = p.schema?.node_labels?.reduce((s, l) => s + l.count, 0) ?? 0;
            const totalEdges = p.schema?.edge_types?.reduce((s, t) => s + t.count, 0) ?? 0;
            const isUpdating = updatingProjects.has(p.project.name) || activeIndexProjects.has(p.project.name);
            return (
              <article key={p.project.name} className="rounded-lg border border-white/[0.08] bg-card/70 p-4 shadow-sm transition-colors hover:border-white/[0.14] hover:bg-card sm:p-5">
                <div className="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
                  <div className="flex min-w-0 items-start gap-3">
                    <div className="mt-2"><HealthDot name={p.project.name} /></div>
                    <div className="min-w-0 flex-1">
                      <h3 className="mb-1 text-[16px] font-semibold text-foreground">{p.project.name}</h3>
                      <p className="break-all font-mono text-[12px] leading-5 text-foreground/45">{p.project.root_path}</p>
                    </div>
                  </div>
                  <div className="flex shrink-0 flex-wrap items-center gap-2">
                    <IndexedAt
                      value={p.project.indexed_at}
                      label={t.projects.lastReindexed}
                      missingLabel={t.projects.noReindexRecord}
                    />
                    <AdrButton project={p.project.name} />
                    <button
                      onClick={() => void updateProject(p.project.name)}
                      disabled={isUpdating || Boolean(batchProgress)}
                      className="inline-flex h-9 items-center gap-2 rounded-lg border border-white/[0.09] bg-white/[0.04] px-3 text-[13px] font-semibold text-foreground/70 transition-colors hover:bg-white/[0.08] hover:text-foreground disabled:opacity-35"
                    >
                      <RefreshCw className={`h-4 w-4 ${isUpdating ? "animate-spin" : ""}`} aria-hidden="true" />
                      {isUpdating ? t.projects.updating : t.projects.updateGraph}
                    </button>
                    <button
                      onClick={() => onSelectProject(p.project.name)}
                      className="inline-flex h-9 items-center gap-2 rounded-lg bg-primary/15 px-3 text-[13px] font-semibold text-primary transition-colors hover:bg-primary/25"
                    >
                      {t.projects.viewGraph}
                      <ArrowUpRight className="h-4 w-4" aria-hidden="true" />
                    </button>
                    <button
                      onClick={() => deleteProject(p.project.name)}
                      disabled={isUpdating || Boolean(batchProgress)}
                      className="inline-flex h-9 w-9 items-center justify-center rounded-lg text-foreground/35 transition-colors hover:bg-destructive/10 hover:text-destructive disabled:cursor-not-allowed disabled:opacity-30 disabled:hover:bg-transparent disabled:hover:text-foreground/35"
                      title={t.projects.deleteTitle}
                      aria-label={t.projects.deleteTitle}
                    >
                      <Trash2 className="h-4 w-4" aria-hidden="true" />
                    </button>
                  </div>
                </div>
                {p.schema && (
                  <>
                    <div className="mb-3 mt-4 flex gap-6 text-[13px] text-foreground/45">
                      <span><strong className="mr-1 text-[14px] text-sky-300 tabular-nums">{totalNodes.toLocaleString()}</strong> {t.projects.nodes}</span>
                      <span><strong className="mr-1 text-[14px] text-amber-300 tabular-nums">{totalEdges.toLocaleString()}</strong> {t.projects.edges}</span>
                    </div>
                    <div className="flex flex-wrap gap-1.5">
                      {p.schema.node_labels?.map((l) => (
                        <span key={l.label} className="inline-flex items-center gap-1.5 rounded-md border border-white/[0.05] px-2 py-1 text-[11px] font-medium" style={{ backgroundColor: colorForLabel(l.label) + "12", color: colorForLabel(l.label) }}>
                          <span className="h-1.5 w-1.5 rounded-full" style={{ backgroundColor: colorForLabel(l.label) }} />
                          {l.label} {l.count.toLocaleString()}
                        </span>
                      ))}
                    </div>
                  </>
                )}
                <RemoteProjectControls project={p.project.name} />
              </article>
            );
          })}
        </div>
      </div>
      {showModal && <CreateIndexModal onClose={() => setShowModal(false)} onCreated={() => { setIndexing(true); refresh(); }} />}
    </ScrollArea>
  );
}
