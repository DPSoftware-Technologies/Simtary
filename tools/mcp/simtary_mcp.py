#!/usr/bin/env python3
"""Simtary MCP server - drive a running Simtary game (dev build) from an MCP client.

The game side is Framework/dev/EditorBridge: a localhost TCP socket that takes one JSON
request per line and runs it on the game's main thread between frames. This server speaks
MCP (JSON-RPC 2.0 over stdio, newline-delimited) to the client and relays tool calls to
that socket. Standard library only.

    python simtary_mcp.py --game-dir E:/HMMWV4/Milistry/build/win_x86-64-dev/Release

The game writes devbridge.json (port + token) into its working directory - the folder
the exe runs in - when it starts. This server reads it on every call, so a restarted game
is picked up without restarting the server.

Tools are the bridge's commands (scene_*, entity_*, native_*, terrain_*, vehicle_*, ...)
plus a few of its own: game_launch / game_stop / game_wait, game_build, and screenshot,
which returns the image itself rather than a path.
"""

import argparse
import base64
import json
import os
import socket
import subprocess
import sys
import time

PROTOCOL_VERSION = "2024-11-05"

ARGS = None
LAUNCHED = None  # the Popen of a game this server started


# --------------------------------------------------------------------- bridge ---

class BridgeError(Exception):
    pass


def bridge_info():
    path = os.path.join(ARGS.game_dir, "devbridge.json")
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        raise BridgeError("game not running (no %s) - start it with game_launch, from a dev build "
                          "(cmake --preset win_x86-64-dev)" % path)


def bridge_call(cmd, args=None, timeout=60.0):
    info = bridge_info()
    request = {"token": info["token"], "cmd": cmd, "args": args or {}}
    try:
        with socket.create_connection(("127.0.0.1", int(info["port"])), timeout=timeout) as s:
            s.sendall((json.dumps(request) + "\n").encode("utf-8"))
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
    except OSError as e:
        raise BridgeError("cannot reach the game on port %s (%s) - is it still running?" % (info["port"], e))
    try:
        reply = json.loads(data.decode("utf-8"))
    except ValueError:
        raise BridgeError("bad reply from the game: %r" % data[:200])
    if not reply.get("ok"):
        raise BridgeError(reply.get("error", "unknown error"))
    return reply.get("result")


# ---------------------------------------------------------------------- tools ---

ANY = {"type": "object", "additionalProperties": True}


def obj(props, required=None):
    schema = {"type": "object", "properties": props, "additionalProperties": True}
    if required:
        schema["required"] = required
    return schema


NUM = {"type": "number"}
STR = {"type": "string"}
BOOL = {"type": "boolean"}
VEC3 = {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}
ENTITY = {"description": "entity id (number) or name (string)"}

