"""Check the graph after full and incremental indexing, below and above the parallel threshold."""
import contextlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile


CPP = r'''#include "ops.hpp"
void Head() {}
void Middle() {
    const wchar_t* s = L"line 0\r\n\
line 1\r\n";
}
int after_one() {
    Ops o;
    return o.run(3);
}
void Trailer() {}
'''

FILES = {
    "package.json": '{"name":"fixture","dependencies":{"ws":"1","connect":"1"}}',
    "tsconfig.json": '{"compilerOptions":{"baseUrl":".","paths":{"@app/*":["src/*"]}}}',
    "src/server/index.ts": """import { WebSocketServer } from 'ws';
import connect from 'connect';
import { readFileSync } from 'fs';
import App from './App.vue';
import Widget from './Widget.vue';
import { CatService } from '@app/cats/cat.service';
import { local } from './util';
export function run() {
    connect(); readFileSync(); local(); new WebSocketServer(); new CatService();
    return [App, Widget];
}
""",
    "src/server/ws.ts": "export class WebSocketServer {}",
    "src/client/connect.ts": "export function connect() {}",
    "src/server/fs.ts": "export function readFileSync() {}",
    "src/server/util.ts": "export function local() {}",
    "src/cats/cat.service.ts": "export class CatService {}",
    "src/other/service.ts": "export function service() {}",
    "src/tests/app.spec.ts": "export function App() {}",
    "src/server/Widget.vue": "<template><div /></template>",
    "ops.hpp": "struct Ops { int run(int x) { return x; } };\n",
    "api/OrderApi.java": 'package api;\npublic interface OrderApi {\n'
                         '    @GET("v1/orders/{id}") Order getOrder(String id);\n}\n',
    "api/Books.kt": 'package api\ninterface Books {\n'
                    '    @GET("v1/books/{id}")\n    fun book(id: String): String\n}\n',
    "api/Resource.java": 'package api;\ninterface Resource {\n'
                         '    @GET @Path("/resource") String getResource();\n}\n',
    "api/Controller.java": 'package api;\nclass Controller {\n'
                           '    @GET("/server") String controller() { return ""; }\n}\n',
    "deploy/helm/values.yaml": "replicas: 1\n",
    "deployed/start.sh": "#!/bin/sh\nexit 0\n",
    "deploy/copied.py": "def copied(): pass\n",
    "deploy/package.json": '{"name":"copy"}',
    "app/env/settings.py": "def setting(): pass\n",
    "virtual/env/pyvenv.cfg": "home = venv\n",
    "virtual/env/ignored.py": "def ignored(): pass\n",
    "kept/deploy/retained.py": "def retained(): pass\n",
    "ignored/deploy/values.yaml": "ignore: true\n",
    ".cbmignore": "!kept/deploy/\nignored/\n",
}
EDGES = """SELECT s.name, e.type, t.label, t.name, t.file_path, e.properties
           FROM edges e JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id
           WHERE s.file_path = ?"""


