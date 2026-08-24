import { FormEvent, useEffect, useMemo, useState } from "react";
import { ClipboardCheck, FileCode2, GitCompareArrows, LoaderCircle, Search, TriangleAlert } from "lucide-react";
import { callTool } from "../../api/rpc";
import type { GraphData, GraphNode } from "../../lib/types";
import { shortPath } from "./analysis";

interface ReviewChangeViewProps {
  project: string;
  data: GraphData;
  onOpenExplore: (node: GraphNode) => void;
}

type ReviewStatus = "pass" | "warn" | "block" | "unknown";
interface ReviewRule { id?: string; status?: ReviewStatus; message?: string; }
interface ReviewItem {
  id?: number;
  name?: string;
  qualified_name?: string;
  label?: string;
  file_path?: string;
  hop?: number;
  risk?: string;
  changed_by?: string;
}
interface ReviewResult {
  status?: ReviewStatus;
  risk?: string;
  risk_label_zh?: string;
  summary_zh?: string;
  changed_files?: string[];
  changed_symbols?: ReviewItem[];
  impacts?: ReviewItem[];
  tests?: string[];
  documentation?: string[];
  rules?: ReviewRule[];
  limitations?: string[];
  summary?: {
    changed_files?: number;
    changed_symbols?: number;
    direct_impacts?: number;
    indirect_impacts?: number;
    affected_files?: number;
    tests?: number;
    entry_points?: number;
    cross_service?: boolean;
  };
  analysis_meta?: Record<string, unknown>;
  error?: string;
}

function statusLabel(status?: string) {
  return status === "pass" ? "通过" : status === "warn" ? "警告" : status === "block" ? "阻断" : "未知";
}

function statusClass(status?: string) {
  return status === "pass"
    ? "border-emerald-300/25 bg-emerald-300/10 text-emerald-200/80"
    : status === "warn"
      ? "border-amber-300/25 bg-amber-300/10 text-amber-200/80"
      : status === "block"
        ? "border-red-300/25 bg-red-300/10 text-red-200/85"
        : "border-slate-300/20 bg-white/[0.04] text-foreground/45";
}

function MetaStatus({ meta }: { meta?: Record<string, unknown> }) {
  if (!meta) return null;
  const entries = ["result", "claim", "coverage", "freshness", "confidence"]
    .map((key) => {
      const value = meta[key];
      if (!value || typeof value !== "object") return null;
      const status = (value as Record<string, unknown>).status ?? (value as Record<string, unknown>).level;
      return status ? `${key}: ${String(status)}` : null;
    })
    .filter(Boolean) as string[];
  return entries.length ? (
    <div className="flex flex-wrap gap-1.5" aria-label="评审元数据状态">
      {entries.map((entry) => <span key={entry} className="rounded border border-border/30 bg-white/[0.03] px-2 py-1 text-[9px] text-foreground/45">{entry}</span>)}
    </div>
  ) : null;
}

function graphNodeFor(data: GraphData, item: ReviewItem) {
  return data.nodes.find((node) => item.id !== undefined && node.id === item.id)
    ?? data.nodes.find((node) => item.qualified_name && node.qualified_name === item.qualified_name)
    ?? data.nodes.find((node) => item.file_path && node.file_path === item.file_path);
}

function ResultList({ title, items, empty, data, onOpenExplore }: { title: string; items: ReviewItem[]; empty: string; data: GraphData; onOpenExplore: (node: GraphNode) => void }) {
  return (
    <section className="border-b border-border/20 px-4 py-4">
      <div className="flex items-center justify-between gap-2"><h3 className="text-[10px] uppercase tracking-wide text-foreground/45">{title}</h3><span className="font-mono text-[10px] text-foreground/25">{items.length}</span></div>
      {items.length ? <div className="mt-2 divide-y divide-border/15">{items.slice(0, 100).map((item, index) => {
        const node = graphNodeFor(data, item);
        return <div key={`${item.id ?? item.qualified_name ?? item.file_path}-${index}`} className="flex min-w-0 items-center gap-2 py-2">
          <FileCode2 className="h-3.5 w-3.5 shrink-0 text-cyan-300/50" aria-hidden="true" />
          <div className="min-w-0 flex-1"><p className="truncate text-[10px] text-foreground/70">{item.name ?? item.qualified_name ?? item.file_path}</p><p className="truncate font-mono text-[9px] text-foreground/25">{item.file_path ? shortPath(item.file_path, 5) : item.qualified_name}</p></div>
          {item.hop !== undefined && <span className="shrink-0 text-[9px] text-amber-200/55">{item.hop} 跳</span>}
          {node && <button type="button" onClick={() => onOpenExplore(node)} className="shrink-0 rounded border border-border/35 px-2 py-1 text-[9px] text-foreground/45 hover:bg-white/[0.05]">定位</button>}
        </div>;
      })}</div> : <p className="mt-2 text-[10px] text-foreground/25">{empty}</p>}
    </section>
  );
}

