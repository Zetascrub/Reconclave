import { useMemo, useState } from 'react'
import type { ArchivedJob, EvidenceRecord, Project, ReconNode, WorkspaceData } from './types'

type View = 'jobs' | 'projects' | 'evidence' | 'map'

function stamp(value: number) { return new Date(value).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }) }

export default function WorkspaceViews({ view, workspace, nodes, projectId, onProject, onCreate }:
  { view: View; workspace: WorkspaceData; nodes: ReconNode[]; projectId: string; onProject: (id: string) => void; onCreate: (name: string, description: string) => Promise<void> }) {
  const [query, setQuery] = useState('')
  const [sort, setSort] = useState<'newest' | 'oldest' | 'name'>('newest')
  const [graph, setGraph] = useState(true)
  const [name, setName] = useState('')
  const [description, setDescription] = useState('')
  const project = workspace.projects.find((item) => item.id === projectId)
  const jobs = useMemo(() => workspace.jobs.filter((item) => !projectId || item.project_id === projectId), [workspace.jobs, projectId])
  const evidence = useMemo(() => workspace.evidence.filter((item) => !projectId || item.project_id === projectId), [workspace.evidence, projectId])
  const filterSort = <T extends { title?: string; name?: string; created_at_ms?: number; captured_at_ms?: number }>(items: T[]) => items.filter((item) => JSON.stringify(item).toLowerCase().includes(query.toLowerCase())).sort((a, b) => {
    if (sort === 'name') return (a.title ?? a.name ?? '').localeCompare(b.title ?? b.name ?? '')
    const av = a.captured_at_ms ?? a.created_at_ms ?? 0; const bv = b.captured_at_ms ?? b.created_at_ms ?? 0
    return sort === 'newest' ? bv - av : av - bv
  })
  const hosts = [...new Set(evidence.flatMap((item) => item.data?.hosts ?? []))]
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
    {view === 'evidence' && <RecordGrid empty="No evidence collected for this project.">{filterSort(evidence).map((item: EvidenceRecord) => <article className="record-card evidence-card" key={item.id}><span className="record-icon">▱</span><div><span className="kicker">{item.kind}</span><h3>{item.title}</h3><p>{item.summary}</p><small>{item.job_id} · {stamp(item.captured_at_ms)}</small></div><span className="count">{item.data?.hosts?.length ?? 1}</span></article>)}</RecordGrid>}
    {view === 'map' && (graph ? <NetworkGraph nodes={nodes} hosts={hosts} /> : <RecordGrid empty="No mapped nodes yet.">{[...nodes.map((node) => ({ id: node.device_id, label: node.device_id, meta: `${node.device_type} · ${node.address}`, live: true })), ...hosts.filter((host) => !nodes.some((node) => node.address === host)).map((host) => ({ id: host, label: host, meta: 'Observed host', live: false }))].filter((item) => JSON.stringify(item).toLowerCase().includes(query.toLowerCase())).map((item) => <article className="record-card" key={item.id}><span className={`record-icon ${item.live ? 'live' : ''}`}>⌁</span><div><span className="kicker">{item.live ? 'LIVE NODE' : 'OBSERVATION'}</span><h3>{item.label}</h3><p>{item.meta}</p></div></article>)}</RecordGrid>)}
  </>
}

function RecordGrid({ children, empty }: { children: React.ReactNode; empty: string }) {
  const count = Array.isArray(children) ? children.length : 1
  return <section className="record-grid">{count ? children : <div className="empty large panel"><span>∿</span><strong>{empty}</strong></div>}</section>
}

function NetworkGraph({ nodes, hosts }: { nodes: ReconNode[]; hosts: string[] }) {
  const points = [...nodes.map((node) => ({ id: node.device_id, label: node.device_type, sub: node.address, live: true })), ...hosts.filter((host) => !nodes.some((node) => node.address === host)).map((host) => ({ id: host, label: host, sub: 'observed', live: false }))].slice(0, 18)
  const radius = 36
  return <section className="network-graph panel"><svg viewBox="0 0 100 70" role="img" aria-label="Project node graph"><defs><filter id="glow"><feGaussianBlur stdDeviation=".7" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>{points.map((point, index) => { const angle = index / Math.max(points.length, 1) * Math.PI * 2 - Math.PI / 2; const x = 50 + Math.cos(angle) * radius; const y = 35 + Math.sin(angle) * 25; return <g key={point.id}><line x1="50" y1="35" x2={x} y2={y} /><circle className={point.live ? 'live' : ''} cx={x} cy={y} r="2.2"/><text x={x} y={y + 5}>{point.label}</text><text className="sub" x={x} y={y + 7.8}>{point.sub}</text></g>})}<circle className="hub" cx="50" cy="35" r="3.2"/><text className="hub-label" x="50" y="42">PROJECT</text></svg>{!points.length && <div className="graph-empty">Run a project Scout operation to populate this map.</div>}</section>
}
