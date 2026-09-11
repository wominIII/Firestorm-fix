# Firestorm Local MCP

This stdio MCP server exposes detailed local state published by the custom
Firestorm viewer. It can inspect loaded objects and linksets, organize the
agent inventory, and perform permission-gated edits. It uses no network listener
and sends no data by itself.

Enable **Local MCP Bridge** in **World > AI Diagnostics and Repair Assistant**,
then register `server.py` as a stdio MCP server. The viewer refreshes
`snapshot.xml` once per second under its user settings directory.

The current server also exposes client-local inventory labels. Inventory reads and
search results include `local_label`; `get_inventory_local_label` reads it,
`set_inventory_local_label` sets or clears it without changing the server-side name,
and `ai_label_inventory_folder` labels the chosen folder itself plus every loaded
descendant using the AI translation configuration in Firestorm.

Server version 0.10 adds `set_linked_prim_transforms`, which updates one or up to
100 child prims in the single selected linkset without moving its root. Requests may
use root-relative local coordinates or Viewer edit-space world coordinates. Firestorm
validates the complete batch before applying it, returns exact previous local transforms,
and the MCP server records those values for undo. Object snapshots now expose both
edit-space and local position/rotation explicitly.

Server version 0.9 adds nearby owned-object discovery over the Viewer's currently
loaded object list. `list_nearby_owned_objects` returns nearest-first UUIDs, linkset
roots, positions, distances and permission/status fields with radius and result caps;
`select_nearby_owned_object` safely selects either the whole owned linkset or one exact
prim so it is highlighted in Firestorm. Follow it with `inspect_object` for faces,
task inventory and complete linkset details. The discovery result is explicitly local
Viewer coverage, not a promise that every object in the simulator has loaded.

Server version 0.8 adds a persistent, cursor-based event feed for Viewer connection,
login, selection, attachment, wearable, and animation changes. It also adds dry-run
change plans, redacted command auditing, and exact undo records for settings, object
transforms, face materials, and Mesh uploader configuration. Script source and credential
values are never retained in the audit log. Protocol version 9 adds permission-gated single/batch linked child prim transforms.
Protocol version 8 adds nearby owned-object discovery and selection/highlighting.
Protocol version 7 adds direct Mesh uploader diagnostics, typed configuration of common
upload options, local feasibility reporting, server-side fee calculation status, and
safe fee calculation without triggering the final paid upload. Protocol version 6 added searchable, paginated viewer/per-account setting discovery,
typed setting reads and writes, and reset-to-default operations. Credential values
are always redacted and cannot be changed through MCP. Protocol version 5 added exact selection context (primary child prim, linkset root,
and selected faces), root-versus-child script targeting, and selected-face material
targeting. Protocol version 4 added `inspect_object`, detailed object-inventory metadata,
`get_inventory_entry`, `list_inventory_folder`, `search_inventory`,
`create_inventory_folder`, `rename_inventory_entry`, `move_inventory_entries`,
and recoverable `trash_inventory_entries`. Inventory search covers the loaded
agent inventory tree; when a folder is incomplete the Viewer requests it from the
simulator and the caller should retry after a `fetch_requested` result.

`set_object_transform` changes position, quaternion rotation, and scale on the
selected linkset root. `set_object_face_material` can target an individual prim in
that linkset and update diffuse/PBR asset UUIDs, RGBA color, and texture transforms.
Firestorm checks target UUIDs and effective move/modify permissions.

Object-script tools can target any prim in the single selected linkset. Ownership
is not required: effective personal or active-group permissions are used. Source
reads require both copy and modify permission, matching the Viewer's normal script
editor. `update_object_script` and `patch_object_script` compile back into the same
item UUID; the latter first downloads the source and applies exact-match edits, so
the script is not deleted and recreated. A modify-only script may accept a complete
source replacement, but its existing source cannot be read first. Creating a script
keeps a backup in the agent's Scripts folder. The simulator remains authoritative
for compilation, permissions, running state, and inventory changes.
