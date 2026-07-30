"""Black-box guard for the foreground visual-console command.

Usage:
    python test_console_command.py <binary> [ui|standard]
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_for_console(port, process, timeout=25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if process.poll() is not None:
            return False
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/" % port, timeout=1) as response:
                return response.status == 200 and bool(response.read())
        except Exception:
            time.sleep(0.2)
    return False


def run_checked(binary, *args, env=None):
    return subprocess.run(
        [binary] + list(args),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        timeout=20,
    )


def main():
    if len(sys.argv) < 2:
        print("usage: python test_console_command.py <binary> [ui|standard]")
        return 2
    binary = os.path.abspath(sys.argv[1])
    variant = sys.argv[2] if len(sys.argv) > 2 else "ui"
    if variant not in ("ui", "standard"):
        print("FAIL: variant must be ui or standard")
        return 2

    help_result = run_checked(binary, "console", "--help")
    if help_result.returncode != 0 or "console [--port=N] [--no-open]" not in help_result.stdout:
        print("FAIL: console --help contract is missing")
        return 1

    invalid_result = run_checked(binary, "console", "--port=invalid", "--no-open")
    if invalid_result.returncode != 2 or "invalid console port" not in invalid_result.stderr:
        print("FAIL: invalid console port was not rejected")
        return 1

    work = tempfile.mkdtemp(prefix="cbm_console_")
    try:
        cache_dir = os.path.join(work, "cache")
        os.makedirs(cache_dir)
        config_path = os.path.join(cache_dir, "config.json")
        port = free_port()
        original_config = {"ui_enabled": False, "ui_port": port}
        with open(config_path, "w", encoding="utf-8") as config_file:
            json.dump(original_config, config_file, separators=(",", ":"))
        with open(config_path, "rb") as config_file:
            original_bytes = config_file.read()

        env = dict(os.environ)
        env["HOME"] = work
        env["USERPROFILE"] = work
        env["TEMP"] = work
        env["TMP"] = work
        env["TMPDIR"] = work
        env["CBM_CACHE_DIR"] = cache_dir
        env["CBM_INDEX_SUPERVISOR"] = "0"

        if variant == "standard":
            result = run_checked(binary, "console", "--no-open", env=env)
            if result.returncode == 0 or "without embedded UI resources" not in result.stderr:
                print("FAIL: standard binary did not fail clearly for console")
                return 1
            print("GREEN: standard binary rejects console without silent fallback")
            return 0

        process = subprocess.Popen(
            [binary, "console", "--no-open"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        try:
            if not wait_for_console(port, process):
                stderr = process.stderr.read().decode("utf-8", "replace") if process.poll() else ""
                print("FAIL: console did not serve the embedded UI: %s" % stderr)
                return 1
            with open(config_path, "rb") as config_file:
                if config_file.read() != original_bytes:
                    print("FAIL: console permanently changed UI configuration")
                    return 1
            print("GREEN: console served on configured loopback port without changing config")
            return 0
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
