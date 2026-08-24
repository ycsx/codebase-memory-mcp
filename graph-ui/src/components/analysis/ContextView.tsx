import { FormEvent, useEffect, useMemo, useState } from "react";
import { BookOpen, CheckCircle2, FileCode2, LoaderCircle, Search, TriangleAlert } from "lucide-react";
import { callTool } from "../../api/rpc";
import type { GraphData, GraphNode } from "../../lib/types";
import { shortPath } from "./analysis";

interface ContextViewProps {
  project: string;
  data: GraphData;
  onOpenExplore: (node: GraphNode) => void;
}

type EvidenceLevel = "scout" | "analysis" | "audit";
interface ContextNode {
  id?: number;
  name?: string;
  label?: string;
  qualified_name?: string;
  file_path?: string;
  file?: string;
  start_line?: number;
  end_line?: number;
  rank?: number;
  callers?: string[];
  callees?: string[];
  evidence?: string;
  in_calls?: number;
  out_calls?: number;
  in_degree?: number;
  out_degree?: number;
}
interface ContextResult {
  project?: string;
  task?: string;
  target?: string | null;
  evidence_level?: EvidenceLevel;
  resolved_target?: ContextNode | null;
  candidates?: ContextNode[];
  evidence?: ContextNode[];
  documentation?: string[];
  budget?: {
    requested_tokens?: number;
    estimated_tokens?: number;
    evidence_limit?: number;
    neighbor_limit?: number;
    truncated?: boolean;
  };
  limitations?: string[];
  analysis_meta?: Record<string, unknown>;
  error?: string;
}

function graphNodeFor(data: GraphData, item?: ContextNode | null) {
  if (!item) return undefined;
  const filePath = item.file_path ?? item.file;
  return data.nodes.find((node) => item.id !== undefined && node.id === item.id) ??
    data.nodes.find((node) => item.qualified_name && node.qualified_name === item.qualified_name) ??
    data.nodes.find((node) => filePath && node.file_path === filePath);
}

function MetaStatus({ meta }: { meta?: Record<string, unknown> }) {
  if (!meta) return null;
  const statuses = ["result", "claim", "coverage", "freshness", "confidence"]
    .map((key) => {
      const value = meta[key];
      if (!value) return null;
      const status = typeof value === "object" && value !== null
        ? (value as Record<string, unknown>).status ?? (value as Record<string, unknown>).level
        : value;
      return status ? { key, status: String(status) } : null;
    })
    .filter(Boolean) as Array<{ key: string; status: string }>;
  if (statuses.length === 0 && typeof meta.status === "string") {
    statuses.push({ key: "status", status: meta.status });
  }
  if (!statuses.length) return null;
  return (
    <div className="flex flex-wrap gap-1.5" aria-label="分析元数据状态">
      {statuses.map(({ key, status }) => (
        <span key={key} className="rounded border border-border/30 bg-white/[0.03] px-2 py-1 text-[9px] text-foreground/45">
          {key}: <span className="text-cyan-200/70">{status}</span>
        </span>
      ))}
    </div>
  );
}

