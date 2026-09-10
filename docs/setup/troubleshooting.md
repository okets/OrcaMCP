# OrcaMCP Troubleshooting

Common issues and solutions when using OrcaMCP.

## Connection Issues

### "Nothing is listening at http://localhost:13618/mcp"

**Symptoms:**
```
Error: Nothing is listening at http://localhost:13618/mcp. OrcaMCP is not running -- use the 'start_orca' tool.
```

**Causes & Solutions:**

1. **OrcaSlicer not running**
   - Launch OrcaSlicer
   - Wait a few seconds for HTTP server to start

2. **Port not listening**
   ```bash
   # Check if anything is listening on 13618
   lsof -i :13618
   ```
   - If nothing shows, the HTTP server may have failed to start
   - Check OrcaSlicer console for errors

3. **Wrong port**
   ```bash
   # Verify your ORCAMCP_PORT setting
   echo $ORCAMCP_PORT
   ```

4. **Firewall blocking**
   - Shouldn't affect localhost, but check firewall if other solutions fail

If instead you saw `OrcaSlicer is reachable but the request did not complete`, that is a
different verdict (busy, not down) — see the `"OrcaMCP is not running" while it clearly
is` section below; the fix is to retry or raise `ORCAMCP_TIMEOUT`, not to relaunch
anything.

### "Connection refused"

**Same solutions as above**, plus:
- Ensure OrcaSlicer is the OrcaMCP version (not vanilla OrcaSlicer)
- Rebuild if you recently updated code

### "Request timed out"

OrcaSlicer's embedded HTTP server runs **one** worker thread, and every MCP handler blocks it while
the operation runs on the GUI thread. Calls are therefore served strictly one at a time: a batch of
eight tool calls queues, and the last one waits for the seven ahead of it.

Raise the ceiling if you are batching or slicing:

```bash
export ORCAMCP_TIMEOUT=300
```

### "OrcaMCP is not running" while it clearly is

This should no longer happen. The bridge used to treat any failed liveness probe — including a
0.3-second probe that queued behind a batch — as "not running", and cached that answer for three
seconds, so a burst of calls could get the verdict fabricated for the calls at the end of it.

The probe now returns one of three verdicts, and only the last one produces that message:

| Verdict | Meaning | What the bridge does |
|---|---|---|
| live | Something answered, even an HTTP error | Forward the request |
| busy | Reachable, no answer inside the probe window | Forward the request anyway, with the full `ORCAMCP_TIMEOUT` |
| down | Connection refused, or nothing listening | Answer locally: "OrcaMCP is not running" |

So if you see it, nothing is listening on the port. Check `ORCAMCP_PORT`, and use `start_orca`.

## Tool Errors

### "Invalid object_id"

**Symptoms:**
```json
{"error": "Invalid object_id: 5 (only 3 objects in scene)"}
```

**Causes & Solutions:**

1. **Object was deleted**
   - Re-query `get_scene_info` to get current object IDs
   - Object IDs shift when objects are deleted

2. **Wrong plate**
   - Objects are per-plate; ensure you're on the right plate
   - Use `select_plate` if needed

### "No objects in scene"

**Causes & Solutions:**

1. **Empty project**
   - Load a model first with `load_model`

2. **Wrong plate selected**
   - Use `get_scene_info` to see all plates and their objects

### "file_path is required"

**Causes & Solutions:**

1. **Missing parameter**
   ```json
   // Wrong
   {"name": "load_model", "arguments": {}}

   // Correct
   {"name": "load_model", "arguments": {"file_path": "/path/to/model.stl"}}
   ```

2. **Typo in parameter name**
   - Check exact parameter names in docs/tools/reference.md

### "File not found"

**Causes & Solutions:**

1. **Wrong path**
   - Use absolute paths
   - Check file exists: `ls -la /path/to/file.stl`

2. **Permission denied**
   - Ensure OrcaSlicer can read the file
   - Check file permissions

## Slicing Issues

### Slicing never completes

**Symptoms:**
- `get_slicing_status` always returns `{"is_slicing": true}`

**Causes & Solutions:**

