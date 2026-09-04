import { useEffect, useMemo, useState } from 'react'
import type { Activity, AppState, CapabilityDescriptor, ReconNode } from './types'

const emptyState: AppState = { revision: 0, nodes: [], coordinator_id: '', updated_at_ms: 0 }

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

function App() {
  const [state, setState] = useState<AppState>(emptyState)
  const [selectedId, setSelectedId] = useState('')
  const [connected, setConnected] = useState(false)
  const [busyCapability, setBusyCapability] = useState('')
  const [result, setResult] = useState<Record<string, unknown> | null>(null)
  const [activity, setActivity] = useState<Activity[]>([])
  const [filter, setFilter] = useState('')

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

  async function invoke(capability: string) {
    if (!selected) return
    setBusyCapability(capability)
    setResult(null)
    try {
      const response = await fetch(`/api/nodes/${encodeURIComponent(selected.device_id)}/invoke`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ capability, arguments: {} }),
      })
      const body = await response.json()
      if (!response.ok) throw new Error(body.message ?? body.error ?? 'Request failed')
      setResult(body)
      addActivity({ title: capability, detail: `${selected.device_id} returned ${body.payload?.status ?? 'a response'}`, tone: 'ok' })
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Request failed'
      setResult({ error: message })
      addActivity({ title: capability, detail: message, tone: 'warn' })
    } finally {
      setBusyCapability('')
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
              </div>
              <div className="cap-head"><div><span className="kicker">CAPABILITY MATRIX</span><h3>Available actions</h3></div><span>{selected.capabilities.length} advertised</span></div>
              <div className="capabilities">
                {selected.capabilities.map((capability) => {
                  const meta = capabilityMeta(selected, capability)
                  const directlyInvokable = capability === 'system.info' || capability === 'desktop.resources' || capability === 'coordination.job.status'
                  return <article key={capability}>
                    <div className="cap-sigil">{capability.split('.').map((part) => part[0]).join('').slice(0, 2).toUpperCase()}</div>
                    <div className="cap-copy"><strong>{capability}</strong><span>v{meta.version} · {meta.permission}</span>{meta.features?.length ? <small>{meta.features.join(' · ')}</small> : null}</div>
                    <button disabled={!directlyInvokable || !!busyCapability} onClick={() => invoke(capability)}>{busyCapability === capability ? 'CALLING…' : directlyInvokable ? 'INVOKE' : 'CONFIGURE'}</button>
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
    </div>
  )
}

export default App