def check(db, incremental):
    modules = db.execute(EDGES, ("src/server/index.ts",)).fetchall()
    imports = {(name, label, path) for _, kind, label, name, path, _ in modules if kind == "IMPORTS"}
    assert {("ws", "ExternalPackage", ""), ("fs", "ExternalPackage", ""),
            ("connect", "ExternalPackage", "")} <= imports, imports
    assert {"src/cats/cat.service.ts", "src/server/Widget.vue", "src/server/util.ts"} <= {
        path for _, _, path in imports
    }, imports
    assert {path for _, kind, _, _, path, _ in modules if kind in ("IMPORTS", "CALLS")}.isdisjoint(
        {"src/server/ws.ts", "src/server/fs.ts", "src/client/connect.ts", "src/tests/app.spec.ts"}
    ), modules
    assert {"CatService", "local"} <= {name for _, kind, _, name, _, _ in modules if kind == "CALLS"}
    for file, name, endpoint in [("api/OrderApi.java", "getOrder", "/v1/orders/{id}"),
                                 ("api/Books.kt", "book", "/v1/books/{id}")]:
        routes = [(kind, target) for caller, kind, _, target, _, _ in db.execute(EDGES, (file,))
                  if caller == name and kind in ("HANDLES", "HTTP_CALLS")]
        assert routes == [("HTTP_CALLS", endpoint)], (file, routes)
    for file in ("api/Resource.java", "api/Controller.java"):
        kinds = {kind for _, kind, _, _, _, _ in db.execute(EDGES, (file,))}
        assert "HANDLES" in kinds and "HTTP_CALLS" not in kinds, (file, kinds)
    paths = {p for (p,) in db.execute("SELECT file_path FROM nodes WHERE label='File'")}
    assert {"deploy/helm/values.yaml", "deployed/start.sh", "app/env/settings.py",
            "kept/deploy/retained.py"} <= paths, paths
    assert paths.isdisjoint({"deploy/copied.py", "deploy/package.json", "virtual/env/ignored.py",
                             "ignored/deploy/values.yaml"}), paths
    assert ("deploy/helm/extra.yaml" in paths) == incremental
    for file in ("lf.cpp", "crlf.cpp"):
        positions = db.execute("SELECT name,start_line,end_line FROM nodes WHERE file_path=? "
                               "AND label='Function' ORDER BY start_line", (file,)).fetchall()
        expected = [("Head", 2, 2), ("Middle", 3, 6), ("after_one", 7, 10), ("Trailer", 11, 11)]
        if incremental and file == "crlf.cpp":
            expected.append(("Added", 12, 12))
        assert positions == expected, (file, positions)
        # The full index resolves cross-file LSP. An incremental pass retains
        # those edges for unchanged callers and reparses the edited CRLF file.
        if not incremental or file == "lf.cpp":
            edges = db.execute(EDGES, (file,)).fetchall()
            assert any(caller == "after_one" and kind == "CALLS" and path == "ops.hpp" and
                       name == "run" and json.loads(props).get("strategy", "").startswith("lsp")
                       for caller, kind, _, name, path, props in edges), (file, edges)


def run(binary, root, padding):
    repo = root / "工程"
    cache = root / "缓存"
    cache.mkdir()
    files = {**FILES, "lf.cpp": CPP, "crlf.cpp": CPP.replace("\n", "\r\n")}
    for i in range(padding):
        files[f"pad/f{i}.ts"] = f"export const v{i} = {i};\n"
    for name, value in files.items():
        target = repo / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(value.encode())
    env = {k: os.environ[k] for k in ("PATH", "SystemRoot", "WINDIR", "COMSPEC", "PATHEXT",
                                       "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH") if k in os.environ}
    # Run the production CLI in-process so its pipeline route is visible on stderr.
    env.update(CBM_WORKERS="4", CBM_CACHE_DIR=str(cache), CBM_INDEX_SUPERVISOR="0",
               CBM_LOG_LEVEL="info", CBM_LOG_FORMAT="text", HOME=str(root), USERPROFILE=str(root),
               TMP=str(root), TEMP=str(root), TMPDIR=str(root))
    for incremental in (False, True):
        if incremental:
            (repo / "deploy/helm/extra.yaml").write_text("replicas: 2\n")
            with (repo / "crlf.cpp").open("ab") as source:
                source.write(b"void Added() {}\r\n")
            changed = ["deploy/helm/values.yaml", "src/server/index.ts", "api/Books.kt",
                       "api/OrderApi.java", "api/Resource.java", "api/Controller.java"]
            for name in changed + [f"pad/f{i}.ts" for i in range(padding)]:
                with (repo / name).open("ab") as change:
                    change.write(b"\n")
        result = subprocess.run([binary, "cli", "index_repository"],
                                input=json.dumps({"repo_path": str(repo), "mode": "full"}).encode(),
                                capture_output=True, env=env, check=True, timeout=90)
        info = json.loads(result.stdout)
        assert info["status"] == "indexed", info
        routes = [line.split(b"path=", 1)[1].split()[0] for line in result.stderr.splitlines()
                  if b"msg=pipeline.route path=" in line]
        assert routes == ([b"incremental"] if incremental else [b"full"]), routes
        if incremental:
            mode = b"parallel" if padding else b"sequential"
            assert b"msg=incremental.mode mode=" + mode in result.stderr, mode
        database = cache / (info["project"] + ".db")
        with contextlib.closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)) as db:
            check(db, incremental)
        print(f"padding={padding}, {'incremental' if incremental else 'full'} graph verified")


if __name__ == "__main__":
    for padding in (0, 60):
        with tempfile.TemporaryDirectory(prefix="cbm-index-regression-") as tmp:
            run(Path(sys.argv[1]).resolve(), Path(tmp).resolve(), padding)
