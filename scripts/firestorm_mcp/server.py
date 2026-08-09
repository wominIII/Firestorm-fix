#!/usr/bin/env python3
"""Read-only MCP server for a locally running Firestorm viewer.

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
import time
import xml.etree.ElementTree as ET


SERVER_NAME = "firestorm-local"
SERVER_VERSION = "0.1.0"
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


def parse_timestamp(value: str | None) -> dt.datetime | None:
    if not value:
        return None
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        return parsed if parsed.tzinfo else parsed.replace(tzinfo=dt.timezone.utc)
    except ValueError:
        return None


class FirestormSnapshot:
    def __init__(self, bridge_dir: Path):
        self.bridge_dir = bridge_dir

    @property
    def status_path(self) -> Path:
        return self.bridge_dir / "status.xml"

    @property
    def snapshot_path(self) -> Path:
        return self.bridge_dir / "snapshot.xml"

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


TOOLS = [
    {
        "name": "firestorm_status",
        "description": "Check whether the local Firestorm viewer MCP bridge is enabled and fresh.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
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
]


def all_objects(snapshot):
    return list(snapshot.get("attachments", [])) + list(snapshot.get("selected_objects", []))


def find_object(snapshot, object_id: str):
    wanted = object_id.lower()
    for obj in all_objects(snapshot):
        if str(obj.get("object_id", "")).lower() == wanted:
            return obj
    raise RuntimeError(f"Object {object_id} is not a current attachment or selected object")


def call_tool(store: FirestormSnapshot, name: str, arguments: dict):
    if name == "firestorm_status":
        return store.status()

    snapshot = store.load()
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
    store = FirestormSnapshot(bridge_dir)
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
                        "Read-only access to the local Firestorm viewer snapshot. "
                        "The viewer must be running with its local MCP bridge enabled."
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


def main():
    parser = argparse.ArgumentParser(description="Firestorm local MCP server")
    parser.add_argument("--bridge-dir", type=Path, default=default_bridge_dir())
    args = parser.parse_args()
    serve(args.bridge_dir.resolve())


if __name__ == "__main__":
    main()
