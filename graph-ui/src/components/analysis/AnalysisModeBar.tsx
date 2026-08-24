import { BookOpen, ClipboardCheck, Flame, GitCompareArrows, Orbit, SearchCode } from "lucide-react";

export type AnalysisMode = "explore" | "query" | "impact" | "hotspots" | "context" | "review";

interface AnalysisModeBarProps {
  mode: AnalysisMode;
  onChange: (mode: AnalysisMode) => void;
}

const MODES = [
  { id: "explore", label: "探索", icon: Orbit },
  { id: "query", label: "影响查询", icon: SearchCode },
  { id: "impact", label: "影响", icon: GitCompareArrows },
  { id: "hotspots", label: "热点", icon: Flame },
  { id: "context", label: "上下文", icon: BookOpen },
  { id: "review", label: "评审", icon: ClipboardCheck },
] as const;

export function AnalysisModeBar({ mode, onChange }: AnalysisModeBarProps) {
  return (
    <div className="h-11 shrink-0 border-b border-border/30 bg-[#0b1920]/75 px-4 flex items-center justify-between">
      <div
        className="inline-flex h-8 items-center rounded-md border border-border/40 bg-black/15 p-0.5"
        role="tablist"
        aria-label="分析视图"
      >
        {MODES.map(({ id, label, icon: Icon }) => (
          <button
            key={id}
            type="button"
            role="tab"
            aria-selected={mode === id}
            onClick={() => onChange(id)}
            className={`h-7 px-3 inline-flex items-center gap-1.5 rounded text-[11px] font-medium transition-colors ${
              mode === id
                ? "bg-white/[0.09] text-foreground shadow-sm"
                : "text-foreground/35 hover:text-foreground/65"
            }`}
          >
            <Icon className="w-3.5 h-3.5" aria-hidden="true" />
            {label}
          </button>
        ))}
      </div>
      <p className="hidden md:block text-[10px] text-foreground/25">
        {mode === "explore"
          ? "完整依赖图"
          : mode === "query"
            ? "单点影响"
          : mode === "impact"
            ? "变更传播"
            : mode === "hotspots"
              ? "高入度风险"
              : mode === "context"
                ? "任务证据包"
                : "Git 变更评审"}
      </p>
    </div>
  );
}
