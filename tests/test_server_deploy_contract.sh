#!/usr/bin/env bash
# Static contract checks for the privileged Linux server installer.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALLER="$ROOT/deploy/install-server.sh"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

require_text() {
    local pattern="$1"
    local description="$2"
    grep -Fq -- "$pattern" "$INSTALLER" || fail "$description"
}

[ -f "$INSTALLER" ] || fail "server installer is missing"
if LC_ALL=C grep -q $'\r$' "$INSTALLER"; then
    fail "server installer must use LF line endings"
fi
bash -n "$INSTALLER"

HELP="$(bash "$INSTALLER" --help)"
for option in --public --port --allow-cidr --allow-all --version --binary \
    --allowed-root --ui-port; do
    grep -Fq -- "$option" <<<"$HELP" || fail "help is missing $option"
done
for removed_option in --domain --tls-cert --tls-key --self-signed; do
    if grep -Fq -- "$removed_option" <<<"$HELP"; then
        fail "obsolete TLS option remains in help: $removed_option"
    fi
done

require_text 'MANAGER_PATH="/usr/local/sbin/cbm-server"' "management command must be registered"
require_text 'cbm-server keys' "top-level key listing shortcut must be documented"
require_text 'cbm-server new-key PRINCIPAL PROJECTS' "top-level key creation shortcut must be documented"
require_text 'cbm-server rotate-key KEY_ID' "top-level key rotation shortcut must be documented"
require_text 'cbm-server revoke-key KEY_ID' "top-level key revocation shortcut must be documented"
require_text 'cbm-server ui-status' "visual-console status shortcut must be documented"
require_text 'cbm-server ui-logs [LINES]' "visual-console log shortcut must be documented"
require_text 'cbm-server ui-password-reset' "visual-console password reset must be documented"
require_text 'cbm-server git-key' "Git SSH deploy-key command must be documented"
require_text 'cbm-server allowed-root' "allowed-root query command must be documented"
require_text 'cbm-server set-allowed-root DIR' "allowed-root update command must be documented"
require_text 'cbm-server key list' "key listing shortcut must be documented"
require_text 'cbm-server key create PRINCIPAL PROJECTS' "key creation shortcut must be documented"
require_text 'cbm-server key rotate KEY_ID' "key rotation shortcut must be documented"
require_text 'cbm-server key revoke KEY_ID' "key revocation shortcut must be documented"
require_text 'management_main "$@"' "installed command must dispatch management operations"

require_text 'ALLOW_ALL=false' "explicit allow-all compatibility flag must remain supported"
require_text 'PUBLIC=false' "loopback mode must be the default"
require_text 'LISTEN_ADDRESS="127.0.0.1"' "the default entry address must be loopback"
require_text '--public) PUBLIC=true' "public test mode must require an explicit flag"
require_text 'configure_listen_mode' "the requested listen mode must be applied"
require_text 'ALLOW_CIDRS=()' "the installer must not guess intranet address ranges"
if grep -Fq '192.168.0.0/16' "$INSTALLER"; then
    fail "the installer must not assume RFC1918 ranges describe the intranet"
fi
if grep -Eq 'ssl_certificate|openssl req -x509|https://\$ACCESS_HOST' "$INSTALLER"; then
    fail "the HTTP-only installer must not generate or configure TLS certificates"
fi
require_text 'checksums.txt' "release checksum manifest must be downloaded"
require_text 'SHA-256 mismatch' "release checksum must be enforced"
require_text 'resolve_release_version' "latest release metadata must be resolved before downloading"
require_text 'installed_release_is_current "$RESOLVED_VERSION"' \
    "an already-installed matching release must be detected"
require_text 'download skipped' "matching installations must report that the download was skipped"
require_text 'probe_visual_console_binary "$BINARY_PATH"' \
    "version matching must also verify that the installed binary contains the UI"
require_text 'releases/download/$RESOLVED_VERSION' \
    "resolved latest versions must pin the subsequent asset download"
require_text 'codebase-memory-mcp-ui-linux-amd64-portable.tar.gz' \
    "server deployment must download the UI release asset"
require_text 'codebase-memory-mcp-ui-linux-arm64-portable.tar.gz' \
    "ARM server deployment must download the UI release asset"
require_text 'openssh-client' "server deployment must install the non-interactive SSH client"
require_text 'ensure_git_ssh_identity' "server deployment must create an isolated Git SSH identity"
require_text 'ssh-keygen -q -t ed25519' "server deployment must generate a deploy key"
require_text 'printf '\''HOME=%s\n'\''' "service processes must use the managed account home"
require_text 'if [ -t 2 ]' "interactive downloads must detect a terminal"
require_text '--progress-bar' "interactive release downloads must show progress"
require_text "tar -xzf \"\$TEMP_DIR/\$asset\" -C \"\$TEMP_DIR/extracted\" codebase-memory-mcp" \
    "only the expected binary may be extracted"
require_text 'create_private_temp_dir' "privileged staging must use a private temporary directory"
require_text "grep -Fq 'serve --bind'" "old non-server release binaries must be rejected"
require_text 'verify_visual_console_binary' "the embedded visual console must be probed before install"
require_text 'serve --bind=127.0.0.1' "MCP backend must bind to loopback"
require_text 'listen $LISTEN_ADDRESS:$PUBLIC_PORT;' \
    "the HTTP entry must honor loopback and explicit public modes"