# tool name -> (bridge command, description, input schema)
BRIDGE_TOOLS = {
    "ping": ("ping", "Game status: app, current scene, loading, play state, editor mode, entity count.", ANY),
    "log": ("log", "Last lines of the game's backlog (log).", obj({"lines": NUM, "filter": STR})),
    "scene_list": ("scene.list", "Registered scenes and their origin (c++, file, runtime).", ANY),
    "scene_load": ("scene.load", "Load a scene by name. Returns at once; poll ping until loading is false.", obj({"name": STR}, ["name"])),
    "scene_new": ("scene.new", "Create and load an empty scene with sun and sky.", obj({"name": STR})),
    "scene_save": ("scene.save", "Save the current scene. .stsd = native map. In a dev build it is also copied into the project's assets/scenes/. Use e.g. assets/scenes/mymap.stsd.", obj({"path": STR}, ["path"])),
    "scene_rescan": ("scene.rescan", "Register maps newly saved into the scene folder as loadable scenes.", ANY),
    "entity_list": ("entity.list", "List entities: id, name, parent, components. filter matches names.", obj({"filter": STR, "roots_only": BOOL, "limit": NUM})),
    "entity_get": ("entity.get", "One entity in full: transform (local and world), components, metadata, children, native components, rigid body.", obj({"id": ENTITY})),
    "entity_create": ("entity.create", "Create an entity. primitive: empty|cube|sphere|plane. position/rotation_deg/scale are local when a parent is given.", obj({"name": STR, "primitive": STR, "parent": ENTITY, "position": VEC3, "rotation_deg": VEC3, "scale": VEC3})),
    "entity_delete": ("entity.delete", "Delete an entity and its children.", obj({"id": ENTITY})),
    "entity_rename": ("entity.rename", "Rename an entity.", obj({"id": ENTITY, "new_name": STR}, ["new_name"])),
    "entity_transform": ("entity.transform", "Set position / rotation_deg (pitch,yaw,roll) / scale. Local for children; a root's position is its world position. Teleports a simulated body.", obj({"id": ENTITY, "position": VEC3, "rotation_deg": VEC3, "scale": VEC3})),
    "entity_parent": ("entity.parent", "Re-parent (parent 0 detaches). keep_world=true keeps it where it is.", obj({"id": ENTITY, "parent": ENTITY, "keep_world": BOOL})),
    "metadata_set": ("metadata.set", "Set metadata keys. Numbers are written as float (and int when whole); force with {\"int\":n} / {\"float\":n}.", obj({"id": ENTITY, "values": ANY}, ["values"])),
    "metadata_remove": ("metadata.remove", "Remove metadata keys.", obj({"id": ENTITY, "keys": {"type": "array", "items": STR}}, ["keys"])),
    "native_list": ("native.list", "Every registered native component name (stVehicle, stInput, stChaseCamera, ...).", ANY),
    "native_attach": ("native.attach", "Attach a native component (writes NCI_n / NCA_n_* like the editor). Returns its localID.", obj({"id": ENTITY, "component": STR, "params": ANY}, ["component"])),
    "native_detach": ("native.detach", "Detach a native component by localID.", obj({"id": ENTITY, "localID": NUM}, ["localID"])),
    "rigidbody_set": ("rigidbody.set", "Add/modify a rigid body: shape box|sphere|capsule|cylinder|convex|mesh, mass (0 = static), box half extents, radius, height, friction, restitution, kinematic.", obj({"id": ENTITY, "shape": STR, "mass": NUM, "box": VEC3, "radius": NUM, "height": NUM, "friction": NUM, "restitution": NUM, "kinematic": BOOL})),
    "model_import": ("model.import", "Import a model (.gltf .glb .fbx .obj) into the scene; returns the root id.", obj({"path": STR, "name": STR, "parent": ENTITY, "position": VEC3, "rotation_deg": VEC3, "scale": VEC3}, ["path"])),
    "terrain_create": ("terrain.create", "Create an authored terrain (flat, four layers: Grass Dirt Rock Sand).", obj({"name": STR, "spacing": NUM, "base": NUM, "radius": NUM})),
    "terrain_generate": ("terrain.generate", "Generate eroded biome terrain into a region. biomes: plains hills mountains dunes canyons marsh lakes.", obj({"terrain": ENTITY, "x": NUM, "z": NUM, "size_x": NUM, "size_z": NUM, "biomes": {"type": "array", "items": STR}, "height": NUM, "base": NUM, "seed": NUM, "erosion": NUM, "warp": NUM, "blending": NUM, "biome_size": NUM, "auto_paint": BOOL, "water_level": NUM})),
    "terrain_brush": ("terrain.brush", "One brush dab: tool raise|lower|smooth|flatten|paint|noise at x,z. paint uses layer (0 Grass 1 Dirt 2 Rock 3 Sand).", obj({"terrain": ENTITY, "tool": STR, "x": NUM, "z": NUM, "radius": NUM, "amount": NUM, "hardness": NUM, "layer": NUM, "target": NUM}, ["x", "z"])),
    "terrain_sample": ("terrain.sample", "Ground height, normal and dominant layer at x,z.", obj({"x": NUM, "z": NUM}, ["x", "z"])),
    "play": ("play", "Play control: play | nophysics | pause | step (frames) | reset.", obj({"state": STR, "frames": NUM})),
    "input_inject": ("input.inject", "Drive an input action (e.g. Throttle 1, Steer -0.5, Brake 1) for some seconds; works while the window is unfocused.", obj({"action": STR, "value": {}, "seconds": NUM}, ["action"])),
    "input_clear": ("input.clear", "Release every injected input.", ANY),
    "camera_get": ("camera.get", "The game camera position, forward, fov.", ANY),
    "camera_set": ("camera.set", "Point the game camera (a scene with its own camera logic overrides it next frame).", obj({"position": VEC3, "target": VEC3}, ["position", "target"])),
    "editor_mode": ("editor.mode", "Turn editor mode on/off.", obj({"on": BOOL})),
    "script_run": ("script.run", "Run Taipan code in the game's script VM; returns what it logged.", obj({"code": STR}, ["code"])),
    "vehicle_fit_visual": ("vehicle.fit_visual", "Fit a child model to an stVehicle: scale to the preset wheelbase, front to +Z, wheel centres on the simulated wheels. Needs the model's wheel nodes. Save the map afterwards.", obj({"id": ENTITY, "model": ENTITY})),
    "vehicle_reset": ("vehicle.reset", "Put an stVehicle back on its wheels where it stands.", obj({"id": ENTITY})),
    "vehicle_telemetry": ("vehicle.telemetry", "A vehicle's speed, rpm, gear, inputs, position and per-wheel load/slip/contact.", obj({"id": ENTITY})),
    "call": (None, "Any bridge command by name (see 'help'), for anything without its own tool.", obj({"cmd": STR, "args": ANY}, ["cmd"])),
    "help": ("help", "Every bridge command with its arguments, straight from the running game.", ANY),
}

