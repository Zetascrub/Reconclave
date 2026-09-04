import { useMemo, useState } from 'react'
import type { ArchivedJob, EvidenceRecord, Project, ReconNode, WorkspaceData } from './types'

type View = 'jobs' | 'projects' | 'evidence' | 'map'

function stamp(value: number) { return new Date(value).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }) }

export default function WorkspaceViews({ view, workspace, nodes, projectId, onProject, onCreate, onInspect }:
  { view: View; workspace: WorkspaceData; nodes: ReconNode[]; projectId: string; onProject: (id: string) => void; onCreate: (name: string, description: string) => Promise<void>; onInspect: (hosts: string[], ports: number[]) => Promise<unknown> }) {
  const [query, setQuery] = useState('')
  const [sort, setSort] = useState<'newest' | 'oldest' | 'name'>('newest')
  const [graph, setGraph] = useState(true)
  const [name, setName] = useState('')
  const [description, setDescription] = useState('')
  const [selectedHosts, setSelectedHosts] = useState<string[]>([])
  const [selectedEvidence, setSelectedEvidence] = useState<EvidenceRecord | null>(null)
  const [ports, setPorts] = useState('22, 53, 80, 443, 445, 1883, 8080, 8443')
  const [authorised, setAuthorised] = useState(false)
  const [inspecting, setInspecting] = useState(false)
  const [inspectionError, setInspectionError] = useState('')
  const project = workspace.projects.find((item) => item.id === projectId)
  const jobs = useMemo(() => workspace.jobs.filter((item) => !projectId || item.project_id === projectId), [workspace.jobs, projectId])
  const evidence = useMemo(() => workspace.evidence.filter((item) => !projectId || item.project_id === projectId), [workspace.evidence, projectId])
  const filterSort = <T extends { title?: string; name?: string; created_at_ms?: number; captured_at_ms?: number }>(items: T[]) => items.filter((item) => JSON.stringify(item).toLowerCase().includes(query.toLowerCase())).sort((a, b) => {
    if (sort === 'name') return (a.title ?? a.name ?? '').localeCompare(b.title ?? b.name ?? '')
    const av = a.captured_at_ms ?? a.created_at_ms ?? 0; const bv = b.captured_at_ms ?? b.created_at_ms ?? 0
    return sort === 'newest' ? bv - av : av - bv
  })
  const hosts = [...new Set(evidence.flatMap((item) => (item.data?.hosts ?? []).map((host) => typeof host === 'string' ? host : host.address)))]
  const mapped = [...nodes.map((node) => ({ id: node.device_id, address: node.address, label: node.device_type, sub: node.address, live: true, node })), ...hosts.filter((host) => !nodes.some((node) => node.address === host)).map((host) => ({ id: host, address: host, label: host, sub: 'observed', live: false }))]
  const selectedHost = selectedHosts.length === 1 ? mapped.find((item) => item.address === selectedHosts[0]) : undefined
  const hostEvidence = selectedHost ? evidence.filter((item) => item.data?.hosts?.some((host) => (typeof host === 'string' ? host : host.address) === selectedHost.address)) : []
  const title = view === 'projects' ? 'Project registry' : view === 'jobs' ? 'Operation history' : view === 'evidence' ? 'Evidence library' : 'Project network map'

  return <>
    <section className="hero"><div><p className="eyebrow">PROJECT INTELLIGENCE</p><h1>{title}</h1><p className="subhead">{project ? project.name : 'All authorised project workspaces'}</p></div></section>
    <section className="workspace-toolbar panel">
      <label><span>PROJECT</span><select value={projectId} onChange={(event) => onProject(event.target.value)}><option value="">All projects</option>{workspace.projects.map((item) => <option key={item.id} value={item.id}>{item.name}</option>)}</select></label>
      <label className="workspace-search"><span>FILTER</span><input value={query} onChange={(event) => setQuery(event.target.value)} placeholder="Search records…" /></label>
      <label><span>SORT</span><select value={sort} onChange={(event) => setSort(event.target.value as typeof sort)}><option value="newest">Newest first</option><option value="oldest">Oldest first</option><option value="name">Name</option></select></label>
      {view === 'map' && <div className="segmented"><button className={graph ? 'active' : ''} onClick={() => setGraph(true)}>GRAPH</button><button className={!graph ? 'active' : ''} onClick={() => setGraph(false)}>LIST</button></div>}
    </section>
    {view === 'projects' && <section className="catalog-layout">
      <div className="panel create-card"><span className="kicker">NEW ENGAGEMENT</span><h2>Create project</h2><label className="field"><span>PROJECT NAME</span><input value={name} onChange={(e) => setName(e.target.value)} /></label><label className="field"><span>DESCRIPTION</span><input value={description} onChange={(e) => setDescription(e.target.value)} /></label><button className="primary-action" disabled={!name.trim()} onClick={async () => { await onCreate(name, description); setName(''); setDescription('') }}>CREATE PROJECT</button></div>
      <RecordGrid empty="No projects yet. Create the first authorised workspace.">{filterSort(workspace.projects).map((item: Project) => <article className="record-card" key={item.id} onClick={() => onProject(item.id)}><span className="record-icon">◇</span><div><span className="kicker">PROJECT</span><h3>{item.name}</h3><p>{item.description || 'No description'}</p><small>{workspace.jobs.filter((job) => job.project_id === item.id).length} jobs · {workspace.evidence.filter((ev) => ev.project_id === item.id).length} evidence · {stamp(item.created_at_ms)}</small></div></article>)}</RecordGrid>
    </section>}
    {view === 'jobs' && <RecordGrid empty="No jobs recorded for this project.">{filterSort(jobs).map((item: ArchivedJob) => <article className="record-card" key={item.id}><span className="record-icon">◫</span><div><span className="kicker">{item.capability ?? 'OPERATION'}</span><h3>{item.id}</h3><p>{item.provider_id} · {item.checked ?? 0}/{item.total ?? 0} checked · {item.hosts?.length ?? 0} hosts</p><small>{stamp(item.updated_at_ms)}</small></div><span className={`status-pill ${item.status}`}><i />{item.status}</span></article>)}</RecordGrid>}
    {view === 'evidence' && <RecordGrid empty="No evidence collected for this project.">{filterSort(evidence).map((item: EvidenceRecord) => <button className="record-card evidence-card" key={item.id} onClick={() => setSelectedEvidence(item)}><span className="record-icon">▱</span><div><span className="kicker">{item.kind}</span><h3>{item.title}</h3><p>{item.summary}</p><small>{item.job_id} · {stamp(item.captured_at_ms)}</small></div><span className="count">{item.data?.hosts?.length ?? 1}</span></button>)}</RecordGrid>}
    {view === 'map' && <section className="map-layout"><div>{graph ? <NetworkGraph points={mapped} selected={selectedHosts} onSelect={(host) => setSelectedHosts((current) => current.includes(host) ? current.filter((item) => item !== host) : [...current, host])} /> : <RecordGrid empty="No mapped nodes yet.">{mapped.filter((item) => JSON.stringify(item).toLowerCase().includes(query.toLowerCase())).map((item) => <button className={`record-card ${selectedHosts.includes(item.address) ? 'selected-record' : ''}`} key={item.id} onClick={() => setSelectedHosts((current) => current.includes(item.address) ? current.filter((host) => host !== item.address) : [...current, item.address])}><span className={`record-icon ${item.live ? 'live' : ''}`}>⌁</span><div><span className="kicker">{item.live ? 'LIVE NODE' : 'OBSERVATION'}</span><h3>{item.address}</h3><p>{item.label}</p></div><span className="selection-box">{selectedHosts.includes(item.address) ? '✓' : '+'}</span></button>)}</RecordGrid>}</div><HostInspector host={selectedHost} selected={selectedHosts} evidence={hostEvidence} project={project} ports={ports} setPorts={setPorts} authorised={authorised} setAuthorised={setAuthorised} busy={inspecting} error={inspectionError} onRun={async () => { const parsed = [...new Set(ports.split(/[\s,]+/).filter(Boolean).map(Number).filter((port) => Number.isInteger(port) && port > 0 && port <= 65535))]; setInspecting(true); setInspectionError(''); try { await onInspect(selectedHosts, parsed) } catch (error) { setInspectionError(error instanceof Error ? error.message : 'Inspection failed') } finally { setInspecting(false) } }} /></section>}
    {selectedEvidence && <div className="modal-shade" onMouseDown={(event) => { if (event.target === event.currentTarget) setSelectedEvidence(null) }}><section className="evidence-viewer panel"><div className="modal-head"><div><span className="kicker">{selectedEvidence.kind}</span><h2>{selectedEvidence.title}</h2><p>{stamp(selectedEvidence.captured_at_ms)}</p></div><button onClick={() => setSelectedEvidence(null)}>×</button></div><p className="evidence-summary">{selectedEvidence.summary}</p><div className="evidence-meta"><span>PROJECT <strong>{workspace.projects.find((item) => item.id === selectedEvidence.project_id)?.name ?? selectedEvidence.project_id}</strong></span><span>JOB <strong>{selectedEvidence.job_id || '—'}</strong></span><span>EVIDENCE ID <strong>{selectedEvidence.id}</strong></span></div><h3>COLLECTED DATA</h3><pre>{JSON.stringify(selectedEvidence.data, null, 2)}</pre></section></div>}
  </>
}