1. **Complex model**
   - Wait longer; complex models take time
   - Check OrcaSlicer's progress indicator

2. **Slicing error**
   - Check OrcaSlicer UI for error messages
   - Model may have issues (non-manifold, etc.)

3. **Invalid settings**
   - Check for conflicting settings
   - Try with default settings

### "Slicing failed" or empty G-code

**Causes & Solutions:**

1. **Object outside build volume**
   - Use `arrange_objects` to position correctly
   - Check object position vs bed size

2. **Invalid model**
   - Try reloading the model
   - Check model in another slicer

3. **Incompatible settings**
   - Reset to default print preset
   - Check layer height vs nozzle size

## Preview/Rendering Issues

### Empty or black preview images

**Causes & Solutions:**

1. **No objects on plate**
   - Load a model first

2. **Camera position wrong**
   - Adjust `camera_position` and `target` values
   - Use suggested values from workflows.md

3. **OpenGL issues**
   - Try different resolution
   - Check graphics driver

### Preview files not created

**Causes & Solutions:**

1. **Permission denied**
   - Check /tmp is writable
   - Check disk space

2. **Path issue on Windows**
   - Temp directory may differ
   - Check returned paths

## Claude Code Integration Issues

### Tools not appearing

**Symptoms:**
- Claude doesn't see OrcaMCP tools
- "What tools are available?" doesn't list OrcaMCP

**Causes & Solutions:**

1. **Config file not found**
   - Check `.mcp.json` exists in project root
   - Or add to `~/.claude.json`

2. **Bridge script path wrong**
   ```json
   // Use absolute path if relative doesn't work
   {"args": ["/full/path/to/scripts/orcamcp-bridge.py"]}
   ```

3. **Python not found**
   ```json
   // Try explicit python path
   {"command": "/usr/bin/python3"}
   ```

4. **Bridge script error**
   - Test manually:
   ```bash
   echo '{"jsonrpc":"2.0","id":1,"method":"ping"}' | python3 scripts/orcamcp-bridge.py
   ```

### "Method not found"

**Symptoms:**
```json
{"error": {"code": -32601, "message": "Method not found: some_method"}}
```

**Causes & Solutions:**

1. **Typo in tool name**
   - Check exact tool names in `tools/list` response

2. **Tool not registered**
   - Ensure you're running OrcaMCP build (not vanilla OrcaSlicer)

## Build Issues

### CMake generator conflict

**Symptoms:**
```
CMake Error: Error: generator : Ninja
Does not match the generator used previously: Unix Makefiles
```

**Solution:**
```bash
rm -rf build/CMakeCache.txt build/CMakeFiles
./build_release_macos.sh -s -x
```

### Missing dependencies

**macOS:**
```bash
brew install cmake ninja ccache
```

**Linux:**
```bash
sudo apt install build-essential cmake ninja-build
```

### Compilation errors in MCP code

**Solutions:**

1. **Clean rebuild**
   ```bash
   rm -rf build
   ./build_release_macos.sh
   ```

2. **Check syntax**
   - nlohmann::json requires proper bracing
   - Check for missing semicolons

## Performance Issues

### Slow response times

**Causes & Solutions:**

1. **Large responses**
   - Use `save_to_file: true` for render_plate_view
   - Use `with_model_object_features: false` for get_scene_info

2. **Many objects**
   - Complex scenes take longer to process
   - Consider splitting across plates

3. **Debug logging enabled**
   - Disable `ORCAMCP_DEBUG` in production

### High memory usage

**Causes & Solutions:**

1. **Many preview images**
   - Preview files accumulate in /tmp
   - Restart OrcaSlicer to clean up (done on init)

2. **Large models**
   - Normal for detailed meshes
   - Close unused projects

## Getting Help

If you can't resolve an issue:

1. **Check logs**
   - Run OrcaSlicer from terminal to see output
   - Enable `ORCAMCP_DEBUG=1` for bridge logs

2. **Minimal reproduction**
   - Find simplest steps to reproduce
   - Note exact error messages

3. **Report issue**
   - GitHub issues with:
     - OrcaMCP version/commit
     - OS and version
     - Steps to reproduce
     - Error messages/logs
