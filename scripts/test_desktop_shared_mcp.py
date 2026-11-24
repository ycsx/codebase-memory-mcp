"""Exercise the Desktop console and client config migration with a UI binary (Python 3.11+)."""
import argparse
import concurrent.futures
import contextlib
import http.client
import json
import os
from pathlib import Path
import queue
import secrets
import socket
import subprocess
import tempfile
import threading
import time
import tomllib


def request(port, method, route, body=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    try:
        conn.request(method, route, None if body is None else json.dumps(body),
                     {"Content-Type": "application/json", **(headers or {})})
        reply = conn.getresponse()
        return reply.status, reply.getheader("Mcp-Session-Id"), reply.read()
    finally:
        conn.close()


@contextlib.contextmanager
def console(binary, env, log):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with log.open("wb") as output:
        child = subprocess.Popen([binary, "console", "--no-open", f"--port={port}"],
                                 env=env, cwd=env["HOME"], stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if child.poll() is not None:
                    raise RuntimeError(f"console exited with {child.returncode}, check the UI build")
                try:
                    status, _, data = request(port, "GET", "/api/processes")
                    if status == 200 and json.loads(data)["self_pid"] == child.pid:
                        break
                except OSError:
                    pass
                time.sleep(0.1)
            else:
                raise TimeoutError("Desktop console did not become ready")
            yield port, child.pid
        finally:
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()


def rpc(port, headers, method, params, session=None, notification=False):
    body = {"jsonrpc": "2.0", "method": method, "params": params}
    if not notification:
        body["id"] = 1
    if session:
        headers = {**headers, "Mcp-Session-Id": session}
    return request(port, "POST", "/mcp", body, headers)


def open_session(port, headers, name):
    status, session, _ = rpc(port, headers, "initialize", {
        "protocolVersion": "2025-11-25", "capabilities": {},
        "clientInfo": {"name": name, "version": "1"},
    })
    assert status == 200 and session
    assert rpc(port, headers, "notifications/initialized", {}, session, True)[0] == 202
    status, _, data = rpc(port, headers, "tools/list", {}, session)
    tools = {tool["name"] for tool in json.loads(data)["result"]["tools"]}
    assert status == 200 and {"index_repository", "search_graph", "list_projects"} <= tools
    return session, tools


def call(port, headers, session, tool, args):
    status, _, data = rpc(port, headers, "tools/call", {"name": tool, "arguments": args}, session)
    assert status == 200
    result = json.loads(data)["result"]
    assert not result.get("isError")
    return result["structuredContent"]


@contextlib.contextmanager
def codex_client(executable, env, repo):
    child_env = {key: value for key, value in env.items() if not key.startswith("CBM_DESKTOP_")}
    child_env["CBM_CACHE_DIR"] = str(repo.parent / "standalone-cache")
    with subprocess.Popen([executable, "app-server", "--stdio"], cwd=repo, env=child_env,
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          text=True, encoding="utf-8") as child:
        replies = queue.Queue()

        def read_replies():
            for line in child.stdout:
                replies.put(json.loads(line))
            replies.put(None)

        reader = threading.Thread(target=read_replies, daemon=True)
        reader.start()
        request_id = 0

        def send(message):
            child.stdin.write(json.dumps(message) + "\n")
            child.stdin.flush()

        def ask(method, **params):
            nonlocal request_id
            request_id += 1
            send({"id": request_id, "method": method, "params": params})
            deadline = time.monotonic() + 45
            while True:
                reply = replies.get(timeout=max(0, deadline - time.monotonic()))
                if reply is None:
                    raise RuntimeError(f"Codex exited while handling {method}")
                if reply.get("id") == request_id:
                    assert "result" in reply, (method, reply.get("error"))
                    return reply["result"]

        try:
            ask("initialize", clientInfo={"name": "cbm-smoke", "version": "1"},
                capabilities={"experimentalApi": True})
            send({"method": "initialized"})
            thread = ask("thread/start", cwd=str(repo), ephemeral=True)["thread"]["id"]
            server = "codebase-memory-mcp"
            inventory = ask("mcpServerStatus/list", threadId=thread, serverName=server,
                            detail="toolsAndAuthOnly")
            tools = next(item["tools"] for item in inventory["data"] if item["name"] == server)
            assert "list_projects" in tools and "index_repository" in tools, sorted(tools)

            def projects():
                result = ask("mcpServer/tool/call", threadId=thread, server=server,
                             tool="list_projects", arguments={})
                assert not result.get("isError")
                if os.name == "nt":
                    query = (f"Get-CimInstance Win32_Process -Filter \"ParentProcessId={child.pid} "
                             "AND Name='codebase-memory-mcp.exe'\" | Select-Object -Expand ProcessId")
                    children = subprocess.check_output(["powershell.exe", "-NoProfile",
                                                        "-NonInteractive", "-Command", query])
                    assert not children.strip(), "Codex also started a stdio backend"
                return {p["name"] for p in result["structuredContent"]["projects"]}

            yield projects
        finally:
            child.stdin.close()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
            reader.join()


def main(binary, sandbox, codex_executable=None):
    root = Path(sandbox)
    codex = root / ".codex" / "config.toml"
    codex.parent.mkdir()
    claude = root / ".claude.json"
    (root / ".claude").mkdir()
    repo = root / "example"
    repo.mkdir()
    (repo / "hello.py").write_text("def hello(): return 42\n")
    table = "[mcp_servers.codebase-memory-mcp]\n"
    preferences = ('startup_timeout_sec = 120\n[mcp_servers.codebase-memory-mcp.tools.search_graph]\n'
                   'approval_mode = "approve"\n')
    codex.write_text(table + 'command = "codebase-memory-mcp"\nargs = []\n' + preferences)
    name = "codebase-memory-mcp"
    installed = (root / ".local" / "bin" / binary.name).as_posix()
    claude.write_text(json.dumps({"mcpServers": {name: {"command": installed, "args": []}}}))
    token = "cbm-desktop-" + secrets.token_hex(32)
    owner = secrets.token_hex(32)
    env = {k: os.environ[k] for k in ("PATH", "SystemRoot", "WINDIR", "COMSPEC", "PATHEXT") if k in os.environ}
    env.update(HOME=sandbox, USERPROFILE=sandbox, TMP=sandbox, TEMP=sandbox, TMPDIR=sandbox,
               CODEX_HOME=str(codex.parent), APPDATA=str(root / "roaming"),
               LOCALAPPDATA=str(root / "local"), XDG_CONFIG_HOME=str(root / "xdg"),
               CBM_CACHE_DIR=str(root / "cache"), CBM_WORKERS="2",
               CBM_DESKTOP_MCP_TOKEN=token, CBM_DESKTOP_SERVICE_TOKEN=owner)
    log = root / "console.log"
    for attempt in range(2):
        with console(binary, env, log) as (port, pid):
            endpoint = f"http://127.0.0.1:{port}/mcp"
            config = tomllib.loads(codex.read_text())["mcp_servers"][name]
            other = json.loads(claude.read_text())["mcpServers"][name]
            assert config["url"] == other["url"] == endpoint
            assert "command" not in config and "args" not in config and other["type"] == "http"
            assert preferences in codex.read_text()
            headers = config["http_headers"]
            assert headers == other["headers"] == {"Authorization": "Bearer " + token}
            for route in ("/api/processes", "/api/integrations"):
                status, _, data = request(port, "GET", route)
                assert status == 200 and token.encode() not in data
            assert rpc(port, {"Authorization": "Bearer " + owner}, "initialize", {})[0] == 401
            assert rpc(port, {**headers, "Origin": "http://localhost"}, "initialize", {})[0] == 403
            with concurrent.futures.ThreadPoolExecutor(2) as pool:
                (one, tools), (two, other_tools) = list(pool.map(
                    lambda client: open_session(port, headers, client), ("codex-test", "claude-test")))
            assert one != two and tools == other_tools
            index = call(port, headers, one, "index_repository", {"repo_path": str(repo)})
            assert index["status"] == "indexed"
            for session in (one, two):
                call(port, headers, session, "search_graph",
                     {"project": index["project"], "name_pattern": "^hello$", "format": "json"})
            if codex_executable and attempt == 0:
                with codex_client(codex_executable, env, repo) as first:
                    with codex_client(codex_executable, env, repo) as second:
                        assert index["project"] in first() and index["project"] in second()
                    assert index["project"] in first()
                print(f"two Codex app-servers queried the index served by PID {pid}")
            assert request(port, "DELETE", "/mcp", headers={**headers, "Mcp-Session-Id": one})[0] == 204
            assert rpc(port, headers, "tools/list", {}, one)[0] == 404
            projects = call(port, headers, two, "list_projects", {})
            assert index["project"] in {p["name"] for p in projects["projects"]}
            assert json.loads(request(port, "GET", "/api/processes")[2])["self_pid"] == pid
            print(f"startup {attempt + 1}: PID {pid}, port {port}, 2 sessions, {len(tools)} tools")
        logs = log.read_bytes()
        assert token.encode() not in logs
        assert logs.count(b"msg=watcher.watch ") == 1

    custom = table + 'command = "codebase-memory-mcp"\nargs = ["--tool-profile=scout"]\n'
    codex.write_text(custom)
    remote = '{"mcpServers":{"codebase-memory-mcp":{"type":"http","url":"https://example.org/mcp"}}}'
    claude.write_text(remote)
    with console(binary, env, log):
        assert codex.read_text() == custom and claude.read_text() == remote
    codex.write_text('model = "test"\n')
    claude.write_text("{}")
    with console(binary, env, log) as (port, _):
        assert codex.read_text() == 'model = "test"\n' and claude.read_text() == "{}"
        status, _, data = request(port, "POST", "/api/integrations/apply", {"confirm": True})
        assert status == 200 and token.encode() not in data
        assert tomllib.loads(codex.read_text())["mcp_servers"][name]["url"].endswith("/mcp")
        assert json.loads(claude.read_text())["mcpServers"][name]["type"] == "http"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--codex", help="also run two real Codex app-server clients")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cbm-shared-") as sandbox:
        main(args.binary.resolve(), sandbox, args.codex)
