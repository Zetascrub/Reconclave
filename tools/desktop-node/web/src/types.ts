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