export function ReviewChangeView({ project, data, onOpenExplore }: ReviewChangeViewProps) {
  const [ref, setRef] = useState("HEAD");
  const [depth, setDepth] = useState(2);
  const [budget, setBudget] = useState(4000);
  const [includeTests, setIncludeTests] = useState(true);
  const [includeDocs, setIncludeDocs] = useState(true);
  const [result, setResult] = useState<ReviewResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  useEffect(() => { setResult(null); setError(null); }, [project]);

  const run = async (event?: FormEvent) => {
    event?.preventDefault();
    if (!ref.trim() || depth < 1 || budget <= 0) return;
    setLoading(true); setError(null);
    try {
      const next = await callTool<ReviewResult>("review_change", {
        project,
        since: ref.trim(),
        depth,
        token_budget: budget,
        evidence_level: "analysis",
        include_tests: includeTests,
        include_docs: includeDocs,
      });
      if (next.error) setError(next.error); else setResult(next);
    } catch (cause) { setError(cause instanceof Error ? cause.message : "变更评审失败。"); }
    finally { setLoading(false); }
  };

  const changedSymbols = result?.changed_symbols ?? [];
  const impacts = result?.impacts ?? [];
  const rules = result?.rules ?? [];
  const summary = result?.summary;
  const status = result?.status;
  const statText = useMemo(() => {
    if (!summary) return "等待一次 Git 变更评审";
    return `${summary.changed_files ?? 0} 个文件 · ${summary.changed_symbols ?? 0} 个变更符号 · ${summary.direct_impacts ?? 0} 个直接影响 · ${summary.indirect_impacts ?? 0} 个间接影响`;
  }, [summary]);

  return <div className="h-full min-h-0 overflow-y-auto bg-[#08151b]">
    <form onSubmit={run} className="sticky top-0 z-10 flex flex-wrap items-center gap-2 border-b border-border/25 bg-[#08151b]/95 px-4 py-2.5 backdrop-blur">
      <div className="flex h-8 min-w-[190px] flex-1 items-center rounded-md border border-border/40 bg-[#0b1920] focus-within:border-primary/50"><GitCompareArrows className="ml-2.5 h-3.5 w-3.5 shrink-0 text-foreground/25" aria-hidden="true" /><input aria-label="评审 Git ref" value={ref} onChange={(e) => setRef(e.target.value)} placeholder="比较基准，例如 HEAD 或 main" className="h-full min-w-0 flex-1 bg-transparent px-2.5 text-[11px] text-foreground/75 outline-none placeholder:text-foreground/20" /></div>
      <label className="flex h-8 items-center gap-1.5 rounded-md border border-border/40 bg-[#0b1920] px-2 text-[10px] text-foreground/40">深度 <input aria-label="评审深度" type="number" min={1} max={15} value={depth} onChange={(e) => setDepth(Number(e.target.value))} className="w-9 bg-transparent text-right font-mono text-foreground/70 outline-none" /></label>
      <label className="flex h-8 items-center gap-1.5 rounded-md border border-border/40 bg-[#0b1920] px-2 text-[10px] text-foreground/40">预算 <input aria-label="评审 Token 预算" type="number" min={1} value={budget} onChange={(e) => setBudget(Number(e.target.value))} className="w-16 bg-transparent text-right font-mono text-foreground/70 outline-none" /></label>
      <button type="submit" disabled={loading || !ref.trim() || depth < 1 || budget <= 0} className="inline-flex h-8 items-center gap-1.5 rounded-md bg-primary/15 px-3 text-[11px] font-medium text-primary transition-colors hover:bg-primary/25 disabled:opacity-35">{loading ? <LoaderCircle className="h-3.5 w-3.5 animate-spin" aria-hidden="true" /> : <ClipboardCheck className="h-3.5 w-3.5" aria-hidden="true" />}评审变更</button>
      <label className="flex items-center gap-1 text-[10px] text-foreground/35"><input type="checkbox" checked={includeTests} onChange={(e) => setIncludeTests(e.target.checked)} />测试</label>
      <label className="flex items-center gap-1 text-[10px] text-foreground/35"><input type="checkbox" checked={includeDocs} onChange={(e) => setIncludeDocs(e.target.checked)} />文档</label>
    </form>
    {error ? <div className="flex min-h-64 items-center justify-center p-8 text-center"><div><TriangleAlert className="mx-auto mb-2 h-5 w-5 text-amber-300" aria-hidden="true" /><p className="text-[12px] text-foreground/65">无法完成变更评审</p><p className="mt-1 break-words font-mono text-[10px] text-red-200/60">{error}</p></div></div> : !result ? <div className="flex min-h-64 items-center justify-center p-8 text-center"><div><Search className="mx-auto mb-2 h-5 w-5 text-cyan-200/45" aria-hidden="true" /><p className="text-[12px] text-foreground/55">输入 Git ref 后开始评审</p><p className="mt-1 text-[10px] text-foreground/25">结果由图谱、Git diff 和确定性规则共同生成</p></div></div> : <div>
      <section className="border-b border-border/20 px-4 py-4"><div className="flex flex-wrap items-start justify-between gap-3"><div><div className="flex items-center gap-2"><span className={`rounded border px-2 py-1 text-[10px] ${statusClass(status)}`}>{statusLabel(status)}</span><span className={`rounded border px-2 py-1 text-[10px] ${statusClass(result.risk === "unknown" ? "unknown" : result.risk === "critical" || result.risk === "high" ? "warn" : "pass")}`}>风险 {result.risk_label_zh ?? "未知"}</span></div><p className="mt-2 text-[12px] text-foreground/75">{result.summary_zh}</p><p className="mt-1 text-[10px] text-foreground/30">{statText}</p></div><MetaStatus meta={result.analysis_meta} /></div></section>
      <section className="grid grid-cols-2 gap-0 border-b border-border/20 sm:grid-cols-4"><div className="border-r border-border/20 p-4"><p className="text-[9px] text-foreground/35">直接影响</p><p className="mt-1 font-mono text-lg text-amber-200/75">{summary?.direct_impacts ?? 0}</p></div><div className="border-r border-border/20 p-4"><p className="text-[9px] text-foreground/35">间接影响</p><p className="mt-1 font-mono text-lg text-cyan-200/70">{summary?.indirect_impacts ?? 0}</p></div><div className="border-r border-border/20 p-4"><p className="text-[9px] text-foreground/35">入口节点</p><p className="mt-1 font-mono text-lg text-red-200/70">{summary?.entry_points ?? 0}</p></div><div className="p-4"><p className="text-[9px] text-foreground/35">跨服务</p><p className="mt-1 text-[12px] text-foreground/65">{summary?.cross_service ? "是" : "否"}</p></div></section>
      <section className="border-b border-border/20 px-4 py-4"><h3 className="text-[10px] uppercase tracking-wide text-foreground/45">评审规则</h3><div className="mt-2 grid gap-2 sm:grid-cols-2">{rules.map((rule) => <div key={rule.id} className="rounded border border-border/25 bg-white/[0.02] p-2.5"><div className="flex items-center justify-between gap-2"><span className="font-mono text-[9px] text-foreground/35">{rule.id}</span><span className={`rounded border px-1.5 py-0.5 text-[9px] ${statusClass(rule.status)}`}>{statusLabel(rule.status)}</span></div><p className="mt-1 text-[10px] leading-4 text-foreground/55">{rule.message}</p></div>)}</div></section>
      <ResultList title="变更文件" items={(result.changed_files ?? []).map((file) => ({ file_path: file }))} empty="没有变更文件" data={data} onOpenExplore={onOpenExplore} />
      <ResultList title="变更符号" items={changedSymbols} empty="没有解析出变更符号" data={data} onOpenExplore={onOpenExplore} />
      <ResultList title="影响节点" items={impacts} empty="没有发现已索引的入站影响" data={data} onOpenExplore={onOpenExplore} />
      <section className="grid grid-cols-1 gap-0 border-b border-border/20 sm:grid-cols-2"><div className="border-b border-border/20 p-4 sm:border-b-0 sm:border-r"><h3 className="text-[10px] uppercase tracking-wide text-foreground/45">测试候选</h3>{(result.tests?.length ?? 0) ? <ul className="mt-2 space-y-1">{result.tests?.slice(0, 50).map((item) => <li key={item} className="truncate font-mono text-[9px] text-emerald-200/55">{shortPath(item, 6)}</li>)}</ul> : <p className="mt-2 text-[10px] text-foreground/25">未发现测试候选</p>}</div><div className="p-4"><h3 className="text-[10px] uppercase tracking-wide text-foreground/45">文档候选</h3>{(result.documentation?.length ?? 0) ? <ul className="mt-2 space-y-1">{result.documentation?.slice(0, 50).map((item) => <li key={item} className="truncate font-mono text-[9px] text-cyan-200/55">{shortPath(item, 6)}</li>)}</ul> : <p className="mt-2 text-[10px] text-foreground/25">未发现文档候选</p>}</div></section>
      {(result.limitations?.length ?? 0) > 0 && <section className="px-4 py-4"><div className="flex items-start gap-2"><TriangleAlert className="mt-0.5 h-3.5 w-3.5 shrink-0 text-amber-300/55" aria-hidden="true" /><div className="space-y-1">{result.limitations?.map((item) => <p key={item} className="text-[9px] leading-4 text-foreground/30">{item}</p>)}</div></div></section>}
    </div>}
  </div>;
}
