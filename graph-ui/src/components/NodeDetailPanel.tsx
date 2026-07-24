import { useEffect, useMemo, useState } from "react";
import {
  ArrowDownLeft,
  ArrowUpRight,
  ChevronDown,
  Code2,
  ExternalLink,
  Maximize2,
  Minimize2,
  Search,
  X,
} from "lucide-react";
import { Button } from "@/components/ui/button";
import { ScrollArea } from "@/components/ui/scroll-area";
import { colorForLabel } from "../lib/colors";
import { callTool } from "../api/rpc";
import type { GraphEdge, GraphNode, RepoInfo } from "../lib/types";

const CONNECTION_PREVIEW_LIMIT = 25;

interface Connection {
  node: GraphNode;
  edgeType: string;
  direction: "inbound" | "outbound";
}

interface NodeDetailPanelProps {
  node: GraphNode;
  allNodes: GraphNode[];
  allEdges: GraphEdge[];
  project: string | null;
  repoInfo: RepoInfo | null;
  expanded?: boolean;
  onToggleExpanded?: () => void;
  onClose: () => void;
  onNavigate: (node: GraphNode) => void;
}

interface SnippetResult {
  source?: string;
  start_line?: number;
  end_line?: number;
}

function lineSuffix(node: GraphNode): string {
  if (!node.start_line) return "";
  const end = node.end_line && node.end_line !== node.start_line ? `-L${node.end_line}` : "";
  return `#L${node.start_line}${end}`;
}

function encodePath(path: string): string {
  return path.split("/").map(encodeURIComponent).join("/");
}

function githubUrl(node: GraphNode, repoInfo: RepoInfo | null): string | null {
  if (!repoInfo?.blob_base || !node.file_path) return null;
  return `${repoInfo.blob_base}/${encodePath(node.file_path)}${lineSuffix(node)}`;
}

function groupByType(connections: Connection[]): [string, Connection[]][] {
  const groups = new Map<string, Connection[]>();
  for (const connection of connections) {
    groups.set(connection.edgeType, [
      ...(groups.get(connection.edgeType) ?? []),
      connection,
    ]);
  }
  return [...groups.entries()].sort((a, b) => b[1].length - a[1].length);
}

function searchableConnectionText(connection: Connection): string {
  const { node, edgeType } = connection;
  return [node.name, node.label, node.qualified_name, node.file_path, edgeType]
    .filter(Boolean)
    .join(" ")
    .toLowerCase();
}

function connectionDescriptor(node: GraphNode): string | null {
  if (node.qualified_name && node.qualified_name !== node.name) return node.qualified_name;
  if (!node.file_path) return null;
  return node.start_line ? `${node.file_path}:${node.start_line}` : node.file_path;
}

