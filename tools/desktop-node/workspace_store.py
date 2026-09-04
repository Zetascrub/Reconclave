"""Durable local project, job, and evidence index for the desktop coordinator."""

from __future__ import annotations

import json
import os
import pathlib
import threading
import time
import uuid


class WorkspaceStore:
    def __init__(self, path: pathlib.Path) -> None:
        self.path = path
        self.lock = threading.RLock()
        self.data = {"revision": 0, "projects": [], "jobs": [], "evidence": [], "automations": []}
        self._load()

    def _load(self) -> None:
        if not self.path.is_file():
            return
        document = json.loads(self.path.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            raise ValueError("workspace root must be an object")
        for name in ("projects", "jobs", "evidence", "automations"):
            if not isinstance(document.get(name, []), list):
                raise ValueError(f"workspace {name} must be a list")
        self.data = {"revision": int(document.get("revision", 0)),
                     "projects": document.get("projects", []),
                     "jobs": document.get("jobs", []),
                     "evidence": document.get("evidence", []),
                     "automations": document.get("automations", [])}

    def snapshot(self) -> dict:
        with self.lock:
            return json.loads(json.dumps(self.data))

    def _save(self) -> None:
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        temporary = self.path.with_suffix(".tmp")
        temporary.write_text(json.dumps(self.data, indent=2) + "\n", encoding="utf-8")
        temporary.chmod(0o600)
        os.replace(temporary, self.path)

    def _commit(self) -> dict:
        self.data["revision"] += 1
        self._save()
        return self.snapshot()

    def create_project(self, body: dict) -> dict:
        name = str(body.get("name", "")).strip()
        if not name or len(name) > 80:
            raise ValueError("project name must contain 1-80 characters")
        now = int(time.time() * 1000)
        project = {"id": f"project-{uuid.uuid4().hex[:12]}", "name": name,
                   "description": str(body.get("description", "")).strip()[:500],
                   "created_at_ms": now, "updated_at_ms": now}
        with self.lock:
            self.data["projects"].append(project)
            self._commit()
        return project

    def upsert_job(self, body: dict) -> dict:
        job_id = str(body.get("id", "")).strip()
        project_id = str(body.get("project_id", "")).strip()
        if not job_id or not project_id:
            raise ValueError("job id and project_id are required")
        with self.lock:
            if not any(item.get("id") == project_id for item in self.data["projects"]):
                raise ValueError("project does not exist")
            now = int(time.time() * 1000)
            existing = next((item for item in self.data["jobs"] if item.get("id") == job_id), None)
            safe = {key: body.get(key) for key in (
                "project_id", "provider_id", "capability", "status", "checked", "total",
                "hosts", "scope", "error") if key in body}
            if existing is None:
                existing = {"id": job_id, "created_at_ms": now}
                self.data["jobs"].append(existing)
            existing.update(safe)
            existing["updated_at_ms"] = now
            self._commit()
            return json.loads(json.dumps(existing))

    def add_evidence(self, body: dict) -> dict:
        project_id = str(body.get("project_id", "")).strip()
        if not project_id:
            raise ValueError("project_id is required")
        with self.lock:
            if not any(item.get("id") == project_id for item in self.data["projects"]):
                raise ValueError("project does not exist")
            evidence_id = str(body.get("id", "")).strip() or f"evidence-{uuid.uuid4().hex[:16]}"
            existing = next((item for item in self.data["evidence"] if item.get("id") == evidence_id), None)
            if existing is not None:
                return json.loads(json.dumps(existing))
            record = {"id": evidence_id, "project_id": project_id,
                      "job_id": str(body.get("job_id", "")),
                      "kind": str(body.get("kind", "observation"))[:48],
                      "title": str(body.get("title", "Evidence"))[:120],
                      "summary": str(body.get("summary", ""))[:1000],
                      "data": body.get("data", {}),
                      "captured_at_ms": int(body.get("captured_at_ms", time.time() * 1000))}
            self.data["evidence"].append(record)
            self._commit()
            return json.loads(json.dumps(record))

    def create_automation(self, body: dict) -> dict:
        project_id = str(body.get("project_id", "")).strip()
        node_id = str(body.get("node_id", "")).strip()
        condition = str(body.get("condition", ""))
        playbook = str(body.get("playbook", ""))
        if condition not in ("dhcp_assigned", "internet_possible"):
            raise ValueError("unsupported automation condition")
        if playbook not in ("network_scout", "system_snapshot"):
            raise ValueError("unsupported automation playbook")
        interval_ms = int(body.get("interval_ms", 0))
        if interval_ms and not 10000 <= interval_ms <= 86400000:
            raise ValueError("recurring interval must be 10 seconds to 24 hours")
        with self.lock:
            if not any(item.get("id") == project_id for item in self.data["projects"]):
                raise ValueError("project does not exist")
            now = int(time.time() * 1000)
            rule = {"id": f"rule-{uuid.uuid4().hex[:12]}", "project_id": project_id,
                    "node_id": node_id, "condition": condition, "playbook": playbook,
                    "interval_ms": interval_ms, "enabled": True, "created_at_ms": now,
                    "updated_at_ms": now, "last_triggered_ms": 0, "last_error": ""}
            self.data["automations"].append(rule)
            self._commit()
            return json.loads(json.dumps(rule))

    def set_automation(self, rule_id: str, body: dict) -> dict:
        with self.lock:
            rule = next((item for item in self.data["automations"] if item.get("id") == rule_id), None)
            if rule is None:
                raise KeyError(rule_id)
            if "enabled" in body:
                rule["enabled"] = body["enabled"] is True
            for key in ("last_triggered_ms", "last_error"):
                if key in body:
                    rule[key] = body[key]
            rule["updated_at_ms"] = int(time.time() * 1000)
            self._commit()
            return json.loads(json.dumps(rule))

    def delete_automation(self, rule_id: str) -> dict:
        with self.lock:
            before = len(self.data["automations"])
            self.data["automations"] = [item for item in self.data["automations"] if item.get("id") != rule_id]
            if len(self.data["automations"]) == before:
                raise KeyError(rule_id)
            self._commit()
            return {"deleted": rule_id}
