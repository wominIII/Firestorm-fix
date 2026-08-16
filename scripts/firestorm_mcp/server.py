#!/usr/bin/env python3
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
import time
import xml.etree.ElementTree as ET
import uuid


SERVER_NAME = "firestorm-local"
SERVER_VERSION = "0.4.0"
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


class FirestormSnapshot:
    def __init__(self, bridge_dir: Path):
        self.bridge_dir = bridge_dir

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
            "protocol_version": 4,
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
                    return result
            time.sleep(0.1)
        self.request_path.unlink(missing_ok=True)
        raise RuntimeError("Firestorm did not process the MCP write request before it expired")


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
        "description": "Get detailed metadata, permissions and full path for one agent-inventory item or folder UUID.",
        "inputSchema": {
            "type": "object",
            "properties": {"entry_id": {"type": "string"}},
            "required": ["entry_id"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
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
            "required": ["object_id", "face"],
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
            "properties": {"object_id": {"type": "string"}, "item_id": {"type": "string"}},
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
    if name == "inspect_object":
        fields = {key: arguments[key] for key in
                  ("object_id", "include_inventory", "include_linkset") if key in arguments}
        return store.execute("inspect_object", fields)
    if name == "get_inventory_entry":
        return store.execute("inventory_get", {"entry_id": arguments["entry_id"]})
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
        return store.execute("set_object_transform", fields)
    if name == "set_object_face_material":
        allowed = (
            "object_id", "face", "diffuse_texture_id", "pbr_material_id", "color",
            "texture_scale", "texture_offset", "texture_rotation_radians", "reason",
        )
        fields = {key: arguments[key] for key in allowed if key in arguments}
        material_fields = set(fields) - {"object_id", "face", "reason"}
        if not material_fields:
            raise RuntimeError("Provide at least one face material field")
        return store.execute("set_object_face_material", fields)
    if name in {
        "create_object_script", "read_object_script", "update_object_script", "patch_object_script",
        "delete_object_script", "set_object_script_running", "reset_object_script",
    }:
        allowed = ("object_id", "item_id", "name", "source", "edits", "running", "reason")
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
                        "Read detailed local Firestorm state, inspect loaded linksets, organize the agent "
                        "inventory, and manage permitted LSL scripts in the single selected linkset. "
                        "Read current state before writing and obey viewer and simulator permission failures."
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