export function NodeDetailPanel({
  node,
  allNodes,
  allEdges,
  project,
  repoInfo,
  expanded = false,
  onToggleExpanded,
  onClose,
  onNavigate,
}: NodeDetailPanelProps) {
  const [code, setCode] = useState<string | null>(null);
  const [codeLoading, setCodeLoading] = useState(false);
  const [codeError, setCodeError] = useState<string | null>(null);
  const [connectionQuery, setConnectionQuery] = useState("");
  const [showAllConnections, setShowAllConnections] = useState(false);

  useEffect(() => {
    setCode(null);
    setCodeError(null);
    setCodeLoading(false);
    setConnectionQuery("");
    setShowAllConnections(false);
  }, [node.id]);

  const canFetchCode = Boolean(project && node.qualified_name);
  const ghUrl = githubUrl(node, repoInfo);

  const loadCode = async () => {
    if (!project || !node.qualified_name) return;
    setCodeLoading(true);
    setCodeError(null);
    try {
      const result = await callTool<SnippetResult>("get_code_snippet", {
        qualified_name: node.qualified_name,
        project,
      });
      setCode(result.source ?? "(source not available)");
    } catch (error) {
      setCodeError(error instanceof Error ? error.message : "Failed to load code");
    } finally {
      setCodeLoading(false);
    }
  };

  const connections = useMemo(() => {
    const nodeMap = new Map<number, GraphNode>();
    for (const graphNode of allNodes) nodeMap.set(graphNode.id, graphNode);

    const result: Connection[] = [];
    for (const edge of allEdges) {
      if (edge.source === node.id) {
        const target = nodeMap.get(edge.target);
        if (target) {
          result.push({ node: target, edgeType: edge.type, direction: "outbound" });
        }
      }
      if (edge.target === node.id) {
        const source = nodeMap.get(edge.source);
        if (source) {
          result.push({ node: source, edgeType: edge.type, direction: "inbound" });
        }
      }
    }
    return result;
  }, [node.id, allNodes, allEdges]);

  const normalizedQuery = connectionQuery.trim().toLowerCase();
  const filteredConnections = useMemo(
    () =>
      normalizedQuery
        ? connections.filter((connection) =>
            searchableConnectionText(connection).includes(normalizedQuery),
          )
        : connections,
    [connections, normalizedQuery],
  );

  const outbound = connections.filter((connection) => connection.direction === "outbound");
  const inbound = connections.filter((connection) => connection.direction === "inbound");
  const filteredOutbound = filteredConnections.filter(
    (connection) => connection.direction === "outbound",
  );
  const filteredInbound = filteredConnections.filter(
    (connection) => connection.direction === "inbound",
  );
  const outboundGroups = groupByType(filteredOutbound);
  const inboundGroups = groupByType(filteredInbound);
  const previewCount = [...outboundGroups, ...inboundGroups].reduce(
    (count, [, groupedConnections]) =>
      count + Math.min(groupedConnections.length, CONNECTION_PREVIEW_LIMIT),
    0,
  );
  const queryShowsAll = normalizedQuery.length > 0;
  const revealAll = showAllConnections || queryShowsAll;
  const canRevealMore = filteredConnections.length > previewCount;

  return (
    <div className="flex h-full min-h-0 w-full flex-col overflow-hidden bg-[#0b1920]/95 backdrop-blur-xl">
      <div className="border-b border-border/30 px-4 pb-3 pt-4">
        <div className="mb-2 flex items-start justify-between gap-3">
          <div className="min-w-0 flex-1">
            <div className="mb-1.5 flex items-start gap-2">
              <span
                className="mt-1 h-2.5 w-2.5 shrink-0 rounded-full"
                style={{ backgroundColor: colorForLabel(node.label) }}
              />
              <h3 className="min-w-0 break-words text-[13px] font-semibold leading-5 text-foreground">
                {node.name}
              </h3>
            </div>
            <div className="flex flex-wrap items-center gap-2">
              <span
                className="inline-block rounded-md px-2 py-0.5 text-[10px] font-medium"
                style={{
                  backgroundColor: colorForLabel(node.label) + "18",
                  color: colorForLabel(node.label),
                }}
              >
                {node.label}
              </span>
              {node.qualified_name && (
                <span className="min-w-0 break-all font-mono text-[10px] text-foreground/25">
                  {node.qualified_name}
                </span>
              )}
            </div>
          </div>
          <div className="flex shrink-0 items-center gap-1">
            {onToggleExpanded && (
              <Button
                type="button"
                variant="ghost"
                size="icon-sm"
                onClick={onToggleExpanded}
                aria-label={expanded ? "Restore detail panel" : "Expand detail panel"}
                aria-pressed={expanded}
                title={expanded ? "Restore detail panel" : "Expand detail panel"}
                className="text-foreground/35 hover:text-foreground/80"
              >
                {expanded ? <Minimize2 /> : <Maximize2 />}
              </Button>
            )}
            <Button
              type="button"
              variant="ghost"
              size="icon-sm"
              onClick={onClose}
              aria-label="Close detail panel"
              title="Close detail panel"
              className="text-foreground/35 hover:text-foreground/80"
            >
              <X />
            </Button>
          </div>
        </div>

        {node.file_path && (
          <p className="mt-2 break-all font-mono text-[11px] leading-relaxed text-foreground/30">
            {node.file_path}
            {node.start_line ? (
              <span className="text-foreground/45">
                :{node.start_line}
                {node.end_line && node.end_line !== node.start_line ? `-${node.end_line}` : ""}
              </span>
            ) : null}
          </p>
        )}

        <div className="mt-2.5 flex flex-wrap items-center gap-2">
          {canFetchCode && (
            <Button
              type="button"
              variant="secondary"
              size="xs"
              onClick={code ? () => setCode(null) : loadCode}
              disabled={codeLoading}
            >
              <Code2 />
              {codeLoading ? "Loading..." : code ? "Hide code" : "Show code"}
            </Button>
          )}
          {ghUrl && (
            <Button asChild variant="secondary" size="xs">
              <a href={ghUrl} target="_blank" rel="noopener noreferrer">
                <ExternalLink />
                Open on GitHub
              </a>
            </Button>
          )}
        </div>

        {codeError && <p className="mt-2 text-[11px] text-red-400/80">{codeError}</p>}
        {code && (
          <pre className="mt-2 max-h-[260px] overflow-auto rounded-md border border-white/[0.06] bg-black/40 p-2.5 font-mono text-[10.5px] leading-relaxed text-foreground/75 whitespace-pre">
            {code}
          </pre>
        )}

        <div className="mt-3 grid grid-cols-3 divide-x divide-border/30 border-t border-border/20 pt-3">
          {[
            { label: "Out", value: outbound.length, color: "text-primary" },
            { label: "In", value: inbound.length, color: "text-accent" },
            { label: "Total", value: connections.length, color: "text-foreground" },
          ].map((stat) => (
            <div key={stat.label} className="px-3 first:pl-0">
              <p className="text-[9px] uppercase tracking-widest text-foreground/25">
                {stat.label}
              </p>
              <p className={`text-[18px] font-semibold tabular-nums ${stat.color}`}>
                {stat.value}
              </p>
            </div>
          ))}
        </div>
      </div>

      {connections.length > 0 && (
        <div className="border-b border-border/20 px-4 py-3">
          <div className="mb-2 flex items-center justify-between gap-3">
            <p className="text-[11px] font-medium text-foreground/50">
              Connections
              {normalizedQuery && (
                <span className="ml-1.5 text-foreground/20">
                  {filteredConnections.length}/{connections.length}
                </span>
              )}
            </p>
            {(canRevealMore || showAllConnections) && !queryShowsAll && (
              <button
                type="button"
                onClick={() => setShowAllConnections((value) => !value)}
                className="text-[10px] font-medium text-primary/70 transition-colors hover:text-primary"
              >
                {showAllConnections ? "Show less" : `Show all ${connections.length}`}
              </button>
            )}
          </div>
          <label className="relative block">
            <Search className="pointer-events-none absolute left-2.5 top-1/2 size-3.5 -translate-y-1/2 text-foreground/20" />
            <input
              type="search"
              value={connectionQuery}
              onChange={(event) => setConnectionQuery(event.target.value)}
              placeholder="Filter connections"
              aria-label="Filter connections"
              className="h-8 w-full rounded-md border border-border/35 bg-black/15 pl-8 pr-3 text-[11px] text-foreground/75 outline-none placeholder:text-foreground/20 focus:border-primary/40 focus:ring-2 focus:ring-primary/10"
            />
          </label>
        </div>
      )}

      <ScrollArea className="min-h-0 flex-1">
        <div className={`space-y-5 py-3 ${expanded ? "px-5" : "px-4"}`}>
          {filteredOutbound.length > 0 && (
            <ConnectionSection
              title="References"
              count={filteredOutbound.length}
              direction="outbound"
              groups={outboundGroups}
              expanded={expanded}
              showAll={revealAll}
              onShowAll={() => setShowAllConnections(true)}
              onNavigate={onNavigate}
            />
          )}
          {filteredInbound.length > 0 && (
            <ConnectionSection
              title="Referenced by"
              count={filteredInbound.length}
              direction="inbound"
              groups={inboundGroups}
              expanded={expanded}
              showAll={revealAll}
              onShowAll={() => setShowAllConnections(true)}
              onNavigate={onNavigate}
            />
          )}
          {connections.length === 0 && (
            <p className="py-8 text-center text-[12px] text-foreground/20">No connections</p>
          )}
          {connections.length > 0 && filteredConnections.length === 0 && (
            <p className="py-8 text-center text-[12px] text-foreground/25">
              No matching connections
            </p>
          )}
        </div>
      </ScrollArea>
    </div>
  );
}

