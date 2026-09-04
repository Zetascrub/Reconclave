import { useEffect, useMemo, useRef, useState } from 'react'
import type { Activity, AppState, CapabilityDescriptor, ReconNode, ScanJob } from './types'

const emptyState: AppState = { revision: 0, nodes: [], coordinator_id: '', updated_at_ms: 0 }

function savedActivity(): Activity[] {
  try {
    const value = JSON.parse(localStorage.getItem('reconclave.activity') ?? '[]') as Array<Omit<Activity, 'time'> & { time: string }>
    return value.slice(0, 8).map((item) => ({ ...item, time: new Date(item.time) }))
  } catch { return [] }
}

function savedJob(): ScanJob | null {
  try { return JSON.parse(localStorage.getItem('reconclave.scoutJob') ?? 'null') as ScanJob | null }
  catch { return null }
}

function timeAgo(seconds: number) {
  if (seconds < 2) return 'just now'
  if (seconds < 60) return `${Math.floor(seconds)}s ago`
  return `${Math.floor(seconds / 60)}m ago`
}

function bytes(value?: number) {
  if (!value) return '—'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let amount = value
  let unit = 0
  while (amount >= 1024 && unit < units.length - 1) { amount /= 1024; unit += 1 }
  return `${amount.toFixed(unit > 2 ? 1 : 0)} ${units[unit]}`
}

function capabilityMeta(node: ReconNode, id: string): CapabilityDescriptor {
  return node.capability_descriptors.find((item) => item.id === id) ?? {
    id, version: 1, permission: 'public', features: [], limits: { weight: 1, max_concurrency: 1 },
  }
}

function NodeGlyph({ node }: { node: ReconNode }) {
  const label = node.device_type.includes('desktop') ? 'DX' :
    node.device_type.includes('p4') ? 'P4' : node.device_type.includes('card') ? 'CP' : 'NX'
  return <div className={`node-glyph ${node.status}`}><span>{label}</span><i /></div>
}

function defaultScope(address: string) {
  const octets = address.split('.')
  if (octets.length !== 4 || octets.some((part) => !/^\d+$/.test(part))) {
    return { network: '', start: '', end: '' }
  }
  const prefix = octets.slice(0, 3).join('.')
  return { network: `${prefix}.0/24`, start: `${prefix}.1`, end: `${prefix}.254` }
}

