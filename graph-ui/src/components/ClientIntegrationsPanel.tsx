import { useCallback, useEffect, useMemo, useState } from "react";
import {
  AlertCircle,
  CheckCircle2,
  ChevronDown,
  ChevronRight,
  FileCog,
  PlugZap,
  RefreshCw,
  ShieldCheck,
} from "lucide-react";
import { Button } from "@/components/ui/button";
import {
  applyIntegrations,
  getIntegrationPlan,
  type IntegrationPlan,
} from "../api/integrations";
import { useUiMessages } from "../lib/i18n";

function displayClientName(id: string): string {
  return id
    .split("-")
    .filter(Boolean)
    .map((part) => part.length <= 3 ? part.toUpperCase() : part[0].toUpperCase() + part.slice(1))
    .join(" ");
}

function FileList({ paths, empty }: { paths: string[]; empty: string }) {
  if (paths.length === 0) {
    return <p className="py-3 text-[11px] text-foreground/30">{empty}</p>;
  }
  return (
    <ul className="divide-y divide-border/20">
      {paths.map((path) => (
        <li key={path} className="flex min-h-9 items-center gap-2 py-2">
          <FileCog className="size-3.5 shrink-0 text-foreground/25" aria-hidden="true" />
          <span className="min-w-0 truncate font-mono text-[10px] text-foreground/55" title={path}>
            {path}
          </span>
        </li>
      ))}
    </ul>
  );
}