LOCAL_TOOLS = {
    "screenshot": ("Capture the game view and return the image.", obj({"path": STR})),
    "game_launch": ("Start the game (dev build) and wait until the bridge answers. scene picks the startup scene.", obj({"scene": STR, "args": {"type": "array", "items": STR}, "timeout": NUM})),
    "game_stop": ("Stop the game this server launched (or the one in devbridge.json).", ANY),
    "game_wait": ("Wait some seconds (max 120) - let a scene load, a car drive, physics settle.", obj({"seconds": NUM}, ["seconds"])),
    "game_build": ("Build the game with CMake (preset, default win_x86-64-dev). Long: minutes when the engine changed. Stop the game first.", obj({"preset": STR, "timeout": NUM})),
}


def text(s):
    return {"content": [{"type": "text", "text": s if isinstance(s, str) else json.dumps(s, indent=1)}]}


def error(s):
    return {"content": [{"type": "text", "text": s}], "isError": True}


def fix_entity_args(args):
    """'id' may arrive as a name string; the bridge takes either, under 'id' or 'name'."""
    return args


def call_tool(name, args):
    global LAUNCHED
    args = args or {}
    if name in BRIDGE_TOOLS:
        cmd, _, _ = BRIDGE_TOOLS[name]
        if cmd is None:
            cmd, args = args.get("cmd", ""), args.get("args", {})
        timeout = 600.0 if cmd in ("terrain.generate", "scene.save") else 60.0
        return text(bridge_call(cmd, fix_entity_args(args), timeout))

    if name == "screenshot":
        path = bridge_call("screenshot", {"path": args["path"]} if args.get("path") else {})
        with open(path, "rb") as f:
            data = base64.b64encode(f.read()).decode("ascii")
        return {"content": [{"type": "image", "data": data, "mimeType": "image/png"},
                            {"type": "text", "text": path}]}

    if name == "game_wait":
        time.sleep(max(0.0, min(120.0, float(args.get("seconds", 1)))))
        return text("waited")

    if name == "game_launch":
        exe = os.path.join(ARGS.game_dir, ARGS.exe)
        if not os.path.isfile(exe):
            return error("no game at %s - build the dev preset first (game_build)" % exe)
        cmdline = [exe]
        if args.get("scene"):
            cmdline.append("--scene=" + args["scene"])
        cmdline += list(args.get("args", []))
        stale = os.path.join(ARGS.game_dir, "devbridge.json")
        try:
            os.remove(stale)  # a previous run's file would answer for a game that is not there
        except OSError:
            pass
        # Its own console output must NOT inherit this process's stdout: that is the MCP
        # channel, and one printf from the game corrupts the protocol.
        LAUNCHED = subprocess.Popen(cmdline, cwd=ARGS.game_dir, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + float(args.get("timeout", 90))
        last = ""
        while time.time() < deadline:
            if LAUNCHED.poll() is not None:
                return error("game exited during startup (code %s)" % LAUNCHED.returncode)
            try:
                status = bridge_call("ping", timeout=5)
                if not status.get("loading"):
                    return text(status)
            except BridgeError as e:
                last = str(e)
            time.sleep(1.0)
        return error("game did not answer in time: " + last)

    if name == "game_stop":
        if LAUNCHED is not None and LAUNCHED.poll() is None:
            LAUNCHED.terminate()
            try:
                LAUNCHED.wait(10)
            except subprocess.TimeoutExpired:
                LAUNCHED.kill()
            LAUNCHED = None
            return text("stopped")
        try:
            pid = int(bridge_info().get("pid", 0))
        except BridgeError:
            return text("not running")
        if pid:
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
            else:
                os.kill(pid, 15)
        return text("stopped pid %d" % pid)

    if name == "game_build":
        preset = args.get("preset", "win_x86-64-dev")
        result = subprocess.run(["cmake", "--build", "--preset", preset, "--", "/m", "/v:minimal"],
                                cwd=ARGS.project_dir, capture_output=True, text=True, stdin=subprocess.DEVNULL,
                                timeout=float(args.get("timeout", 1800)))
        lines = [l for l in (result.stdout + result.stderr).splitlines() if ": error" in l or "fatal error" in l]
        summary = "exit %d" % result.returncode
        return (text if result.returncode == 0 else error)(summary + ("\n" + "\n".join(lines[:40]) if lines else ""))

    return error("unknown tool " + name)


def tool_list():
    tools = []
    for name, (_, desc, schema) in BRIDGE_TOOLS.items():
        tools.append({"name": name, "description": desc, "inputSchema": schema})
    for name, (desc, schema) in LOCAL_TOOLS.items():
        tools.append({"name": name, "description": desc, "inputSchema": schema})
    return tools


# ----------------------------------------------------------------- json-rpc ---

def respond(msg_id, result=None, err=None):
    out = {"jsonrpc": "2.0", "id": msg_id}
    if err is not None:
        out["error"] = err
    else:
        out["result"] = result
    sys.stdout.write(json.dumps(out) + "\n")
    sys.stdout.flush()


def handle(msg):
    method = msg.get("method")
    msg_id = msg.get("id")
    if method == "initialize":
        respond(msg_id, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "simtary", "version": "1.0"},
            "instructions": "Drive a running Simtary game (dev build): build scenes from entities and "
                            "components, generate terrain, save maps, drive vehicles with input_inject, "
                            "and look with screenshot. Start with game_launch or ping.",
        })
    elif method == "tools/list":
        respond(msg_id, {"tools": tool_list()})
    elif method == "tools/call":
        params = msg.get("params", {})
        try:
            respond(msg_id, call_tool(params.get("name", ""), params.get("arguments")))
        except BridgeError as e:
            respond(msg_id, error(str(e)))
        except Exception as e:  # never take the server down over one tool call
            respond(msg_id, error("%s: %s" % (type(e).__name__, e)))
    elif method == "ping":
        respond(msg_id, {})
    elif msg_id is not None:
        respond(msg_id, err={"code": -32601, "message": "method not found: %s" % method})
    # notifications (no id) need no answer


def main():
    global ARGS
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-dir", required=True, help="folder the game exe runs in (devbridge.json appears there)")
    parser.add_argument("--exe", default="Milistry.exe")
    parser.add_argument("--project-dir", default=None, help="CMake project for game_build (default: game-dir/../../..)")
    ARGS = parser.parse_args()
    ARGS.game_dir = os.path.abspath(ARGS.game_dir)
    if ARGS.project_dir is None:
        ARGS.project_dir = os.path.abspath(os.path.join(ARGS.game_dir, "..", "..", ".."))
    del here

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        handle(msg)


if __name__ == "__main__":
    main()
