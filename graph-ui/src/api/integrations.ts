export interface IntegrationHook {
  agent: string;
  path: string;
}

export interface IntegrationPlan {
  type: "agent.install.plan.v1";
  agents_detected: string[];
  config_files_planned: string[];
  instruction_files_planned: string[];
  skill_files_planned: string[];
  agent_files_planned: string[];
  prompt_files_planned: string[];
  hooks_planned: IntegrationHook[];
  writes_started: false;
  network_after_install: false;
  next_safe_command: string;
}

interface ApplyIntegrationsResponse {
  status: "applied";
  plan?: IntegrationPlan;
}

async function readJson<T>(response: Response): Promise<T> {
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    const message = typeof body?.error === "string" ? body.error : `HTTP ${response.status}`;
    throw new Error(message);
  }
  return body as T;
}

export async function getIntegrationPlan(): Promise<IntegrationPlan> {
  return readJson<IntegrationPlan>(await fetch("/api/integrations"));
}

export async function applyIntegrations(): Promise<ApplyIntegrationsResponse> {
  return readJson<ApplyIntegrationsResponse>(await fetch("/api/integrations/apply", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ confirm: true }),
  }));
}