require_text 'location = /mcp' "Nginx must expose only the exact MCP path"
require_text 'if $ALLOW_ALL || [ "${#ALLOW_CIDRS[@]}" -eq 0 ]; then' \
    "MCP must not apply an IP allowlist unless the user explicitly requests one"
require_text 'codebase-memory-mcp-ui.service' "visual console must have an independent systemd unit"
require_text 'console --no-open --port=$UI_BACKEND_PORT' \
    "visual console must bind through its loopback-only command"
require_text 'auth_basic_user_file "$UI_AUTH_FILE"' \
    "visual console must require a separate browser password"
require_text 'location / {' "Nginx must expose the visual-console route"
ui_location="$(sed -n '/^[[:space:]]*location \/ {/,/^[[:space:]]*}/p' "$INSTALLER")"
grep -Fq 'allow all;' <<<"$ui_location" ||
    fail "password-protected visual-console route must not depend on client IP"
grep -Fq 'auth_basic ' <<<"$ui_location" ||
    fail "public visual-console route must keep browser authentication enabled"
require_text 'proxy_set_header Host 127.0.0.1:$UI_BACKEND_PORT' \
    "visual-console proxy must preserve the upstream loopback Host guard"
require_text 'remove_legacy_tls_material' \
    "upgrades must remove certificates generated by the old installer"
require_text 'location ^~ /admin/v1/' \
    "local-only administration endpoints must remain blocked remotely"
require_text 'CBM_MCP_TRUSTED_PROXIES="127.0.0.1/32"' "trusted proxy must be constrained"
require_text 'remote-key create' "managed authentication must be initialized"
require_text 'NoNewPrivileges=true' "systemd hardening must remain enabled"
require_text 'ProtectSystem=strict' "systemd filesystem protection must remain enabled"
require_text 'ReadWritePaths=' "allowed-root updates must reset the systemd path sandbox"
require_text 'management_write_allowed_root_overrides' \
    "allowed-root updates must update both service sandboxes"
require_text 'preserve_existing_allowed_root' \
    "idempotent reinstall must preserve an existing allowed root"
require_text 'systemd-analyze verify' "the generated systemd unit must be validated"
require_text 'rotate 30' "audit log rotation must be configured"

if grep -Eq 'proxy_pass[^;]*/admin/v1' "$INSTALLER"; then
    fail "remote Nginx config must not expose the local admin API"
fi

extract_function() {
    local name="$1"
    sed -n "/^${name}()/,/^}/p" "$INSTALLER"
}

# Exercise the network-mode decision without running the privileged installer.
eval "$(extract_function configure_listen_mode)"
warn() { :; }
info() { :; }
detect_server_address() { printf '%s\n' "10.20.30.40"; }
ALLOW_ALL=false
ALLOW_CIDRS=()
PUBLIC=false
configure_listen_mode
[ "$LISTEN_ADDRESS" = "127.0.0.1" ] || fail "default mode must bind to loopback"
[ "$ACCESS_HOST" = "127.0.0.1" ] || fail "default client URL must use loopback"
PUBLIC=true
configure_listen_mode
[ "$LISTEN_ADDRESS" = "0.0.0.0" ] || fail "--public must bind to all IPv4 interfaces"
[ "$ACCESS_HOST" = "10.20.30.40" ] || fail "public summary must use the detected server IP"

# Exercise the version decision helpers without running the privileged installer.
eval "$(extract_function resolve_release_version)"
eval "$(extract_function binary_version)"
eval "$(extract_function installed_release_is_current)"

VERSION="latest"
REPOSITORY="example/project"
RESOLVED_VERSION="latest"
curl() {
    printf '%s\n' '{"id":1,"tag_name":"v1.2.3","name":"release"}'
}
resolve_release_version
[ "$RESOLVED_VERSION" = "v1.2.3" ] || fail "latest release tag must be parsed"

curl() { return 1; }
resolve_release_version
[ "$RESOLVED_VERSION" = "latest" ] || fail "metadata failures must use the download fallback"

VERSION="v2.0.0"
resolve_release_version
[ "$RESOLVED_VERSION" = "v2.0.0" ] || fail "explicit release tags must remain unchanged"

contract_tmp="$(mktemp -d)"
trap 'rm -rf -- "$contract_tmp"' EXIT
BINARY_PATH="$contract_tmp/codebase-memory-mcp"
cat >"$BINARY_PATH" <<'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = "--version" ]; then
    echo "codebase-memory-mcp 1.2.3"
elif [ "${1:-}" = "--help" ]; then
    echo "serve --bind"
    echo "remote-key"
fi
EOF
chmod +x "$BINARY_PATH"

PROBE_RESULT=0
probe_visual_console_binary() { return "$PROBE_RESULT"; }
installed_release_is_current "v1.2.3" || fail "matching complete installations must be reused"
if installed_release_is_current "v1.2.4"; then
    fail "a different installed version must not be reused"
fi
PROBE_RESULT=1
if installed_release_is_current "v1.2.3"; then
    fail "a matching binary without a working visual console must not be reused"
fi

echo "PASS: Linux server installer syntax and security contracts are enforced"
