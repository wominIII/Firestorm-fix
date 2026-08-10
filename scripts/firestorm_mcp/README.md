# Firestorm Local MCP

This stdio MCP server exposes the snapshot published by the custom Firestorm
viewer and can directly update the transform of the single selected root object.
It uses no network listener and sends no data by itself.

Enable **Local MCP Bridge** in **World > AI Diagnostics and Repair Assistant**,
then register `server.py` as a stdio MCP server. The viewer refreshes
`snapshot.xml` once per second under its user settings directory.

`set_object_transform` can change position, quaternion rotation, and scale without
a confirmation dialog. Firestorm still requires the target UUID to match the only
selected root object and enforces simulator move/modify permissions.
`set_object_face_material` can change diffuse/PBR asset UUIDs, RGBA color, and texture
scale, offset, and rotation for one face. Script source is intentionally excluded;
only metadata already available to the viewer is returned.