function RecordGrid({ children, empty }: { children: React.ReactNode; empty: string }) {
  const count = Array.isArray(children) ? children.length : 1
  return <section className="record-grid">{count ? children : <div className="empty large panel"><span>∿</span><strong>{empty}</strong></div>}</section>
}

type MapPoint = { id: string; address: string; label: string; sub: string; live: boolean; node?: ReconNode }

function NetworkGraph({ points, selected, onSelect }: { points: MapPoint[]; selected: string[]; onSelect: (host: string) => void }) {
  const visible = points.slice(0, 18)
  const radius = 36
  return <section className="network-graph panel"><svg viewBox="0 0 100 70" role="img" aria-label="Interactive project node graph"><defs><filter id="glow"><feGaussianBlur stdDeviation=".7" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>{visible.map((point, index) => { const angle = index / Math.max(visible.length, 1) * Math.PI * 2 - Math.PI / 2; const x = 50 + Math.cos(angle) * radius; const y = 35 + Math.sin(angle) * 25; const chosen = selected.includes(point.address); return <g className={`graph-node ${chosen ? 'selected' : ''}`} key={point.id} role="button" tabIndex={0} aria-pressed={chosen} onClick={() => onSelect(point.address)} onKeyDown={(event) => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); onSelect(point.address) } }}><line x1="50" y1="35" x2={x} y2={y} /><circle className={point.live ? 'live' : ''} cx={x} cy={y} r={chosen ? '3' : '2.2'}/><text x={x} y={y + 5}>{point.label}</text><text className="sub" x={x} y={y + 7.8}>{point.sub}</text></g>})}<circle className="hub" cx="50" cy="35" r="3.2"/><text className="hub-label" x="50" y="42">PROJECT</text></svg>{!visible.length && <div className="graph-empty">Run a project Scout operation to populate this map.</div>}</section>
}

