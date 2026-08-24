#!/usr/bin/env python3
# $LicenseInfo:firstyear=2026&license=viewerlgpl$
# Firestorm Viewer Source Code
# Copyright (C) 2026, Firestorm contributors.
# Distributed under the GNU Lesser General Public License, version 2.1.
# $/LicenseInfo$
"""Bidirectional MCP server for a locally running Firestorm viewer.

The viewer publishes an atomic LLSD XML snapshot. This process exposes that
snapshot over MCP stdio and never opens a network socket.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import sys
import threading
import time
import xml.etree.ElementTree as ET
import uuid


SERVER_NAME = "firestorm-local"
SERVER_VERSION = "0.8.0"
DEFAULT_PROTOCOL_VERSION = "2024-11-05"


def default_bridge_dir() -> Path:
    configured = os.environ.get("FIRESTORM_MCP_DIR")
    if configured:
        return Path(configured)
    appdata = os.environ.get("APPDATA")
    if not appdata:
        raise RuntimeError("APPDATA is unavailable and FIRESTORM_MCP_DIR is not set")
    return Path(appdata) / "Firestorm_x64" / "user_settings" / "firestorm_mcp_bridge"


def parse_llsd_value(element: ET.Element):
    tag = element.tag
    text = element.text or ""
    if tag == "map":
        result = {}
        children = list(element)
        index = 0
        while index < len(children):
            key_node = children[index]
            if key_node.tag != "key" or index + 1 >= len(children):
                raise ValueError("Malformed LLSD map")
            result[key_node.text or ""] = parse_llsd_value(children[index + 1])
            index += 2
        return result
    if tag == "array":
        return [parse_llsd_value(child) for child in element]
    if tag in {"string", "uuid", "uri", "date"}:
        return text
    if tag in {"integer", "int"}:
        return int(text or "0")
    if tag == "real":
        return float(text or "0")
    if tag == "boolean":
        return text.strip().lower() in {"1", "true"}
    if tag == "undef":
        return None
    if tag == "binary":
        return text.strip()
    raise ValueError(f"Unsupported LLSD type: {tag}")


def read_llsd(path: Path):
    last_error = None
    for _ in range(3):
        try:
            root = ET.parse(path).getroot()
            if root.tag != "llsd" or not list(root):
                raise ValueError("Not an LLSD document")
            return parse_llsd_value(list(root)[0])
        except (OSError, ET.ParseError, ValueError) as exc:
            last_error = exc
            time.sleep(0.02)
    raise RuntimeError(f"Unable to read {path}: {last_error}")


def append_llsd(parent: ET.Element, value):
    if isinstance(value, dict):
        node = ET.SubElement(parent, "map")
        for key, item in value.items():
            ET.SubElement(node, "key").text = str(key)
            append_llsd(node, item)
    elif isinstance(value, (list, tuple)):
        node = ET.SubElement(parent, "array")
        for item in value:
            append_llsd(node, item)
    elif isinstance(value, bool):
        ET.SubElement(parent, "boolean").text = "true" if value else "false"
    elif isinstance(value, int):
        ET.SubElement(parent, "integer").text = str(value)
    elif isinstance(value, float):
        ET.SubElement(parent, "real").text = repr(value)
    elif value is None:
        ET.SubElement(parent, "undef")
    else:
        ET.SubElement(parent, "string").text = str(value)


def write_llsd_atomic(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    root = ET.Element("llsd")
    append_llsd(root, value)
    temporary = path.with_suffix(path.suffix + ".tmp")
    ET.ElementTree(root).write(temporary, encoding="utf-8", xml_declaration=True)
    os.replace(temporary, path)


def parse_timestamp(value: str | None) -> dt.datetime | None:
    if not value:
        return None
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        return parsed if parsed.tzinfo else parsed.replace(tzinfo=dt.timezone.utc)
    except ValueError:
        return None


def _safe_log_value(value, depth=0):
    """Keep audit records useful without retaining credentials or full script bodies."""
    if depth > 5:
        return "<depth-limit>"
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            lowered = str(key).lower()
            if any(marker in lowered for marker in ("password", "secret", "token", "api_key", "apikey", "source")):
                result[str(key)] = "<redacted>"
            else:
                result[str(key)] = _safe_log_value(item, depth + 1)
        return result
    if isinstance(value, list):
        return [_safe_log_value(item, depth + 1) for item in value[:100]]
    if isinstance(value, str) and len(value) > 1000:
        return value[:1000] + "…<truncated>"
    return value


class MCPActivityJournal:
    """Persistent audit/undo data plus a lightweight snapshot-difference event feed."""

    def __init__(self, bridge_dir: Path):
        self.bridge_dir = bridge_dir
        self.events_path = bridge_dir / "events.jsonl"
        self.audit_path = bridge_dir / "audit.jsonl"
        self.undo_path = bridge_dir / "undo.json"
        self.lock = threading.RLock()
        self.stop_event = threading.Event()
        self.thread = None
        self.cursor = 0
        self.previous = None
        self._load_cursor()

    def _load_cursor(self):
        if not self.events_path.exists():
            return
        try:
            lines = self.events_path.read_text(encoding="utf-8").splitlines()
            if lines:
                self.cursor = int(json.loads(lines[-1]).get("cursor", 0))
        except (OSError, ValueError, json.JSONDecodeError):
            self.cursor = 0

    @staticmethod
    def _append(path: Path, value: dict):
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")

    def add_event(self, event_type: str, data: dict | None = None):
        with self.lock:
            self.cursor += 1
            event = {
                "cursor": self.cursor,
                "time": dt.datetime.now(dt.timezone.utc).isoformat(),
                "type": event_type,
                "data": _safe_log_value(data or {}),
            }
            self._append(self.events_path, event)
            return event

    def audit(self, action: str, arguments: dict, result: dict | None = None, error: str | None = None):
        entry = {
            "time": dt.datetime.now(dt.timezone.utc).isoformat(),
            "action": action,
            "arguments": _safe_log_value(arguments),
            "success": error is None and bool((result or {}).get("success", True)),
            "state": (result or {}).get("state", "failed" if error else "completed"),
            "message": error or (result or {}).get("message", ""),
            "request_id": (result or {}).get("request_id"),
        }
        with self.lock:
            self._append(self.audit_path, entry)

    def read_lines(self, path: Path, limit: int):
        if not path.exists():
            return []
        with self.lock:
            lines = path.read_text(encoding="utf-8").splitlines()[-limit:]
        result = []
        for line in lines:
            try:
                result.append(json.loads(line))
            except json.JSONDecodeError:
                pass
        return result

    def events(self, after_cursor=0, limit=100, event_types=None):
        entries = self.read_lines(self.events_path, 2000)
        allowed = set(event_types or [])
        entries = [entry for entry in entries if int(entry.get("cursor", 0)) > after_cursor]
        if allowed:
            entries = [entry for entry in entries if entry.get("type") in allowed]
        entries = entries[:limit]
        return {
            "events": entries,
            "next_cursor": entries[-1]["cursor"] if entries else after_cursor,
            "latest_cursor": self.cursor,
        }

    def _load_undo(self):
        try:
            value = json.loads(self.undo_path.read_text(encoding="utf-8"))
            return value if isinstance(value, list) else []
        except (OSError, ValueError, json.JSONDecodeError):
            return []

    def _save_undo(self, entries):
        temporary = self.undo_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(entries[-100:], ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, self.undo_path)

    def push_undo(self, action: str, arguments: dict, description: str):
        with self.lock:
            entries = self._load_undo()
            entry = {
                "id": str(uuid.uuid4()),
                "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
                "description": description,
                "action": action,
                "arguments": arguments,
            }
            entries.append(entry)
            self._save_undo(entries)
            return entry

    def list_undo(self, limit=20):
        with self.lock:
            return [{k: v for k, v in entry.items() if k != "arguments"}
                    for entry in reversed(self._load_undo()[-limit:])]

    def peek_undo(self, undo_id=None):
        with self.lock:
            entries = self._load_undo()
            for entry in reversed(entries):
                if undo_id is None or entry.get("id") == undo_id:
                    return entry
        raise RuntimeError("No matching undoable MCP change is available")

    def consume_undo(self, undo_id):
        with self.lock:
            entries = self._load_undo()
            entries = [entry for entry in entries if entry.get("id") != undo_id]
            self._save_undo(entries)

    @staticmethod
    def _signature(status, snapshot):
        context = snapshot.get("selection_context", {}) if snapshot else {}
        return {
            "connected": bool(status.get("enabled") and status.get("fresh")),
            "logged_in": bool((snapshot or {}).get("mcp_bridge", {}).get("viewer_logged_in")),
            "selection": {
                "primary_object_id": context.get("primary_object_id"),
                "primary_root_id": context.get("primary_root_id"),
                "object_count": context.get("object_count", 0),
            },
            "attachments": sorted(str(item.get("object_id")) for item in (snapshot or {}).get("attachments", [])),
            "wearables": sorted(str(item.get("item_id", item.get("name", ""))) for item in (snapshot or {}).get("wearables", [])),
            "animations": sorted(str(item.get("animation_id", item.get("id", item))) for item in (snapshot or {}).get("animations", [])),
            "mesh_upload": {
                key: (snapshot or {}).get("mesh_upload", {}).get(key)
                for key in ("open", "phase", "status_text", "fee_calculated", "upload_allowed", "locally_feasible")
            },
        }

    def observe(self):
        status = {"enabled": False, "fresh": False}
        snapshot = {}
        try:
            status_path = self.bridge_dir / "status.xml"
            if status_path.exists():
                status.update(read_llsd(status_path))
                updated = parse_timestamp(status.get("updated_at"))
                status["fresh"] = bool(updated and (dt.datetime.now(dt.timezone.utc) - updated).total_seconds() <= 5)
            snapshot_path = self.bridge_dir / "snapshot.xml"
            if status.get("fresh") and snapshot_path.exists():
                snapshot = read_llsd(snapshot_path)
        except Exception:
            pass
        current = self._signature(status, snapshot)
        previous = self.previous
        self.previous = current
        if previous is None:
            self.add_event("monitor_started", current)
            return
        for key, event_type in (
            ("connected", "viewer_connection_changed"),
            ("logged_in", "login_state_changed"),
            ("selection", "selection_changed"),
            ("attachments", "attachments_changed"),
            ("wearables", "wearables_changed"),
            ("animations", "animations_changed"),
            ("mesh_upload", "mesh_upload_state_changed"),
        ):
            if previous.get(key) != current.get(key):
                self.add_event(event_type, {"before": previous.get(key), "after": current.get(key)})

    def start(self):
        self.thread = threading.Thread(target=self._watch, name="firestorm-mcp-events", daemon=True)
        self.thread.start()

    def _watch(self):
        while not self.stop_event.is_set():
            self.observe()
            self.stop_event.wait(1.0)

    def stop(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2.0)


class FirestormSnapshot:
    def __init__(self, bridge_dir: Path, journal: MCPActivityJournal | None = None):
        self.bridge_dir = bridge_dir
        self.journal = journal

    @property
    def status_path(self) -> Path:
        return self.bridge_dir / "status.xml"

    @property
    def snapshot_path(self) -> Path:
        return self.bridge_dir / "snapshot.xml"

    @property
    def request_path(self) -> Path:
        return self.bridge_dir / "request.xml"

    @property
    def result_path(self) -> Path:
        return self.bridge_dir / "result.xml"

    def status(self):
        result = {
            "bridge_directory": str(self.bridge_dir),
            "status_file_exists": self.status_path.exists(),
            "snapshot_file_exists": self.snapshot_path.exists(),
        }
        if self.status_path.exists():
            result.update(read_llsd(self.status_path))
        updated = parse_timestamp(result.get("updated_at"))
        if updated:
            result["age_seconds"] = max(
                0.0, (dt.datetime.now(dt.timezone.utc) - updated).total_seconds()
            )
            result["fresh"] = result["age_seconds"] <= 5.0
        else:
            result["fresh"] = False
        return result

    def load(self):
        status = self.status()
        if not status.get("enabled"):
            raise RuntimeError(
                "Firestorm MCP bridge is disabled. Open World > AI Diagnostics and "
                "Repair Assistant and enable the local MCP bridge."
            )
        if not status.get("fresh"):
            raise RuntimeError("Firestorm MCP bridge is not updating; the viewer may not be running")
        if not self.snapshot_path.exists():
            raise RuntimeError("Firestorm has not published a snapshot yet")
        return read_llsd(self.snapshot_path)

    def execute(self, action: str, arguments: dict, timeout: float = 45.0):
        status = self.status()
        if not status.get("enabled") or not status.get("fresh"):
            raise RuntimeError("Firestorm MCP bridge is not enabled and updating")
        if int(status.get("protocol_version", 0)) < 2 or status.get("read_only", True):
            raise RuntimeError("The running Firestorm viewer does not support MCP write commands")
        if self.request_path.exists():
            raise RuntimeError("Another Firestorm MCP write request is already pending")

        request_id = str(uuid.uuid4())
        request = {
            "protocol_version": 7,
            "request_id": request_id,
            "action": action,
            "expires_at": time.time() + timeout,
            **arguments,
        }
        if self.result_path.exists():
            self.result_path.unlink()
        write_llsd_atomic(self.request_path, request)

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.result_path.exists():
                result = read_llsd(self.result_path)
                if result.get("request_id") == request_id:
                    self.result_path.unlink(missing_ok=True)
                    if self.journal:
                        self.journal.audit(action, arguments, result=result)
                    return result
            time.sleep(0.1)
        self.request_path.unlink(missing_ok=True)
        message = "Firestorm did not process the MCP write request before it expired"
        if self.journal:
            self.journal.audit(action, arguments, error=message)
        raise RuntimeError(message)


TOOLS = [
    {
        "name": "firestorm_status",
        "description": "Check whether the local Firestorm viewer MCP bridge is enabled and fresh.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "get_recent_events",
        "description": (
            "Read viewer connection, login, selection, attachment, wearable and animation changes from "
            "the persistent event feed. Pass next_cursor back as after_cursor on the next call."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "after_cursor": {"type": "integer", "minimum": 0, "default": 0},
                "limit": {"type": "integer", "minimum": 1, "maximum": 500, "default": 100},
                "types": {"type": "array", "items": {"type": "string"}, "maxItems": 20},
            },
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "get_mcp_audit_log",
        "description": "Read the latest redacted MCP command audit records. Script bodies and credentials are never retained.",
        "inputSchema": {
            "type": "object",
            "properties": {"limit": {"type": "integer", "minimum": 1, "maximum": 500, "default": 100}},
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "preview_mcp_change",
        "description": (
            "Dry-run a supported change without mutating Firestorm. Returns current and proposed values, "
            "permission/selection checks, warnings and whether the operation can later be undone."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "operation": {
                    "type": "string",
                    "enum": ["set_viewer_setting", "reset_viewer_setting", "set_object_transform",
                             "set_object_face_material", "configure_mesh_upload"],
                },
                "arguments": {"type": "object"},
            },
            "required": ["operation", "arguments"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "list_undoable_mcp_changes",
        "description": "List recent MCP changes that have an exact inverse operation available.",
        "inputSchema": {
            "type": "object",
            "properties": {"limit": {"type": "integer", "minimum": 1, "maximum": 100, "default": 20}},
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "undo_mcp_change",
        "description": (
            "Undo the newest undoable MCP setting, object-transform, face-material or Mesh-uploader configuration change. "
            "Optionally supply an exact undo ID from list_undoable_mcp_changes."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"undo_id": {"type": "string"}},
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "get_selection_context",
        "description": (
            "Return the exact current Firestorm selection: primary child prim, linkset root, "
            "individually selected prims, and selected face indices. Read this before editing a linked object."
        ),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "list_viewer_settings",
        "description": (
            "List Firestorm viewer and per-account settings with names, declared types, comments and metadata. "
            "Sensitive credential settings are listed but their values are always redacted."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "group": {"type": "string", "enum": ["all", "viewer", "account"], "default": "all"},
                "query": {"type": "string", "description": "Case-insensitive name/comment search."},
                "changed_only": {"type": "boolean", "default": False},
                "include_values": {"type": "boolean", "default": False},
                "offset": {"type": "integer", "minimum": 0, "default": 0},
                "limit": {"type": "integer", "minimum": 1, "maximum": 1000, "default": 200},
            },
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "get_viewer_setting",
        "description": "Read one exact Firestorm setting and its default value; credentials remain redacted.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "group": {"type": "string", "enum": ["viewer", "account"]},
                "name": {"type": "string"},
            },
            "required": ["group", "name"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "set_viewer_setting",
        "description": (
            "Change one persisted, user-visible Firestorm setting using its declared value type. "
            "Sensitive credentials and internal/hidden controls are rejected."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "group": {"type": "string", "enum": ["viewer", "account"]},
                "name": {"type": "string"},
                "value": {},
            },
            "required": ["group", "name", "value"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": False},
    },
    {
        "name": "reset_viewer_setting",
        "description": "Restore one persisted, user-visible Firestorm setting to its declared default.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "group": {"type": "string", "enum": ["viewer", "account"]},
                "name": {"type": "string"},
            },
            "required": ["group", "name"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": False},
    },
    {
        "name": "get_mesh_upload_state",
        "description": (
            "Inspect the currently open Mesh upload floater: files, per-LOD model/face/vertex/triangle counts, "
            "rig validity, upload options, validation issues, logs, fee results and overall feasibility."
        ),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": True, "openWorldHint": True},
    },
    {
        "name": "configure_mesh_upload",
        "description": (
            "Safely change common options in the currently open Mesh upload floater. This never selects a local file, "
            "calculates a fee, spends L$, or performs the final upload."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "model_name": {"type": "string", "minLength": 1, "maxLength": 63},
                "upload_textures": {"type": "boolean"},
                "upload_skin": {"type": "boolean"},
                "upload_joints": {"type": "boolean"},
                "lock_scale_if_joint_position": {"type": "boolean"},
                "import_scale": {"type": "number", "exclusiveMinimum": 0},
                "pelvis_offset": {"type": "number"},
            },
            "minProperties": 1,
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "calculate_mesh_upload_fee",
        "description": (
            "Start the uploader's standard server-side weights and fee calculation after local validation passes. "
            "Poll get_mesh_upload_state for completion. This does not perform the final paid upload."
        ),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "get_avatar_wearables",
        "description": "List the avatar's currently worn body parts and clothing inventory items.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "get_avatar_attachments",
        "description": "List current attachments and HUDs with attachment points, transforms, permissions and faces.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "get_selected_objects",
        "description": "Inspect objects currently selected in Firestorm, including transforms and permissions.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "get_object_transform",
        "description": "Get transform and link information for one visible attachment or selected object UUID.",
        "inputSchema": {
            "type": "object",
            "properties": {"object_id": {"type": "string"}},
            "required": ["object_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "get_face_materials",
        "description": "Get legacy texture and PBR material data for one attachment or selected object UUID.",
        "inputSchema": {
            "type": "object",
            "properties": {"object_id": {"type": "string"}},
            "required": ["object_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "get_script_metadata",
        "description": "List readable script inventory metadata for attachments and selected objects; never returns no-mod source.",
        "inputSchema": {
            "type": "object",
            "properties": {"object_id": {"type": "string"}},
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "list_running_animations",
        "description": "List animations currently reported by the local avatar and their sources and priorities.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "get_full_snapshot",
        "description": "Return the complete current read-only Firestorm diagnostic snapshot.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "inspect_object",
        "description": (
            "Inspect any currently loaded object UUID in detail, optionally requesting its task inventory "
            "and returning every prim in its linkset. Reading does not require ownership; unavailable "
            "inventory remains permission- and simulator-gated."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "object_id": {"type": "string"},
                "include_inventory": {"type": "boolean", "default": True},
                "include_linkset": {"type": "boolean", "default": True},
            },
            "required": ["object_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": True},
    },
    {
        "name": "get_inventory_entry",
        "description": "Get detailed metadata, permissions, local Chinese label and full path for one agent-inventory item or folder UUID.",
        "inputSchema": {
            "type": "object",
            "properties": {"entry_id": {"type": "string"}},
            "required": ["entry_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "get_inventory_local_label",
        "description": "Read the client-local label for one agent-inventory item or folder UUID without changing its server-side name.",
        "inputSchema": {
            "type": "object",
            "properties": {"entry_id": {"type": "string"}},
            "required": ["entry_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "set_inventory_local_label",
        "description": "Set a client-local label on one inventory item or folder. Pass an empty label to clear it; the Second Life server name is never changed.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "entry_id": {"type": "string"},
                "label": {"type": "string", "maxLength": 1024},
            },
            "required": ["entry_id", "label"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": False},
    },
    {
        "name": "ai_label_inventory_folder",
        "description": (
            "Use the AI translation configuration in Firestorm to generate local Chinese labels for the selected "
            "folder itself and every descendant folder and item. Existing local labels may be replaced; server names are unchanged."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"folder_id": {"type": "string"}},
            "required": ["folder_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "list_inventory_folder",
        "description": "List a folder in the agent inventory, optionally recursively. Retry if fetch_requested is returned.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "folder_id": {"type": "string", "description": "Omit or use an empty string for inventory root."},
                "recursive": {"type": "boolean", "default": False},
                "include_trash": {"type": "boolean", "default": False},
                "limit": {"type": "integer", "minimum": 1, "maximum": 1000, "default": 200},
            },
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "search_inventory",
        "description": "Search names and descriptions across an agent-inventory subtree. Retry if fetch_requested is returned.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string"},
                "folder_id": {"type": "string", "description": "Omit or use an empty string for inventory root."},
                "include_trash": {"type": "boolean", "default": False},
                "limit": {"type": "integer", "minimum": 1, "maximum": 1000, "default": 200},
            },
            "required": ["query"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "create_inventory_folder",
        "description": "Create a normal folder in the agent inventory.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "parent_id": {"type": "string", "description": "Omit or use an empty string for inventory root."},
                "name": {"type": "string", "minLength": 1, "maxLength": 63},
            },
            "required": ["name"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "rename_inventory_entry",
        "description": "Rename one ordinary agent-inventory item or folder; protected system folders are rejected.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "entry_id": {"type": "string"},
                "name": {"type": "string", "minLength": 1, "maxLength": 63},
            },
            "required": ["entry_id", "name"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "move_inventory_entries",
        "description": "Move up to 100 agent-inventory items or ordinary folders into another folder.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "entry_ids": {"type": "array", "items": {"type": "string"}, "minItems": 1, "maxItems": 100},
                "destination_folder_id": {"type": "string"},
            },
            "required": ["entry_ids", "destination_folder_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "trash_inventory_entries",
        "description": "Move up to 100 agent-inventory entries to Trash. This is recoverable and does not purge them.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "entry_ids": {"type": "array", "items": {"type": "string"}, "minItems": 1, "maxItems": 100},
            },
            "required": ["entry_ids"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "set_object_transform",
        "description": (
            "Directly set position, quaternion rotation and/or scale on the single root object "
            "currently selected in Firestorm. The viewer checks UUID and permissions, then sends "
            "the update to the simulator without a confirmation dialog."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "object_id": {"type": "string"},
                "position": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
                "rotation_quaternion": {"type": "array", "items": {"type": "number"}, "minItems": 4, "maxItems": 4},
                "scale": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
                "reason": {"type": "string", "maxLength": 500},
            },
            "required": ["object_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "set_object_face_material",
        "description": (
            "Directly update one face on a prim in the single linkset currently selected in Firestorm. "
            "Omit face to use the one uniquely selected face in Firestorm. "
            "Supports diffuse texture UUID, PBR material UUID, RGBA color and texture transforms."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "object_id": {"type": "string"},
                "face": {"type": "integer", "minimum": 0},
                "diffuse_texture_id": {"type": "string"},
                "pbr_material_id": {"type": "string", "description": "Use an empty string to clear PBR material."},
                "color": {"type": "array", "items": {"type": "number", "minimum": 0, "maximum": 1}, "minItems": 4, "maxItems": 4},
                "texture_scale": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2},
                "texture_offset": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2},
                "texture_rotation_radians": {"type": "number"},
                "reason": {"type": "string", "maxLength": 500},
            },
            "required": ["object_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "create_object_script",
        "description": (
            "Create and compile an LSL script in a prim of the single linkset currently selected in Firestorm. "
            "A source backup is retained in the agent Scripts inventory folder."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "object_id": {"type": "string"},
                "target_scope": {
                    "type": "string", "enum": ["linkset_root", "exact_prim"], "default": "linkset_root",
                    "description": "Write to the parent/root prim by default; use exact_prim to write to the selected child."
                },
                "name": {"type": "string", "minLength": 1, "maxLength": 63},
                "source": {"type": "string", "maxLength": 262144},
                "running": {"type": "boolean", "default": True},
                "reason": {"type": "string", "maxLength": 500},
            },
            "required": ["object_id", "name", "source"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "read_object_script",
        "description": (
            "Read source for an LSL script in the selected linkset. Viewer-standard effective copy and modify "
            "permissions are required, including permissions granted through the active group; ownership itself is not required."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "object_id": {"type": "string"}, "item_id": {"type": "string"},
                "target_scope": {"type": "string", "enum": ["exact_prim", "linkset_root"], "default": "exact_prim"},
            },
            "required": ["object_id", "item_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": True},
    },
    {
        "name": "update_object_script",
        "description": (
            "Replace and compile the same modifiable LSL script item in the selected linkset. This updates in place "
            "and preserves its item UUID; it does not delete and recreate the script."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "object_id": {"type": "string"},
                "item_id": {"type": "string"},
                "target_scope": {"type": "string", "enum": ["exact_prim", "linkset_root"], "default": "exact_prim"},
                "source": {"type": "string", "maxLength": 262144},
                "running": {"type": "boolean", "default": True},
                "reason": {"type": "string", "maxLength": 500},
            },
            "required": ["object_id", "item_id", "source"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "patch_object_script",
        "description": (
            "Read a copy-and-modify LSL script, verify exact old text, apply one or more replacements, then compile "
            "back into the same item UUID. Use this for safe read-before-edit changes without deleting the script."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "object_id": {"type": "string"},
                "item_id": {"type": "string"},
                "target_scope": {"type": "string", "enum": ["exact_prim", "linkset_root"], "default": "exact_prim"},
                "edits": {
                    "type": "array", "minItems": 1, "maxItems": 100,
                    "items": {
                        "type": "object",
                        "properties": {
                            "old_text": {"type": "string", "minLength": 1},
                            "new_text": {"type": "string"},
                            "replace_all": {"type": "boolean", "default": False},
                            "expected_matches": {"type": "integer", "minimum": 1},
                        },
                        "required": ["old_text", "new_text"],
                        "additionalProperties": False,
                    },
                },
                "running": {"type": "boolean", "default": True},
                "reason": {"type": "string", "maxLength": 500},
            },
            "required": ["object_id", "item_id", "edits"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "delete_object_script",
        "description": "Permanently remove one modifiable LSL script from the single selected root object.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "object_id": {"type": "string"}, "item_id": {"type": "string"},
                "target_scope": {"type": "string", "enum": ["exact_prim", "linkset_root"], "default": "exact_prim"},
                "reason": {"type": "string", "maxLength": 500},
            },
            "required": ["object_id", "item_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "set_object_script_running",
        "description": "Start or stop one modifiable LSL script in the single selected root object.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "object_id": {"type": "string"}, "item_id": {"type": "string"},
                "target_scope": {"type": "string", "enum": ["exact_prim", "linkset_root"], "default": "exact_prim"},
                "running": {"type": "boolean"}, "reason": {"type": "string", "maxLength": 500},
            },
            "required": ["object_id", "item_id", "running"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "reset_object_script",
        "description": "Reset one modifiable LSL script in the single selected root object.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "object_id": {"type": "string"}, "item_id": {"type": "string"},
                "target_scope": {"type": "string", "enum": ["exact_prim", "linkset_root"], "default": "exact_prim"},
                "reason": {"type": "string", "maxLength": 500},
            },
            "required": ["object_id", "item_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
]


def all_objects(snapshot):
    return list(snapshot.get("attachments", [])) + list(snapshot.get("selected_objects", []))


def find_object(snapshot, object_id: str):
    wanted = object_id.lower()
    for obj in all_objects(snapshot):
        if str(obj.get("object_id", "")).lower() == wanted:
            return obj
    raise RuntimeError(f"Object {object_id} is not a current attachment or selected object")


def preview_change(store: FirestormSnapshot, snapshot: dict, operation: str, arguments: dict):
    plan = {
        "dry_run": True,
        "operation": operation,
        "feasible": True,
        "changes": [],
        "warnings": [],
        "reversible": operation in {
            "set_viewer_setting", "reset_viewer_setting", "set_object_transform",
            "set_object_face_material", "configure_mesh_upload",
        },
    }
    if operation in {"set_viewer_setting", "reset_viewer_setting"}:
        current = store.execute("settings_get", {
            "group": arguments.get("group", ""), "name": arguments.get("name", "")
        }).get("setting", {})
        target = current.get("default") if operation == "reset_viewer_setting" else arguments.get("value")
        plan["changes"].append({"field": "value", "before": current.get("value"), "after": target})
        plan["setting"] = {key: current.get(key) for key in ("group", "name", "type", "writable", "sensitive")}
        plan["feasible"] = bool(current.get("writable"))
        if not plan["feasible"]:
            plan["warnings"].append("The viewer reports that this setting is not MCP-writable")
        return plan

    if operation == "set_object_transform":
        obj = find_object(snapshot, str(arguments.get("object_id", "")))
        context = snapshot.get("selection_context", {})
        selected_root = str(context.get("primary_root_id", "")).lower()
        object_id = str(obj.get("object_id", ""))
        for key in ("position", "rotation_quaternion", "scale"):
            if key in arguments:
                plan["changes"].append({"field": key, "before": obj.get(key), "after": arguments[key]})
        permissions = obj.get("permissions", {})
        plan["checks"] = {
            "selected_root_matches": selected_root == object_id.lower() and bool(obj.get("is_root")),
            "move_permission": bool(permissions.get("move")),
            "modify_permission": bool(permissions.get("modify")),
        }
        plan["feasible"] = all(plan["checks"].values()) and bool(plan["changes"])
        if not plan["checks"]["selected_root_matches"]:
            plan["warnings"].append("Transform writes require this exact linkset root to be selected")
        return plan

    if operation == "set_object_face_material":
        obj = find_object(snapshot, str(arguments.get("object_id", "")))
        face_index = arguments.get("face")
        if face_index is None:
            selected = obj.get("selected_faces", [])
            if len(selected) == 1:
                face_index = selected[0]
        face = next((item for item in obj.get("faces", []) if item.get("face") == face_index), None)
        mapping = {
            "diffuse_texture_id": "diffuse_texture_id", "pbr_material_id": "pbr_material_id",
            "color": "color", "texture_scale": "scale", "texture_offset": "offset",
            "texture_rotation_radians": "rotation_radians",
        }
        if face:
            for supplied, current_key in mapping.items():
                if supplied in arguments:
                    before = face.get(current_key)
                    if supplied in {"texture_scale", "texture_offset"} and isinstance(before, list):
                        before = before[:2]
                    plan["changes"].append({"field": supplied, "before": before, "after": arguments[supplied]})
        plan["face"] = face_index
        plan["checks"] = {
            "face_resolved": face is not None,
            "modify_permission": bool(obj.get("permissions", {}).get("modify")),
        }
        plan["feasible"] = all(plan["checks"].values()) and bool(plan["changes"])
        if face is None:
            plan["warnings"].append("Supply a valid face or select exactly one face in Firestorm")
        return plan

    if operation == "configure_mesh_upload":
        state = store.execute("mesh_upload_state", {}).get("upload", {})
        current = dict(state.get("options", {}))
        current["model_name"] = state.get("model_name")
        for key, target in arguments.items():
            if key in current:
                plan["changes"].append({"field": key, "before": current.get(key), "after": target})
        plan["phase"] = state.get("phase")
        plan["issues"] = state.get("issues", [])
        plan["feasible"] = bool(state.get("open")) and bool(plan["changes"])
        if not state.get("open"):
            plan["warnings"].append("Open the Mesh upload floater before applying this plan")
        return plan

    raise RuntimeError(f"Unsupported preview operation: {operation}")


def face_undo_arguments(snapshot: dict, arguments: dict):
    obj = find_object(snapshot, str(arguments["object_id"]))
    face_index = arguments.get("face")
    if face_index is None and len(obj.get("selected_faces", [])) == 1:
        face_index = obj["selected_faces"][0]
    face = next((item for item in obj.get("faces", []) if item.get("face") == face_index), None)
    if face is None:
        return None
    undo = {"object_id": arguments["object_id"], "face": face_index}
    mapping = {
        "diffuse_texture_id": ("diffuse_texture_id", None),
        "pbr_material_id": ("pbr_material_id", None),
        "color": ("color", None),
        "texture_scale": ("scale", 2),
        "texture_offset": ("offset", 2),
        "texture_rotation_radians": ("rotation_radians", None),
    }
    for supplied, (current_key, length) in mapping.items():
        if supplied in arguments:
            value = face.get(current_key)
            undo[supplied] = value[:length] if length and isinstance(value, list) else value
    return undo


def call_tool(store: FirestormSnapshot, name: str, arguments: dict):
    if name == "firestorm_status":
        return store.status()

    journal = store.journal
    if name == "get_recent_events":
        return journal.events(
            int(arguments.get("after_cursor", 0)), int(arguments.get("limit", 100)), arguments.get("types")
        )
    if name == "get_mcp_audit_log":
        return {"entries": journal.read_lines(journal.audit_path, int(arguments.get("limit", 100)))}
    if name == "preview_mcp_change":
        return preview_change(store, store.load(), arguments["operation"], arguments["arguments"])
    if name == "list_undoable_mcp_changes":
        return {"changes": journal.list_undo(int(arguments.get("limit", 20)))}

    snapshot = store.load()
    if name == "undo_mcp_change":
        entry = journal.peek_undo(arguments.get("undo_id"))
        result = store.execute(entry["action"], entry["arguments"])
        if result.get("success"):
            journal.consume_undo(entry["id"])
            journal.add_event("mcp_change_undone", {"undo_id": entry["id"], "description": entry["description"]})
        result["undone"] = {key: value for key, value in entry.items() if key != "arguments"}
        return result
    if name == "get_selection_context":
        return {
            "captured_at": snapshot.get("captured_at"),
            "selection_context": snapshot.get("selection_context", {}),
            "selected_objects": snapshot.get("selected_objects", []),
        }
    if name == "list_viewer_settings":
        fields = {key: arguments[key] for key in
                  ("group", "query", "changed_only", "include_values", "offset", "limit")
                  if key in arguments}
        return store.execute("settings_list", fields)
    if name == "get_viewer_setting":
        return store.execute("settings_get", {
            "group": arguments["group"], "name": arguments["name"]
        })
    if name == "set_viewer_setting":
        fields = {
            "group": arguments["group"], "name": arguments["name"], "value": arguments["value"]
        }
        previous = store.execute("settings_get", {
            "group": arguments["group"], "name": arguments["name"]
        }).get("setting", {})
        result = store.execute("settings_set", fields)
        if result.get("success") and "value" in previous:
            journal.push_undo("settings_set", {
                "group": arguments["group"], "name": arguments["name"], "value": previous["value"]
            }, f"Restore setting {arguments['group']}:{arguments['name']}")
        return result
    if name == "reset_viewer_setting":
        previous = store.execute("settings_get", {
            "group": arguments["group"], "name": arguments["name"]
        }).get("setting", {})
        result = store.execute("settings_reset", {
            "group": arguments["group"], "name": arguments["name"]
        })
        if result.get("success") and "value" in previous:
            journal.push_undo("settings_set", {
                "group": arguments["group"], "name": arguments["name"], "value": previous["value"]
            }, f"Restore setting {arguments['group']}:{arguments['name']}")
        return result
    if name == "get_mesh_upload_state":
        return store.execute("mesh_upload_state", {})
    if name == "configure_mesh_upload":
        previous_state = store.execute("mesh_upload_state", {}).get("upload", {})
        previous = dict(previous_state.get("options", {}))
        previous["model_name"] = previous_state.get("model_name")
        undo_settings = {key: previous[key] for key in arguments if key in previous and previous[key] is not None}
        result = store.execute("mesh_upload_configure", {"settings": arguments})
        if result.get("success") and undo_settings:
            journal.push_undo("mesh_upload_configure", {"settings": undo_settings},
                              "Restore Mesh uploader configuration")
        return result
    if name == "calculate_mesh_upload_fee":
        return store.execute("mesh_upload_calculate", {})
    if name == "get_avatar_wearables":
        return {"captured_at": snapshot.get("captured_at"), "wearables": snapshot.get("wearables", [])}
    if name == "get_avatar_attachments":
        return {"captured_at": snapshot.get("captured_at"), "attachments": snapshot.get("attachments", [])}
    if name == "get_selected_objects":
        return {"captured_at": snapshot.get("captured_at"), "selected_objects": snapshot.get("selected_objects", [])}
    if name == "get_object_transform":
        obj = find_object(snapshot, str(arguments["object_id"]))
        keys = (
            "object_id", "name", "position", "rotation_quaternion", "scale",
            "attachment_point", "is_attachment", "is_mesh", "is_rigged_mesh",
            "is_root", "link_children", "permissions",
        )
        return {key: obj.get(key) for key in keys if key in obj}
    if name == "get_face_materials":
        obj = find_object(snapshot, str(arguments["object_id"]))
        return {
            "object_id": obj.get("object_id"),
            "name": obj.get("name"),
            "permissions": obj.get("permissions"),
            "faces": obj.get("faces", []),
        }
    if name == "get_script_metadata":
        wanted = str(arguments.get("object_id", "")).lower()
        result = []
        for obj in all_objects(snapshot):
            if wanted and str(obj.get("object_id", "")).lower() != wanted:
                continue
            scripts = [
                item for item in obj.get("inventory", [])
                if str(item.get("asset_type", "")).lower() in {"lsltext", "script"}
            ]
            if scripts or obj.get("inventory_loaded") is False:
                result.append({
                    "object_id": obj.get("object_id"),
                    "name": obj.get("name"),
                    "inventory_loaded": obj.get("inventory_loaded"),
                    "scripts": scripts,
                    "source_access": "metadata_only",
                })
        return {"objects": result}
    if name == "list_running_animations":
        return {"captured_at": snapshot.get("captured_at"), "animations": snapshot.get("animations", [])}
    if name == "get_full_snapshot":
        return snapshot
    if name == "inspect_object":
        fields = {key: arguments[key] for key in
                  ("object_id", "include_inventory", "include_linkset") if key in arguments}
        return store.execute("inspect_object", fields)
    if name == "get_inventory_entry":
        return store.execute("inventory_get", {"entry_id": arguments["entry_id"]})
    if name == "get_inventory_local_label":
        result = store.execute("inventory_get", {"entry_id": arguments["entry_id"]})
        if not result.get("success"):
            return result
        entry = result.get("entry", {})
        return {
            "success": result.get("success", False),
            "entry_id": arguments["entry_id"],
            "name": entry.get("name"),
            "kind": entry.get("kind"),
            "local_label": entry.get("local_label", ""),
        }
    if name == "set_inventory_local_label":
        return store.execute("inventory_local_label_set", {
            "entry_id": arguments["entry_id"], "label": arguments["label"]
        })
    if name == "ai_label_inventory_folder":
        return store.execute("inventory_ai_label_folder", {
            "folder_id": arguments["folder_id"]
        }, timeout=120.0)
    if name == "list_inventory_folder":
        fields = {key: arguments[key] for key in
                  ("folder_id", "recursive", "include_trash", "limit") if key in arguments}
        return store.execute("inventory_list", fields)
    if name == "search_inventory":
        fields = {key: arguments[key] for key in
                  ("query", "folder_id", "include_trash", "limit") if key in arguments}
        return store.execute("inventory_search", fields)
    if name == "create_inventory_folder":
        fields = {key: arguments[key] for key in ("parent_id", "name") if key in arguments}
        return store.execute("inventory_create_folder", fields)
    if name == "rename_inventory_entry":
        return store.execute("inventory_rename", {
            "entry_id": arguments["entry_id"], "name": arguments["name"]
        })
    if name == "move_inventory_entries":
        return store.execute("inventory_move", {
            "entry_ids": arguments["entry_ids"],
            "destination_folder_id": arguments["destination_folder_id"],
        })
    if name == "trash_inventory_entries":
        return store.execute("inventory_trash", {"entry_ids": arguments["entry_ids"]})
    if name == "set_object_transform":
        fields = {key: arguments[key] for key in
                  ("object_id", "position", "rotation_quaternion", "scale", "reason")
                  if key in arguments}
        if not any(key in fields for key in ("position", "rotation_quaternion", "scale")):
            raise RuntimeError("Provide position, rotation_quaternion and/or scale")
        obj = find_object(snapshot, str(arguments["object_id"]))
        undo = {"object_id": arguments["object_id"]}
        for key in ("position", "rotation_quaternion", "scale"):
            if key in fields and key in obj:
                undo[key] = obj[key]
        result = store.execute("set_object_transform", fields)
        if result.get("success"):
            journal.push_undo("set_object_transform", undo,
                              f"Restore transform of {obj.get('name') or obj.get('object_id')}")
        return result
    if name == "set_object_face_material":
        allowed = (
            "object_id", "face", "diffuse_texture_id", "pbr_material_id", "color",
            "texture_scale", "texture_offset", "texture_rotation_radians", "reason",
        )
        fields = {key: arguments[key] for key in allowed if key in arguments}
        material_fields = set(fields) - {"object_id", "face", "reason"}
        if not material_fields:
            raise RuntimeError("Provide at least one face material field")
        undo = face_undo_arguments(snapshot, fields)
        result = store.execute("set_object_face_material", fields)
        if result.get("success") and undo:
            journal.push_undo("set_object_face_material", undo,
                              f"Restore face {undo.get('face')} material on {arguments['object_id']}")
        return result
    if name in {
        "create_object_script", "read_object_script", "update_object_script", "patch_object_script",
        "delete_object_script", "set_object_script_running", "reset_object_script",
    }:
        allowed = ("object_id", "item_id", "name", "source", "edits", "running", "reason", "target_scope")
        fields = {key: arguments[key] for key in allowed if key in arguments}
        return store.execute(name, fields)
    raise RuntimeError(f"Unknown tool: {name}")


def tool_result(value, *, is_error: bool = False):
    text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=2)
    result = {"content": [{"type": "text", "text": text}]}
    if is_error:
        result["isError"] = True
    return result


def response(request_id, result=None, error=None):
    message = {"jsonrpc": "2.0", "id": request_id}
    if error is not None:
        message["error"] = error
    else:
        message["result"] = result
    sys.stdout.write(json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def serve(bridge_dir: Path):
    journal = MCPActivityJournal(bridge_dir)
    store = FirestormSnapshot(bridge_dir, journal)
    journal.start()
    try:
        for raw_line in sys.stdin.buffer:
            try:
                request = json.loads(raw_line)
                method = request.get("method")
                request_id = request.get("id")
                if request_id is None:
                    continue
                if method == "initialize":
                    requested = request.get("params", {}).get("protocolVersion")
                    response(request_id, {
                        "protocolVersion": requested or DEFAULT_PROTOCOL_VERSION,
                        "capabilities": {"tools": {"listChanged": False}},
                        "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                        "instructions": (
                            "Read detailed local Firestorm state, inspect loaded linksets, organize inventory, "
                            "preview changes before writing, follow the incremental event cursor, and use the "
                            "undo/audit tools for reversible operations. Obey viewer and simulator permissions."
                        ),
                    })
                elif method == "ping":
                    response(request_id, {})
                elif method == "tools/list":
                    response(request_id, {"tools": TOOLS})
                elif method == "tools/call":
                    params = request.get("params", {})
                    try:
                        value = call_tool(store, params.get("name", ""), params.get("arguments") or {})
                        response(request_id, tool_result(value))
                    except Exception as exc:
                        response(request_id, tool_result(str(exc), is_error=True))
                elif method in {"resources/list", "prompts/list"}:
                    response(request_id, {"resources": []} if method == "resources/list" else {"prompts": []})
                else:
                    response(request_id, error={"code": -32601, "message": f"Method not found: {method}"})
            except Exception as exc:
                request_id = request.get("id") if isinstance(locals().get("request"), dict) else None
                if request_id is not None:
                    response(request_id, error={"code": -32603, "message": str(exc)})
    finally:
        journal.stop()


def main():
    parser = argparse.ArgumentParser(description="Firestorm local MCP server")
    parser.add_argument("--bridge-dir", type=Path, default=default_bridge_dir())
    args = parser.parse_args()
    serve(args.bridge_dir.resolve())


if __name__ == "__main__":
    main()
