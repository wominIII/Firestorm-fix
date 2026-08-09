# Firestorm Local MCP

This stdio MCP server exposes the read-only snapshot published by the custom
Firestorm viewer. It uses no network listener and sends no data by itself.

Enable **Local MCP Bridge** in **World > AI Diagnostics and Repair Assistant**,
then register `server.py` as a stdio MCP server. The viewer refreshes
`snapshot.xml` once per second under its user settings directory.

All current tools are read-only. Script source is intentionally excluded;
only metadata already available to the viewer is returned.