function App() {
  const [state, setState] = useState<AppState>(emptyState)
  const [selectedId, setSelectedId] = useState('')
  const [connected, setConnected] = useState(false)
  const [busyCapability, setBusyCapability] = useState('')
  const [result, setResult] = useState<Record<string, unknown> | null>(null)
  const [activity, setActivity] = useState<Activity[]>(savedActivity)
  const [filter, setFilter] = useState('')
  const [scoutOpen, setScoutOpen] = useState(false)
  const [scope, setScope] = useState({ network: '', start: '', end: '' })
  const [authorised, setAuthorised] = useState(false)
  const [scoutError, setScoutError] = useState('')
  const [scanJob, setScanJob] = useState<ScanJob | null>(savedJob)
  const statusFailures = useRef(0)

  useEffect(() => {
    fetch('/api/state').then((response) => response.json()).then(setState).catch(() => setConnected(false))
    const events = new EventSource('/api/events')
    events.addEventListener('state', (event) => {
      setState(JSON.parse((event as MessageEvent).data))
      setConnected(true)
    })
    events.onopen = () => setConnected(true)
    events.onerror = () => setConnected(false)
    return () => events.close()
  }, [])

  useEffect(() => {
    localStorage.setItem('reconclave.activity', JSON.stringify(activity))
  }, [activity])

  useEffect(() => {
    if (scanJob) localStorage.setItem('reconclave.scoutJob', JSON.stringify(scanJob))
    else localStorage.removeItem('reconclave.scoutJob')
  }, [scanJob])

  const nodes = useMemo(() => {
    const query = filter.trim().toLowerCase()
    return query ? state.nodes.filter((node) =>
      [node.device_id, node.device_type, ...node.capabilities].some((value) => value.toLowerCase().includes(query))) : state.nodes
  }, [filter, state.nodes])
  const selected = state.nodes.find((node) => node.device_id === selectedId) ?? state.nodes[0]
  const capabilityCount = new Set(state.nodes.flatMap((node) => node.capabilities)).size
  const coordinatorCount = state.nodes.filter((node) => node.roles.includes('coordinator')).length

  function addActivity(entry: Omit<Activity, 'id' | 'time'>) {
    setActivity((items) => [{ ...entry, id: crypto.randomUUID(), time: new Date() }, ...items].slice(0, 8))
  }

  async function requestCapability(node: ReconNode, capability: string, arguments_: Record<string, unknown> = {}, operatorAuthorised = false) {
    setBusyCapability(capability)
    try {
      const response = await fetch(`/api/nodes/${encodeURIComponent(node.device_id)}/invoke`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ capability, arguments: arguments_, operator_authorised: operatorAuthorised }),
      })
      const body = await response.json()
      if (!response.ok) throw new Error(body.message ?? body.error ?? 'Request failed')
      if (body.payload?.status === 'rejected' || body.payload?.status === 'error') {
        throw new Error(body.payload?.error?.message ?? body.payload?.error?.code ??
          `Node returned ${body.payload.status}`)
      }
      return body
    } catch (error) {
      throw error instanceof Error ? error : new Error('Request failed')
    } finally {
      setBusyCapability('')
    }
  }

  async function invoke(capability: string) {
    if (!selected) return
    setResult(null)
    try {
      const body = await requestCapability(selected, capability)
      setResult(body)
      addActivity({ title: capability, detail: `${selected.device_id} returned ${body.payload?.status ?? 'a response'}`, tone: 'ok' })
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Request failed'
      setResult({ error: message })
      addActivity({ title: capability, detail: message, tone: 'warn' })
    }
  }

  function configureScout() {
    if (!selected) return
    setScope(defaultScope(selected.address))
    setAuthorised(false)
    setScoutError('')
    setScoutOpen(true)
  }

  async function startScout() {
    if (!selected) return
    setScoutError('')
    try {
      const body = await requestCapability(selected, 'net.discovery.scan', {
        network: scope.network, start_ip: scope.start, end_ip: scope.end,
      }, authorised)
      const next = body.payload?.result as Omit<ScanJob, 'providerId'>
      setScanJob({ providerId: selected.device_id, ...next })
      setScoutOpen(false)
      addActivity({ title: 'Scout dispatched', detail: `${scope.start} → ${scope.end} via ${selected.device_id}`, tone: 'info' })
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Dispatch failed'
      setScoutError(message)
      addActivity({ title: 'Scout refused', detail: message, tone: 'warn' })
    }
  }

  useEffect(() => {
    if (!scanJob || scanJob.job_status !== 'running') return
    const provider = state.nodes.find((node) => node.device_id === scanJob.providerId)
    if (!provider) return
    const timer = window.setTimeout(async () => {
      try {
        const body = await requestCapability(provider, 'coordination.job.status')
        const next = { providerId: provider.device_id, ...body.payload?.result } as ScanJob
        statusFailures.current = 0
        setScanJob(next)
        if (next.job_status === 'complete') {
          addActivity({ title: 'Scout complete', detail: `${next.hosts.length} responsive host${next.hosts.length === 1 ? '' : 's'} observed`, tone: 'ok' })
        }
      } catch (error) {
        statusFailures.current += 1
        const message = error instanceof Error ? error.message : 'Status request failed'
        if (statusFailures.current >= 3) {
          setScanJob((current) => current ? { ...current, job_status: 'failed', error: message } : current)
          addActivity({ title: 'Scout telemetry lost', detail: message, tone: 'warn' })
        }
      }
    }, 1000)
    return () => window.clearTimeout(timer)
  }, [scanJob, state.nodes])

  async function cancelScout() {
    if (!scanJob) return
    const provider = state.nodes.find((node) => node.device_id === scanJob.providerId)
    if (!provider) return
    try {
      const body = await requestCapability(provider, 'coordination.job.cancel', { job_id: scanJob.job_id })
      setScanJob({ providerId: provider.device_id, ...body.payload?.result })
      addActivity({ title: 'Scout cancelled', detail: provider.device_id, tone: 'info' })
    } catch (error) {
      addActivity({ title: 'Cancel failed', detail: error instanceof Error ? error.message : 'Request failed', tone: 'warn' })
    }
  }

  return (
    <div className="app-shell">
      <div className="ambient ambient-one" /><div className="ambient ambient-two" />
      <header className="topbar">
        <div className="brand-mark"><span>R</span></div>
        <div className="brand"><strong>RECONCLAVE</strong><span>OPERATIONS DECK</span></div>
        <div className="topbar-spacer" />
        <div className={`link-state ${connected ? 'online' : ''}`}><i />{connected ? 'LIVE LINK' : 'RECONNECTING'}</div>
        <button className="operator"><span>LOCAL</span><strong>{state.coordinator_id || 'INITIALISING'}</strong></button>
      </header>

      <aside className="rail">
        <nav aria-label="Primary navigation">
          <button className="active" title="Network"><span>⌁</span><small>Network</small></button>
          <button disabled title="Jobs"><span>◫</span><small>Jobs</small></button>
          <button disabled title="Projects"><span>◇</span><small>Projects</small></button>
          <button disabled title="Evidence"><span>▱</span><small>Evidence</small></button>
        </nav>
        <div className="rail-foot"><div className="pulse-ring" /><small>RC/01</small></div>
      </aside>

      <main>
        <section className="hero">
          <div><p className="eyebrow">DISTRIBUTED OPERATIONS</p><h1>Network constellation</h1>
            <p className="subhead">Live capability map across trusted and discoverable nodes.</p></div>
          <div className="hero-time"><span>{state.updated_at_ms ? new Date(state.updated_at_ms).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '--:--'}</span><small>LAST TELEMETRY</small></div>
        </section>

        <section className="metrics">
          <article><span className="metric-icon cyan">⌁</span><div><strong>{state.nodes.length.toString().padStart(2, '0')}</strong><small>ACTIVE NODES</small></div><em>DISCOVERED</em></article>
          <article><span className="metric-icon violet">◇</span><div><strong>{capabilityCount.toString().padStart(2, '0')}</strong><small>CAPABILITIES</small></div><em>AVAILABLE</em></article>
          <article><span className="metric-icon amber">△</span><div><strong>{coordinatorCount.toString().padStart(2, '0')}</strong><small>COORDINATORS</small></div><em>ONLINE</em></article>
        </section>

        {scanJob && <section className={`job-strip ${scanJob.job_status}`}>
          <div className="job-orbit"><span>{scanJob.job_status === 'running' ? '⌁' : '✓'}</span></div>
          <div className="job-title"><span className="kicker">ACTIVE OPERATION</span><strong>NETWORK SCOUT</strong><small>{scanJob.providerId} · job {scanJob.job_id ?? 'pending'}</small></div>
          <div className="job-progress"><div><span style={{ width: `${scanJob.total ? Math.min(100, scanJob.checked / scanJob.total * 100) : 0}%` }} /></div><small>{scanJob.checked} / {scanJob.total} ADDRESSES</small></div>
          <div className="job-hosts"><strong>{scanJob.hosts?.length ?? 0}</strong><small>HOSTS</small></div>
          <span className={`status-pill ${scanJob.job_status}`}><i />{scanJob.job_status}</span>
          {scanJob.error && <small className="job-error">{scanJob.error}</small>}
          {scanJob.job_status === 'running' && state.nodes.find((node) => node.device_id === scanJob.providerId)?.capabilities.includes('coordination.job.cancel') && <button className="abort" onClick={cancelScout}>ABORT</button>}
          {scanJob.job_status !== 'running' && <button className="dismiss" onClick={() => setScanJob(null)}>DISMISS</button>}
        </section>}

        <section className="workspace">
          <div className="roster panel">
            <div className="panel-head"><div><span className="kicker">NODE ROSTER</span><h2>Connected systems</h2></div><span className="count">{nodes.length}</span></div>
            <label className="search"><span>⌕</span><input value={filter} onChange={(event) => setFilter(event.target.value)} placeholder="Filter nodes or capabilities" /></label>
            <div className="node-list">
              {nodes.map((node) => <button key={node.device_id} onClick={() => { setSelectedId(node.device_id); setResult(null) }} className={`node-row ${node.device_id === selected?.device_id ? 'selected' : ''}`}>
                <NodeGlyph node={node} /><span className="node-copy"><strong>{node.device_id}</strong><small>{node.device_type} · {node.address}</small></span>
                <span className="node-tail"><i className={node.status} />{node.local ? 'LOCAL' : timeAgo(node.age_seconds)}</span>
              </button>)}
              {!nodes.length && <div className="empty"><span>⌁</span><strong>No matching signals</strong><small>Clear the filter or wait for discovery.</small></div>}
            </div>
          </div>

          <div className="detail panel">
            {selected ? <>
              <div className="detail-head"><NodeGlyph node={selected} /><div><span className="kicker">SELECTED NODE</span><h2>{selected.device_id}</h2><p>{selected.device_type} · firmware {selected.firmware}</p></div><span className={`status-pill ${selected.status}`}><i />{selected.status}</span></div>
              <div className="facts">
                <div><small>ENDPOINT</small><strong>{selected.address}:{selected.port}</strong></div>
                <div><small>LINK PROFILE</small><strong>{selected.resources.network_mbps ? `${selected.resources.network_mbps} Mbps` : 'Unreported'}</strong></div>
                <div><small>STORAGE FREE</small><strong>{bytes(selected.resources.storage_free_bytes)}</strong></div>
                <div><small>ROLES</small><strong>{selected.roles.join(' / ')}</strong></div>
                <div><small>TRUST</small><strong>{selected.security?.paired ? selected.security.mode ?? 'Paired' : 'Public / unpaired'}</strong></div>
                <div><small>ACTIVE COORDINATOR</small><strong>{selected.security?.active_coordinator ? `${selected.security.active_coordinator} · P${selected.security.active_priority}` : selected.security?.primary_coordinator ?? 'None leased'}</strong></div>
              </div>
              <div className="cap-head"><div><span className="kicker">CAPABILITY MATRIX</span><h3>Available actions</h3></div><span>{selected.capabilities.length} advertised</span></div>
              <div className="capabilities">
                {selected.capabilities.map((capability) => {
                  const meta = capabilityMeta(selected, capability)
                  const isScout = capability === 'net.discovery.scan'
                  const directlyInvokable = capability === 'system.info' || capability === 'desktop.resources' || capability === 'coordination.job.status'
                  return <article key={capability}>
                    <div className="cap-sigil">{capability.split('.').map((part) => part[0]).join('').slice(0, 2).toUpperCase()}</div>
                    <div className="cap-copy"><strong>{capability}</strong><span>v{meta.version} · {meta.permission}</span>{meta.features?.length ? <small>{meta.features.join(' · ')}</small> : null}</div>
                    <button disabled={(!directlyInvokable && !isScout) || !!busyCapability || (isScout && scanJob?.job_status === 'running')} onClick={() => isScout ? configureScout() : invoke(capability)}>{busyCapability === capability ? 'CALLING…' : isScout ? 'CONFIGURE' : directlyInvokable ? 'INVOKE' : 'PLANNED'}</button>
                  </article>
                })}
              </div>
              {result && <div className="result"><div><span className="kicker">LATEST RESPONSE</span><button onClick={() => setResult(null)}>CLOSE</button></div><pre>{JSON.stringify(result, null, 2)}</pre></div>}
            </> : <div className="empty large"><span>⌁</span><strong>Waiting for constellation</strong><small>Reconclave nodes will appear here as they announce.</small></div>}
          </div>

          <div className="activity panel">
            <div className="panel-head"><div><span className="kicker">ACTIVITY STREAM</span><h2>Recent operations</h2></div><span className="live-tag"><i />LIVE</span></div>
            <div className="timeline">
              {activity.map((item) => <article key={item.id}><i className={item.tone} /><div><strong>{item.title}</strong><p>{item.detail}</p></div><time>{item.time.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}</time></article>)}
              {!activity.length && <div className="empty"><span>∿</span><strong>Channel is quiet</strong><small>Node commands and job events will appear here.</small></div>}
            </div>
          </div>
        </section>
      </main>
      {scoutOpen && selected && <div className="modal-shade" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) setScoutOpen(false) }}>
        <section className="scout-modal" role="dialog" aria-modal="true" aria-labelledby="scout-title">
          <div className="modal-head"><div><span className="kicker">SCOPED OPERATION</span><h2 id="scout-title">Configure Network Scout</h2><p>Provider: {selected.device_id}</p></div><button onClick={() => setScoutOpen(false)} aria-label="Close">×</button></div>
          <div className="scope-visual"><span>{scope.start || 'START'}</span><div><i /><i /><i /><i /><i /></div><span>{scope.end || 'END'}</span></div>
          <label className="field"><span>NETWORK / CIDR</span><input value={scope.network} onChange={(event) => setScope({ ...scope, network: event.target.value })} placeholder="192.168.1.0/24" /></label>
          <div className="field-pair">
            <label className="field"><span>FIRST ADDRESS</span><input value={scope.start} onChange={(event) => setScope({ ...scope, start: event.target.value })} /></label>
            <label className="field"><span>LAST ADDRESS</span><input value={scope.end} onChange={(event) => setScope({ ...scope, end: event.target.value })} /></label>
          </div>
          <div className="scope-note"><strong>BOUNDARY ENFORCEMENT</strong><p>The coordinator permits IPv4 /24 or smaller. The provider independently verifies that this scope is locally attached.</p></div>
          <label className="authorise"><input type="checkbox" checked={authorised} onChange={(event) => setAuthorised(event.target.checked)} /><span><strong>I confirm this network is authorised for assessment.</strong><small>This acknowledgement is required for every dispatched Scout operation.</small></span></label>
          {scoutError && <div className="modal-error"><strong>DISPATCH REFUSED</strong><span>{scoutError}</span></div>}
          <div className="modal-actions"><button className="secondary" onClick={() => setScoutOpen(false)}>CANCEL</button><button className="primary" disabled={!authorised || !scope.network || !scope.start || !scope.end || !!busyCapability} onClick={startScout}>{busyCapability ? 'DISPATCHING…' : 'DISPATCH SCOUT'}</button></div>
        </section>
      </div>}
    </div>
  )
}

export default App