function NodeRow({ item, data, onOpenExplore }: { item: ContextNode; data: GraphData; onOpenExplore: (node: GraphNode) => void }) {
  const graphNode = graphNodeFor(data, item);
  const filePath = item.file_path ?? item.file;
  return (
    <div className="min-w-0 border-b border-border/15 px-4 py-3">
      <div className="flex items-start gap-2">
        <FileCode2 className="mt-0.5 h-3.5 w-3.5 shrink-0 text-cyan-300/55" aria-hidden="true" />
        <div className="min-w-0 flex-1">
          <p className="truncate text-[11px] text-foreground/75">{item.name ?? item.qualified_name ?? filePath}</p>
          <p className="mt-1 truncate font-mono text-[9px] text-foreground/28">{item.qualified_name || shortPath(filePath ?? "", 5)}</p>
          {filePath && <p className="mt-0.5 truncate font-mono text-[9px] text-foreground/20">{shortPath(filePath, 5)}{item.start_line ? `:${item.start_line}` : ""}</p>}
          <div className="mt-2 flex flex-wrap gap-x-3 gap-y-1 text-[9px] text-foreground/35">
            <span>调用者 {item.callers?.length ?? 0}</span><span>被调用者 {item.callees?.length ?? 0}</span>
            {item.rank && <span>排序 #{item.rank}</span>}
          </div>
        </div>
        {graphNode && <button type="button" onClick={() => onOpenExplore(graphNode)} className="shrink-0 rounded border border-border/35 px-2 py-1 text-[9px] text-foreground/45 hover:bg-white/[0.05]">定位</button>}
      </div>
      {(item.callers?.length || item.callees?.length) ? (
        <div className="mt-2 grid grid-cols-1 gap-1 text-[9px] text-foreground/30 sm:grid-cols-2">
          <p className="min-w-0 truncate"><span className="text-amber-200/50">调用者:</span> {item.callers?.join(", ") || "无"}</p>
          <p className="min-w-0 truncate"><span className="text-emerald-200/50">被调用者:</span> {item.callees?.join(", ") || "无"}</p>
        </div>
      ) : null}
    </div>
  );
}

export function ContextView({ project, data, onOpenExplore }: ContextViewProps) {
  const [task, setTask] = useState("");
  const [target, setTarget] = useState("");
  const [budget, setBudget] = useState(2000);
  const [evidenceLevel, setEvidenceLevel] = useState<EvidenceLevel>("analysis");
  const [includeDocs, setIncludeDocs] = useState(false);
  const [includeTests, setIncludeTests] = useState(false);
  const [result, setResult] = useState<ContextResult | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => { setResult(null); setError(null); }, [project]);

  const run = async (event?: FormEvent) => {
    event?.preventDefault();
    if (!task.trim() || budget <= 0) return;
    setLoading(true); setError(null);
    try {
      const next = await callTool<ContextResult>("build_context", {
        project, task: task.trim(), token_budget: budget, evidence_level: evidenceLevel,
        ...(target.trim() ? { target: target.trim() } : {}), include_docs: includeDocs, include_tests: includeTests,
      });
      if (next.error) setError(next.error); else setResult(next);
    } catch (cause) { setError(cause instanceof Error ? cause.message : "上下文编译失败。"); }
    finally { setLoading(false); }
  };

  const resolved = result?.resolved_target;
  const candidates = result?.candidates ?? [];
  const evidence = result?.evidence ?? [];
  const summary = useMemo(() => {
    if (!result) return null;
    const b = result.budget;
    return `${evidence.length} 个证据节点 · 预计 ${b?.estimated_tokens ?? 0} / ${b?.requested_tokens ?? budget} tokens`;
  }, [result, evidence.length, budget]);

  return (
    <div className="h-full min-h-0 overflow-y-auto bg-[#08151b]">
      <form onSubmit={run} className="sticky top-0 z-10 flex flex-wrap items-center gap-2 border-b border-border/25 bg-[#08151b]/95 px-4 py-2.5 backdrop-blur">
        <div className="flex h-8 min-w-[220px] flex-1 items-center rounded-md border border-border/40 bg-[#0b1920] focus-within:border-primary/50">
          <Search className="ml-2.5 h-3.5 w-3.5 shrink-0 text-foreground/25" aria-hidden="true" />
          <input aria-label="上下文任务" value={task} onChange={(e) => setTask(e.target.value)} placeholder="描述要处理的任务" className="h-full min-w-0 flex-1 bg-transparent px-2.5 text-[11px] text-foreground/75 outline-none placeholder:text-foreground/20" />
        </div>
        <input aria-label="上下文目标" value={target} onChange={(e) => setTarget(e.target.value)} placeholder="目标（可选）" className="h-8 w-36 min-w-0 rounded-md border border-border/40 bg-[#0b1920] px-2.5 text-[11px] text-foreground/70 outline-none focus:border-primary/50" />
        <label className="flex h-8 items-center gap-1.5 rounded-md border border-border/40 bg-[#0b1920] px-2 text-[10px] text-foreground/40">预算 <input aria-label="Token 预算" type="number" min={1} value={budget} onChange={(e) => setBudget(Number(e.target.value))} className="w-16 bg-transparent text-right font-mono text-foreground/70 outline-none" /></label>
        <select aria-label="证据等级" value={evidenceLevel} onChange={(e) => setEvidenceLevel(e.target.value as EvidenceLevel)} className="h-8 rounded-md border border-border/40 bg-[#0b1920] px-2 text-[10px] text-foreground/65 outline-none"><option value="scout">侦察</option><option value="analysis">分析</option><option value="audit">审计</option></select>
        <button type="submit" disabled={loading || !task.trim() || budget <= 0} className="inline-flex h-8 items-center gap-1.5 rounded-md bg-primary/15 px-3 text-[11px] font-medium text-primary transition-colors hover:bg-primary/25 disabled:opacity-35">{loading ? <LoaderCircle className="h-3.5 w-3.5 animate-spin" aria-hidden="true" /> : <BookOpen className="h-3.5 w-3.5" aria-hidden="true" />}编译上下文</button>
        <label className="flex items-center gap-1 text-[10px] text-foreground/35"><input type="checkbox" checked={includeDocs} onChange={(e) => setIncludeDocs(e.target.checked)} />文档</label>
        <label className="flex items-center gap-1 text-[10px] text-foreground/35"><input type="checkbox" checked={includeTests} onChange={(e) => setIncludeTests(e.target.checked)} />测试</label>
      </form>

      {error ? <div className="flex min-h-64 items-center justify-center p-8 text-center"><div><TriangleAlert className="mx-auto mb-2 h-5 w-5 text-amber-300" aria-hidden="true" /><p className="text-[12px] text-foreground/65">无法编译上下文</p><p className="mt-1 break-words font-mono text-[10px] text-red-300/65">{error}</p></div></div>
        : loading && !result ? <div className="flex min-h-64 items-center justify-center gap-2 text-[11px] text-foreground/35"><LoaderCircle className="h-4 w-4 animate-spin text-primary" aria-hidden="true" />正在整理任务证据</div>
          : !result ? <div className="flex min-h-64 flex-col items-center justify-center gap-2 p-8 text-center"><BookOpen className="h-7 w-7 text-foreground/12" aria-hidden="true" /><p className="text-[11px] text-foreground/25">输入任务后预览可回溯的上下文证据包</p></div>
            : <div className="mx-auto max-w-[1440px]">
              <section className="border-b border-border/25 px-4 py-4 sm:px-5"><div className="flex flex-wrap items-start justify-between gap-3"><div className="min-w-0"><div className="flex flex-wrap items-center gap-2"><CheckCircle2 className="h-3.5 w-3.5 text-emerald-300/70" aria-hidden="true" /><span className="text-[10px] uppercase text-foreground/40">{result.evidence_level ?? evidenceLevel} · 上下文结果</span>{summary && <span className="text-[10px] text-foreground/25">{summary}</span>}</div><h2 className="mt-2 break-words text-[13px] font-semibold text-foreground/80">{resolved?.name ?? "目标未唯一解析"}</h2><p className="mt-1 break-all font-mono text-[10px] text-foreground/28">{resolved?.qualified_name ?? result.target ?? "请从候选中选择限定名称"}</p></div><MetaStatus meta={result.analysis_meta} /></div>
                {resolved && <div className="mt-3 flex flex-wrap gap-2 text-[10px] text-foreground/35"><span>入度 {resolved.in_degree ?? resolved.in_calls ?? 0}</span><span>出度 {resolved.out_degree ?? resolved.out_calls ?? 0}</span>{result.budget?.truncated && <span className="text-amber-200/70">已按预算裁剪</span>}</div>}
              </section>
              {candidates.length > 0 && <section className="border-b border-border/20"><div className="flex h-10 items-center gap-2 border-b border-border/15 px-4"><h3 className="text-[10px] uppercase text-foreground/40">候选目标</h3><span className="text-[10px] text-foreground/20">{candidates.length}</span></div>{candidates.map((item, index) => <NodeRow key={`${item.qualified_name ?? item.file_path ?? item.file}:${index}`} item={item} data={data} onOpenExplore={onOpenExplore} />)}</section>}
              <section className="border-b border-border/20"><div className="flex h-10 items-center gap-2 border-b border-border/15 px-4"><h3 className="text-[10px] uppercase text-foreground/40">证据节点与关系</h3><span className="text-[10px] text-foreground/20">{evidence.length}</span></div>{evidence.length ? evidence.map((item, index) => <NodeRow key={`${item.id ?? item.qualified_name ?? index}`} item={item} data={data} onOpenExplore={onOpenExplore} />) : <p className="px-4 py-8 text-[11px] text-foreground/25">暂无证据节点。</p>}</section>
              {(result.documentation?.length ?? 0) > 0 && <section className="border-b border-border/20 px-4 py-4"><div className="mb-2 flex items-center gap-2"><BookOpen className="h-3.5 w-3.5 text-cyan-200/55" aria-hidden="true" /><h3 className="text-[10px] uppercase text-foreground/40">文档证据</h3></div><div className="flex flex-wrap gap-2">{result.documentation?.map((path) => <span key={path} className="rounded border border-border/25 px-2 py-1 font-mono text-[9px] text-foreground/35">{shortPath(path, 6)}</span>)}</div></section>}
              <section className="grid grid-cols-1 gap-0 border-b border-border/20 sm:grid-cols-2"><div className="border-b border-border/20 p-4 sm:border-b-0 sm:border-r"><h3 className="text-[10px] uppercase text-foreground/40">预算与裁剪</h3><dl className="mt-3 space-y-2 text-[10px] text-foreground/45"><div className="flex justify-between gap-3"><dt>请求预算</dt><dd className="font-mono">{result.budget?.requested_tokens ?? budget}</dd></div><div className="flex justify-between gap-3"><dt>预计消耗</dt><dd className="font-mono">{result.budget?.estimated_tokens ?? 0}</dd></div><div className="flex justify-between gap-3"><dt>证据上限</dt><dd className="font-mono">{result.budget?.evidence_limit ?? "-"}</dd></div><div className="flex justify-between gap-3"><dt>邻居上限</dt><dd className="font-mono">{result.budget?.neighbor_limit ?? "-"}</dd></div><div className="flex justify-between gap-3"><dt>状态</dt><dd className={result.budget?.truncated ? "text-amber-200/70" : "text-emerald-200/65"}>{result.budget?.truncated ? "已裁剪" : "完整"}</dd></div></dl></div><div className="p-4"><h3 className="text-[10px] uppercase text-foreground/40">证据元数据</h3><div className="mt-3"><MetaStatus meta={result.analysis_meta} /></div></div></section>
              {(result.limitations?.length ?? 0) > 0 && <section className="px-4 py-4"><div className="flex items-start gap-2"><TriangleAlert className="mt-0.5 h-3.5 w-3.5 shrink-0 text-amber-300/55" aria-hidden="true" /><div className="space-y-1">{result.limitations?.map((item) => <p key={item} className="text-[9px] leading-4 text-foreground/30">{item}</p>)}</div></div></section>}
            </div>}
    </div>
  );
}