export function ClientIntegrationsPanel() {
  const t = useUiMessages();
  const [plan, setPlan] = useState<IntegrationPlan | null>(null);
  const [loading, setLoading] = useState(true);
  const [applying, setApplying] = useState(false);
  const [expanded, setExpanded] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [success, setSuccess] = useState(false);

  const refresh = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      setPlan(await getIntegrationPlan());
    } catch (err) {
      setError(err instanceof Error ? err.message : "Unable to load integrations");
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const supportingFiles = useMemo(() => {
    if (!plan) return [];
    return Array.from(new Set([
      ...plan.instruction_files_planned,
      ...plan.hooks_planned.map((hook) => hook.path),
    ]));
  }, [plan]);

  const fileCount = (plan?.config_files_planned.length ?? 0) + supportingFiles.length;
  const canApply = Boolean(plan && plan.agents_detected.length > 0 && fileCount > 0);

  const apply = useCallback(async () => {
    setApplying(true);
    setError(null);
    setSuccess(false);
    try {
      const result = await applyIntegrations();
      if (result.plan) setPlan(result.plan);
      setSuccess(true);
      setConfirming(false);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Unable to apply integrations");
    } finally {
      setApplying(false);
    }
  }, []);

  return (
    <section aria-labelledby="client-integrations-title" className="mb-8 overflow-hidden rounded-lg border border-border/40 bg-white/[0.02]">
      <div className="flex min-h-20 items-start justify-between gap-3 px-4 py-4 sm:gap-4 sm:px-5">
        <div className="flex min-w-0 items-start gap-3">
          <div className="grid size-9 shrink-0 place-items-center rounded-md bg-primary/10 text-primary">
            <PlugZap className="size-4.5" aria-hidden="true" />
          </div>
          <div className="min-w-0">
            <h3 id="client-integrations-title" className="text-[13px] font-semibold text-foreground/85">
              {t.integrations.title}
            </h3>
            <p className="mt-1 text-[11px] leading-4 text-foreground/40">
              {t.integrations.description}
            </p>
          </div>
        </div>
        <Button
          type="button"
          variant="ghost"
          size="icon-sm"
          onClick={() => void refresh()}
          disabled={loading || applying}
          title={t.common.refresh}
          aria-label={t.common.refresh}
        >
          <RefreshCw className={loading ? "animate-spin" : ""} />
        </Button>
      </div>

      <div className="grid min-h-16 grid-cols-2 border-y border-border/25 bg-black/10">
        <div className="border-r border-border/25 px-4 py-3 sm:px-5">
          <p className="text-[9px] uppercase text-foreground/25">{t.integrations.detected}</p>
          <p className="mt-1 text-[18px] font-semibold tabular-nums text-foreground/80">
            {loading ? "-" : plan?.agents_detected.length ?? 0}
          </p>
        </div>
        <div className="px-4 py-3 sm:px-5">
          <p className="text-[9px] uppercase text-foreground/25">{t.integrations.managedFiles}</p>
          <p className="mt-1 text-[18px] font-semibold tabular-nums text-primary">
            {loading ? "-" : fileCount}
          </p>
        </div>
      </div>

      <div className="px-4 py-4 sm:px-5">
        {error ? (
          <div role="alert" className="flex items-center justify-between gap-3 rounded-md border border-destructive/25 bg-destructive/5 px-3 py-2.5">
            <span className="flex min-w-0 items-center gap-2 text-[11px] text-destructive">
              <AlertCircle className="size-4 shrink-0" aria-hidden="true" />
              <span className="truncate">{error}</span>
            </span>
            <Button type="button" size="xs" variant="ghost" onClick={() => void refresh()}>
              {t.integrations.retry}
            </Button>
          </div>
        ) : success ? (
          <div role="status" className="flex items-center gap-2 rounded-md border border-primary/25 bg-primary/5 px-3 py-2.5 text-[11px] text-primary">
            <CheckCircle2 className="size-4" aria-hidden="true" />
            {t.integrations.success}
          </div>
        ) : null}

        {!loading && plan && (
          <>
            <div className="mt-4 flex min-h-8 flex-wrap items-center gap-2">
              {plan.agents_detected.length === 0 ? (
                <p className="text-[11px] text-foreground/35">{t.integrations.noClients}</p>
              ) : plan.agents_detected.map((client) => (
                <span key={client} className="rounded-md border border-border/35 bg-black/10 px-2 py-1 text-[10px] text-foreground/60">
                  {displayClientName(client)}
                </span>
              ))}
            </div>

            <button
              type="button"
              className="mt-4 flex h-8 w-full items-center justify-between border-t border-border/25 pt-3 text-left text-[11px] font-medium text-foreground/55 hover:text-foreground/80"
              onClick={() => setExpanded((value) => !value)}
              aria-expanded={expanded}
            >
              <span>{t.integrations.review}</span>
              {expanded ? <ChevronDown className="size-4" /> : <ChevronRight className="size-4" />}
            </button>

            {expanded && (
              <div className="grid grid-cols-1 gap-4 pt-4 sm:grid-cols-2 sm:gap-6">
                <div className="min-w-0">
                  <h4 className="text-[10px] font-medium text-foreground/40">{t.integrations.configFiles}</h4>
                  <FileList paths={plan.config_files_planned} empty={t.integrations.noFiles} />
                </div>
                <div className="min-w-0">
                  <h4 className="text-[10px] font-medium text-foreground/40">{t.integrations.supportingFiles}</h4>
                  <FileList paths={supportingFiles} empty={t.integrations.noFiles} />
                </div>
              </div>
            )}

            <div className="mt-5 flex flex-col items-stretch gap-3 border-t border-border/25 pt-4 sm:flex-row sm:items-center sm:justify-between sm:gap-4">
              <span className="flex min-w-0 items-center gap-2 text-[10px] text-foreground/30">
                <ShieldCheck className="size-4 text-primary/70" aria-hidden="true" />
                {t.integrations.safePreview}
              </span>
              <Button className="self-end sm:self-auto" type="button" size="sm" onClick={() => setConfirming(true)} disabled={!canApply || applying}>
                <PlugZap />
                {t.integrations.apply}
              </Button>
            </div>
          </>
        )}

        {confirming && (
          <div className="mt-4 border-t border-border/25 pt-4" role="dialog" aria-modal="false" aria-labelledby="integration-confirm-title">
            <h4 id="integration-confirm-title" className="text-[12px] font-semibold text-foreground/80">
              {t.integrations.confirmTitle}
            </h4>
            <p className="mt-1 max-w-2xl text-[11px] leading-4 text-foreground/40">
              {t.integrations.confirmBody}
            </p>
            <div className="mt-3 flex justify-end gap-2">
              <Button type="button" size="sm" variant="ghost" onClick={() => setConfirming(false)} disabled={applying}>
                {t.common.cancel}
              </Button>
              <Button type="button" size="sm" onClick={() => void apply()} disabled={applying}>
                {applying ? <RefreshCw className="animate-spin" /> : <ShieldCheck />}
                {applying ? t.integrations.applying : t.integrations.confirm}
              </Button>
            </div>
          </div>
        )}
      </div>
    </section>
  );
}