interface ConnectionSectionProps {
  title: string;
  count: number;
  direction: "inbound" | "outbound";
  groups: [string, Connection[]][];
  expanded: boolean;
  showAll: boolean;
  onShowAll: () => void;
  onNavigate: (node: GraphNode) => void;
}

function ConnectionSection({
  title,
  count,
  direction,
  groups,
  expanded,
  showAll,
  onShowAll,
  onNavigate,
}: ConnectionSectionProps) {
  const DirectionIcon = direction === "outbound" ? ArrowUpRight : ArrowDownLeft;

  return (
    <section>
      <div className="mb-2 flex items-center gap-1.5">
        <DirectionIcon className="size-3.5 text-foreground/25" />
        <p className="text-[11px] font-medium text-foreground/45">
          {title} <span className="text-foreground/20">({count})</span>
        </p>
      </div>
      {groups.map(([type, groupedConnections]) => {
        const visibleConnections = showAll
          ? groupedConnections
          : groupedConnections.slice(0, CONNECTION_PREVIEW_LIMIT);
        const hiddenCount = groupedConnections.length - visibleConnections.length;

        return (
          <div key={type} className="mb-3 last:mb-0">
            <div className="mb-1 flex items-center justify-between gap-2 px-2">
              <p className="text-[9px] font-medium uppercase tracking-wider text-foreground/25">
                {type.replace(/_/g, " ").toLowerCase()}
              </p>
              <span className="font-mono text-[9px] tabular-nums text-foreground/15">
                {groupedConnections.length}
              </span>
            </div>
            <div className="space-y-0.5">
              {visibleConnections.map((connection, index) => {
                const descriptor = connectionDescriptor(connection.node);
                return (
                  <button
                    key={`${connection.node.id}-${connection.edgeType}-${index}`}
                    type="button"
                    onClick={() => onNavigate(connection.node)}
                    title={descriptor ?? connection.node.name}
                    className={`group grid w-full grid-cols-[auto_auto_minmax(0,1fr)_auto] items-start gap-x-2 rounded-md px-2 text-left text-[11px] transition-colors hover:bg-white/[0.05] ${
                      expanded ? "py-2" : "py-1.5"
                    }`}
                  >
                    <DirectionIcon className="mt-0.5 size-3 text-foreground/15 group-hover:text-foreground/35" />
                    <span
                      className="mt-1 h-[6px] w-[6px] shrink-0 rounded-full"
                      style={{ backgroundColor: colorForLabel(connection.node.label) }}
                    />
                    <span className="min-w-0">
                      <span
                        className={`block text-foreground/60 group-hover:text-foreground/90 ${
                          expanded ? "break-words leading-4" : "truncate"
                        }`}
                      >
                        {connection.node.name}
                      </span>
                      {expanded && descriptor && (
                        <span className="mt-0.5 block break-all font-mono text-[9px] leading-3.5 text-foreground/20 group-hover:text-foreground/35">
                          {descriptor}
                        </span>
                      )}
                    </span>
                    <span className="mt-0.5 shrink-0 text-[9px] text-foreground/15">
                      {connection.node.label}
                    </span>
                  </button>
                );
              })}
              {hiddenCount > 0 && (
                <button
                  type="button"
                  onClick={onShowAll}
                  className="flex w-full items-center justify-center gap-1 rounded-md py-1.5 text-[10px] font-medium text-primary/60 transition-colors hover:bg-primary/[0.06] hover:text-primary"
                >
                  <ChevronDown className="size-3" />
                  Show {hiddenCount} more
                </button>
              )}
            </div>
          </div>
        );
      })}
    </section>
  );
}
