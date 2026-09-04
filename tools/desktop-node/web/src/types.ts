export type CapabilityDescriptor = {
  id: string
  version: number
  permission: 'public' | 'trusted'
  features?: string[]
  limits?: { weight?: number; max_concurrency?: number }
}

export type ReconNode = {
  device_id: string
  device_type: string
  firmware: string
  roles: string[]
  capabilities: string[]
  capability_descriptors: CapabilityDescriptor[]
  resources: {
    network_mbps?: number
    persistent_storage?: boolean
    storage_free_bytes?: number
  }
  security?: {
    paired?: boolean
    mode?: string
    primary_coordinator?: string
    coordinator_priority?: number
    active_coordinator?: string
    active_priority?: number
    lease_remaining_ms?: number
  }
  status: 'ready' | 'busy' | 'degraded'
  address: string
  port: number
  age_seconds: number
  local: boolean
}

export type AppState = {
  revision: number
  nodes: ReconNode[]
  coordinator_id: string
  updated_at_ms: number
}

export type Activity = {
  id: string
  time: Date
  title: string
  detail: string
  tone: 'ok' | 'warn' | 'info'
}

export type ScanJob = {
  archiveId?: string
  projectId?: string
  scope?: { network: string; start: string; end: string }
  providerId: string
  job_id?: string | number
  job_status: 'idle' | 'running' | 'complete' | 'failed' | 'stopped'
  checked: number
  total: number
  hosts: string[]
  error?: string
  recurring?: boolean
  run_count?: number
}

export type Project = { id: string; name: string; description: string; created_at_ms: number; updated_at_ms: number }
export type ArchivedJob = { id: string; project_id: string; provider_id?: string; capability?: string; status?: string; checked?: number; total?: number; hosts?: string[]; scope?: Record<string, string>; error?: string; created_at_ms: number; updated_at_ms: number }
export type EvidenceRecord = { id: string; project_id: string; job_id?: string; kind: string; title: string; summary: string; data?: { hosts?: string[]; [key: string]: unknown }; captured_at_ms: number }
export type WorkspaceData = { revision: number; projects: Project[]; jobs: ArchivedJob[]; evidence: EvidenceRecord[] }