function HostInspector({ host, selected, evidence, project, ports, setPorts, authorised, setAuthorised, busy, error, onRun }: { host?: MapPoint; selected: string[]; evidence: EvidenceRecord[]; project?: Project; ports: string; setPorts: (value: string) => void; authorised: boolean; setAuthorised: (value: boolean) => void; busy: boolean; error: string; onRun: () => Promise<void> }) {
  const latest = evidence.find((item) => item.kind === 'tcp-services')
  const observation = latest?.data?.hosts?.find((item) => (typeof item === 'string' ? item : item.address) === host?.address)
  const openPorts = typeof observation === 'object' ? observation.open_ports ?? [] : []
  const parsedPorts = ports.split(/[\s,]+/).filter(Boolean).map(Number)
  const validPorts = parsedPorts.length > 0 && parsedPorts.length <= 128 && parsedPorts.every((port) => Number.isInteger(port) && port > 0 && port <= 65535)
  return <aside className="host-inspector panel">
    <div className="inspector-head"><div><span className="kicker">TARGET INSPECTOR</span><h2>{host?.address ?? (selected.length ? `${selected.length} hosts selected` : 'Select a host')}</h2></div><span className="count">{selected.length}</span></div>
    {host ? <><div className="host-state"><span className={`record-icon ${host.live ? 'live' : ''}`}>⌁</span><div><strong>{host.label}</strong><small>{host.live ? 'Active Reconclave node' : 'Previously observed host'}</small></div></div><div className="inspector-facts"><span>ADDRESS<strong>{host.address}</strong></span><span>STATUS<strong>{host.node?.status ?? (host.live ? 'online' : 'not currently advertised')}</strong></span><span>IDENTITY<strong>{host.node?.device_id ?? 'Unidentified'}</strong></span><span>ROLES<strong>{host.node?.roles.join(', ') || 'Observed endpoint'}</strong></span><span>FIRMWARE<strong>{host.node?.firmware ?? 'Unknown'}</strong></span><span>EVIDENCE<strong>{evidence.length} record(s)</strong></span></div>{host.node && <div className="host-capabilities"><span>CAPABILITIES</span>{host.node.capabilities.map((capability) => <i key={capability}>{capability}</i>)}</div>}{latest && <div className="last-inspection"><span>LATEST TCP OBSERVATION</span><strong>{openPorts.length ? openPorts.join(', ') : 'No open ports observed'}</strong><small>{stamp(latest.captured_at_ms)}</small></div>}</> : <div className="inspector-empty">Choose one host for its full identity and evidence history, or choose several to inspect them together.</div>}
    <div className="inspection-form"><div className="section-label"><span>TCP PORT INSPECTION</span><small>Up to 16 local hosts / 128 ports</small></div><div className="port-presets"><button onClick={() => setPorts('22, 53, 80, 443, 445, 1883, 8080, 8443')}>COMMON</button><button onClick={() => setPorts('80, 443, 8000, 8080, 8443, 8765, 8767')}>WEB</button><button onClick={() => setPorts('20, 21, 22, 23, 25, 53, 80, 110, 135, 139, 143, 443, 445, 3389, 5900')}>EXTENDED</button></div><label className="field"><span>PORTS</span><input value={ports} onChange={(event) => setPorts(event.target.value)} aria-invalid={!validPorts} placeholder="22, 80, 443" /></label><label className="authorise compact"><input type="checkbox" checked={authorised} onChange={(event) => setAuthorised(event.target.checked)} /><span><strong>AUTHORISE TARGETED INSPECTION</strong><small>I am authorised to assess the selected local hosts.</small></span></label>{!project && <div className="inspection-error">Select a project above so the inspection and evidence have a case record.</div>}{error && <div className="inspection-error">{error}</div>}<button className="primary-action inspect-action" disabled={!project || !selected.length || !authorised || !validPorts || busy} onClick={onRun}>{busy ? 'INSPECTING…' : `INSPECT ${selected.length || ''} HOST${selected.length === 1 ? '' : 'S'}`}</button></div>
  </aside>
}
