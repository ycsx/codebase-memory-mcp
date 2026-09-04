#!/usr/bin/env bash
# Install the remote MCP service and browser console on a single Ubuntu/Debian server.
set -euo pipefail

REPOSITORY="ycsx/codebase-memory-mcp"
VERSION="latest"
RESOLVED_VERSION="latest"
PUBLIC_PORT=9766
BACKEND_PORT=19766
UI_BACKEND_PORT=19749
PUBLIC=false
LISTEN_ADDRESS="127.0.0.1"
ACCESS_HOST="127.0.0.1"
ALLOWED_ROOT="/var/lib/codebase-memory-mcp/repos"
ALLOWED_ROOT_EXPLICIT=false
ALLOW_ALL=false
SKIP_PACKAGES=false
LOCAL_BINARY=""
SOURCE_BINARY=""
ADMIN_PRINCIPAL="platform-admin"
ALLOW_CIDRS=()

SERVICE_USER="codebase-memory-mcp"
SERVICE_GROUP="codebase-memory-mcp"
STATE_DIR="/var/lib/codebase-memory-mcp"
LOG_DIR="/var/log/codebase-memory-mcp"
CONFIG_DIR="/etc/codebase-memory-mcp"
BINARY_PATH="/usr/local/bin/codebase-memory-mcp"
MANAGER_PATH="/usr/local/sbin/cbm-server"
AUTH_STORE="$STATE_DIR/remote-auth.json"
AUDIT_LOG="$LOG_DIR/audit.jsonl"
GIT_SSH_DIR="$STATE_DIR/.ssh"
GIT_SSH_PRIVATE_KEY="$GIT_SSH_DIR/id_ed25519"
GIT_SSH_PUBLIC_KEY="$GIT_SSH_PRIVATE_KEY.pub"
ENV_FILE="$CONFIG_DIR/server.env"
UNIT_FILE="/etc/systemd/system/codebase-memory-mcp.service"
UI_UNIT_FILE="/etc/systemd/system/codebase-memory-mcp-ui.service"
NGINX_FILE="/etc/nginx/conf.d/codebase-memory-mcp.conf"
UI_AUTH_FILE="/etc/nginx/codebase-memory-mcp.htpasswd"
UI_USER="admin"
LOGROTATE_FILE="/etc/logrotate.d/codebase-memory-mcp"
TEMP_DIR=""
PROBE_PID=""
INITIAL_UI_PASSWORD=""

info() { printf '  %s\n' "$*"; }
ok() { printf 'OK: %s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

cleanup() {
    if [ -n "$PROBE_PID" ]; then
        kill "$PROBE_PID" 2>/dev/null || true
        wait "$PROBE_PID" 2>/dev/null || true
    fi
    if [ -n "$TEMP_DIR" ] && [ -d "$TEMP_DIR" ]; then
        rm -rf -- "$TEMP_DIR"
    fi
}
trap cleanup EXIT

usage() {
    cat <<'EOF'
Install codebase-memory-mcp as a single-node Ubuntu/Debian service with a
browser-based visual console and one HTTP reverse-proxy port.

Usage:
  sudo bash deploy/install-server.sh [options]

Default mode:
  Run without arguments. Nginx listens on 127.0.0.1:9766 for a company HTTPS
  reverse proxy on the same host. The installer does not manage TLS material.

Optional overrides:
  --public               Listen on 0.0.0.0 for temporary plain-HTTP testing
  --allow-cidr CIDR      Opt in to an Nginx client-IP allowlist; may repeat
  --allow-all            Explicitly disable the IP allowlist (the default)

  --version TAG          Release tag to install (default: latest)
  --repo OWNER/REPO      Release repository (default: ycsx/codebase-memory-mcp)
  --binary FILE          Install a trusted local binary instead of a release
  --port PORT            HTTP entry port (default: 9766)
  --public-port PORT     Compatibility alias for --port
  --backend-port PORT    Loopback MCP port (default: 19766)
  --ui-port PORT         Loopback visual-console port (default: 19749)
  --allowed-root DIR     Only index repositories below this directory
  --admin-principal ID   Principal for the first admin key
  --skip-packages        Do not install required operating-system packages
  --help                 Show this help

The script is idempotent. It preserves an existing Key Store and only creates
an initial admin key when the store does not exist. Public mode sends browser
passwords and MCP Bearer Keys over cleartext HTTP; use it only for testing.
EOF
}

management_usage() {
    cat <<'EOF'
Manage a codebase-memory-mcp server installation.

Usage:
  cbm-server keys
  cbm-server new-key PRINCIPAL PROJECTS [user|ci|admin]
  cbm-server rotate-key KEY_ID
  cbm-server revoke-key KEY_ID
  cbm-server key list
  cbm-server key create PRINCIPAL PROJECTS [user|ci|admin]
  cbm-server key rotate KEY_ID
  cbm-server key revoke KEY_ID
  cbm-server status
  cbm-server ui-status
  cbm-server restart
  cbm-server logs [LINES]
  cbm-server ui-logs [LINES]
  cbm-server ui-password-reset
  cbm-server git-key
  cbm-server allowed-root
  cbm-server set-allowed-root DIR
  cbm-server config
  cbm-server paths

Examples:
  cbm-server new-key alice project-a,project-b
  cbm-server new-key build-bot project-a ci
  cbm-server new-key platform-admin '*' admin

New and rotated plaintext keys are printed once. `key list` shows metadata only;
an existing plaintext key cannot be recovered because only its hash is stored.
The visual-console password is also stored as a one-way hash. Resetting it
prints a new password once.
`git-key` prints the service account's public SSH deploy key. Add that key to
private repositories before indexing them with an SSH URL.
`set-allowed-root` changes the server directory exposed by the local-folder
picker, updates the systemd filesystem sandbox, and restarts both services.
EOF
}

nginx_runtime_group() {
    local candidate
    for candidate in www-data nginx; do
        if getent group "$candidate" >/dev/null 2>&1; then
            printf '%s\n' "$candidate"
            return
        fi
    done
    candidate="$(awk '$1 == "user" { gsub(/;/, "", $2); print $2; exit }' \
        /etc/nginx/nginx.conf 2>/dev/null || true)"
    [ -n "$candidate" ] && getent group "$candidate" >/dev/null 2>&1 ||
        die "cannot determine the Nginx worker group"
    printf '%s\n' "$candidate"
}

reset_ui_password() {
    local password staged auth_group
    [ ! -L "$UI_AUTH_FILE" ] || die "visual-console password file must not be a symbolic link"
    password="$(openssl rand -hex 16)"
    staged="$(mktemp)"
    auth_group="$(nginx_runtime_group)"
    chmod 0600 "$staged"
    printf '%s\n' "$password" | htpasswd -ciB "$staged" "$UI_USER" >/dev/null
    install -o root -g "$auth_group" -m 0640 "$staged" "$UI_AUTH_FILE"
    rm -f -- "$staged"
    printf 'Visual console username: %s\n' "$UI_USER"
    printf 'Visual console password: %s\n' "$password"
    warn "The plaintext password above is shown once; store it now."
}

management_require_root() {
    [ "${EUID:-$(id -u)}" -eq 0 ] || die "run cbm-server with sudo"
}

management_require_key_store() {
    [ -x "$BINARY_PATH" ] || die "server binary not found: $BINARY_PATH"
    [ -f "$AUTH_STORE" ] && [ ! -L "$AUTH_STORE" ] ||
        die "managed Key Store not found: $AUTH_STORE"
}

management_key() {
    local operation="${1:-list}"
    [ "$#" -eq 0 ] || shift
    case "$operation" in
        list|show)
            [ "$#" -eq 0 ] || die "key $operation takes no additional arguments"
            runuser --user "$SERVICE_USER" -- "$BINARY_PATH" remote-key list \
                --store="$AUTH_STORE"
            info "Key metadata shown; stored plaintext keys cannot be recovered."
            ;;
        create|new)
            [ "$#" -ge 2 ] && [ "$#" -le 3 ] ||
                die "usage: cbm-server key create PRINCIPAL PROJECTS [user|ci|admin]"
            local principal="$1"
            local projects="$2"
            local kind="${3:-user}"
            local options=(remote-key create "--store=$AUTH_STORE" "--principal=$principal"
                "--kind=$kind" --profile=analysis "--projects=$projects" --source-read)
            case "$kind" in
                user) ;;
                ci) options+=(--index) ;;
                admin) options+=(--index --delete --admin) ;;
                *) die "key kind must be user, ci, or admin" ;;
            esac
            runuser --user "$SERVICE_USER" -- "$BINARY_PATH" "${options[@]}"
            warn "The plaintext key above is shown once; store it now."
            ;;
        rotate)
            [ "$#" -eq 1 ] || die "usage: cbm-server key rotate KEY_ID"
            runuser --user "$SERVICE_USER" -- "$BINARY_PATH" remote-key rotate "$1" \
                --store="$AUTH_STORE"
            warn "The replacement plaintext key above is shown once; store it now."
            ;;
        revoke)
            [ "$#" -eq 1 ] || die "usage: cbm-server key revoke KEY_ID"
            runuser --user "$SERVICE_USER" -- "$BINARY_PATH" remote-key revoke "$1" \
                --store="$AUTH_STORE"
            ok "Key revoked: $1"
            ;;
        *) die "unknown key operation: $operation" ;;
    esac
}

configured_allowed_root() {
    [ -f "$ENV_FILE" ] && [ ! -L "$ENV_FILE" ] ||
        return 1
    local line value=""
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            CBM_ALLOWED_ROOT=*)
                value="${line#CBM_ALLOWED_ROOT=}"
                break
                ;;
        esac
    done < "$ENV_FILE"
    [ -n "$value" ] || return 1
    case "$value" in
        \"*\")
            value="${value#\"}"
            value="${value%\"}"
            ;;
    esac
    printf '%s\n' "$value"
}

management_current_allowed_root() {
    local value
    value="$(configured_allowed_root)" ||
        die "CBM_ALLOWED_ROOT is not configured in $ENV_FILE"
    printf '%s\n' "$value"
}

management_write_allowed_root() {
    local root="$1"
    local staged line
    local replaced=false
    staged="$(mktemp "$CONFIG_DIR/.server.env.XXXXXX")"
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            CBM_ALLOWED_ROOT=*)
                printf 'CBM_ALLOWED_ROOT=%s\n' "$(env_quote "$root")" >> "$staged"
                replaced=true
                ;;
            *) printf '%s\n' "$line" >> "$staged" ;;
        esac
    done < "$ENV_FILE"
    if ! $replaced; then
        printf 'CBM_ALLOWED_ROOT=%s\n' "$(env_quote "$root")" >> "$staged"
    fi
    chown root:"$SERVICE_GROUP" "$staged"
    chmod 0640 "$staged"
    mv -f -- "$staged" "$ENV_FILE"
}

management_write_allowed_root_overrides() {
    local root="$1"
    local service drop_dir staged
    for service in codebase-memory-mcp codebase-memory-mcp-ui; do
        drop_dir="/etc/systemd/system/$service.service.d"
        install -d -o root -g root -m 0755 "$drop_dir"
        staged="$(mktemp "$drop_dir/.allowed-root.conf.XXXXXX")"
        cat > "$staged" <<EOF
[Service]
ReadWritePaths=
ReadWritePaths="$STATE_DIR"
ReadWritePaths="$LOG_DIR"
ReadWritePaths="$root"
EOF
        chown root:root "$staged"
        chmod 0644 "$staged"
        mv -f -- "$staged" "$drop_dir/allowed-root.conf"
    done
}

management_set_allowed_root() {
    [ "$#" -eq 1 ] || die "usage: cbm-server set-allowed-root DIR"
    local requested="$1"
    [[ "$requested" = /* ]] || die "allowed root must be an absolute path"
    case "$requested" in
        *$'\n'*|*$'\r'*|*\"*|*\\*|*%*) die "allowed root contains unsafe characters" ;;
    esac
    [ -d "$requested" ] && [ ! -L "$requested" ] ||
        die "allowed root must be a real directory: $requested"
    local canonical
    canonical="$(readlink -f -- "$requested")" || die "cannot resolve allowed root: $requested"
    runuser --user "$SERVICE_USER" -- test -r "$canonical" ||
        die "$SERVICE_USER cannot read allowed root: $canonical"
    runuser --user "$SERVICE_USER" -- test -x "$canonical" ||
        die "$SERVICE_USER cannot traverse allowed root: $canonical"

    management_write_allowed_root "$canonical"
    management_write_allowed_root_overrides "$canonical"
    systemctl daemon-reload
    systemd-analyze verify "$UNIT_FILE" "$UI_UNIT_FILE"
    systemctl restart codebase-memory-mcp codebase-memory-mcp-ui
    systemctl is-active --quiet codebase-memory-mcp ||
        die "service failed to restart; inspect with: sudo cbm-server logs"
    systemctl is-active --quiet codebase-memory-mcp-ui ||
        die "visual console failed to restart; inspect with: sudo cbm-server ui-logs"
    ok "Allowed root changed to $canonical"
    info "Reopen New Index; the folder picker is now confined to this directory."
}

management_main() {
    local command_name="${1:-help}"
    [ "$#" -eq 0 ] || shift
    case "$command_name" in
        help|--help|-h)
            management_usage
            ;;
        key|keys)
            management_require_root
            management_require_key_store
            management_key "$@"
            ;;
        new-key|generate-key)
            management_require_root
            management_require_key_store
            management_key create "$@"
            ;;
        rotate-key)
            management_require_root
            management_require_key_store
            management_key rotate "$@"
            ;;
        revoke-key)
            management_require_root
            management_require_key_store
            management_key revoke "$@"
            ;;
        status)
            management_require_root
            [ "$#" -eq 0 ] || die "status takes no arguments"
            systemctl --no-pager --full status codebase-memory-mcp \
                codebase-memory-mcp-ui
            ;;
        ui-status)
            management_require_root
            [ "$#" -eq 0 ] || die "ui-status takes no arguments"
            systemctl --no-pager --full status codebase-memory-mcp-ui
            ;;
        restart)
            management_require_root
            [ "$#" -eq 0 ] || die "restart takes no arguments"
            systemctl restart codebase-memory-mcp codebase-memory-mcp-ui
            systemctl is-active --quiet codebase-memory-mcp || die "service failed to restart"
            systemctl is-active --quiet codebase-memory-mcp-ui ||
                die "visual console failed to restart"
            ok "Remote MCP and visual-console services restarted"
            ;;
        logs)
            management_require_root
            [ "$#" -le 1 ] || die "usage: cbm-server logs [LINES]"
            local lines="${1:-100}"
            case "$lines" in
                ''|*[!0-9]*) die "log line count must be an integer" ;;
            esac
            [ "$lines" -ge 1 ] && [ "$lines" -le 10000 ] ||
                die "log line count must be between 1 and 10000"
            journalctl -u codebase-memory-mcp -u codebase-memory-mcp-ui \
                -n "$lines" --no-pager
            ;;
        ui-logs)
            management_require_root
            [ "$#" -le 1 ] || die "usage: cbm-server ui-logs [LINES]"
            local ui_lines="${1:-100}"
            case "$ui_lines" in
                ''|*[!0-9]*) die "log line count must be an integer" ;;
            esac
            [ "$ui_lines" -ge 1 ] && [ "$ui_lines" -le 10000 ] ||
                die "log line count must be between 1 and 10000"
            journalctl -u codebase-memory-mcp-ui -n "$ui_lines" --no-pager
            ;;
        ui-password-reset|reset-ui-password)
            management_require_root
            [ "$#" -eq 0 ] || die "ui-password-reset takes no arguments"
            reset_ui_password
            ;;
        git-key|git-deploy-key)
            management_require_root
            [ "$#" -eq 0 ] || die "git-key takes no arguments"
            [ -f "$GIT_SSH_PUBLIC_KEY" ] && [ ! -L "$GIT_SSH_PUBLIC_KEY" ] ||
                die "Git SSH deploy key not found: $GIT_SSH_PUBLIC_KEY"
            cat "$GIT_SSH_PUBLIC_KEY"
            ;;
        allowed-root)
            management_require_root
            [ "$#" -eq 0 ] || die "allowed-root takes no arguments"
            management_current_allowed_root
            ;;
        set-allowed-root)
            management_require_root
            management_set_allowed_root "$@"
            ;;
        config)
            management_require_root
            [ "$#" -eq 0 ] || die "config takes no arguments"
            cat "$ENV_FILE"
            ;;
        paths)
            management_require_root
            [ "$#" -eq 0 ] || die "paths takes no arguments"
            printf 'binary=%s\nmanager=%s\nconfig=%s\nkey_store=%s\nui_auth=%s\n' \
                "$BINARY_PATH" "$MANAGER_PATH" "$ENV_FILE" "$AUTH_STORE" "$UI_AUTH_FILE"
            printf 'audit_log=%s\nstate=%s\n' "$AUDIT_LOG" "$STATE_DIR"
            printf 'allowed_root=%s\n' "$(management_current_allowed_root)"
            ;;
        *) die "unknown cbm-server command: $command_name" ;;
    esac
}

if [ "${0##*/}" = "cbm-server" ]; then
    management_main "$@"
    exit 0
fi

require_value() {
    local option="$1"
    local value="${2:-}"
    [ -n "$value" ] || die "$option requires a value"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --public) PUBLIC=true; shift ;;
        --allow-cidr)
            require_value "$1" "${2:-}"
            ALLOW_CIDRS+=("$2")
            shift 2
            ;;
        --allow-cidr=*) ALLOW_CIDRS+=("${1#*=}"); shift ;;
        --allow-all) ALLOW_ALL=true; shift ;;
        --version)
            require_value "$1" "${2:-}"
            VERSION="$2"
            shift 2
            ;;
        --version=*) VERSION="${1#*=}"; shift ;;
        --repo)
            require_value "$1" "${2:-}"
            REPOSITORY="$2"
            shift 2
            ;;
        --repo=*) REPOSITORY="${1#*=}"; shift ;;
        --binary)
            require_value "$1" "${2:-}"
            LOCAL_BINARY="$2"
            shift 2
            ;;
        --binary=*) LOCAL_BINARY="${1#*=}"; shift ;;
        --port)
            require_value "$1" "${2:-}"
            PUBLIC_PORT="$2"
            shift 2
            ;;
        --port=*) PUBLIC_PORT="${1#*=}"; shift ;;
        --public-port)
            require_value "$1" "${2:-}"
            PUBLIC_PORT="$2"
            shift 2
            ;;
        --public-port=*) PUBLIC_PORT="${1#*=}"; shift ;;
        --backend-port)
            require_value "$1" "${2:-}"
            BACKEND_PORT="$2"
            shift 2
            ;;
        --backend-port=*) BACKEND_PORT="${1#*=}"; shift ;;
        --ui-port)
            require_value "$1" "${2:-}"
            UI_BACKEND_PORT="$2"
            shift 2
            ;;
        --ui-port=*) UI_BACKEND_PORT="${1#*=}"; shift ;;
        --allowed-root)
            require_value "$1" "${2:-}"
            ALLOWED_ROOT="$2"
            ALLOWED_ROOT_EXPLICIT=true
            shift 2
            ;;
        --allowed-root=*) ALLOWED_ROOT="${1#*=}"; ALLOWED_ROOT_EXPLICIT=true; shift ;;
        --admin-principal)
            require_value "$1" "${2:-}"
            ADMIN_PRINCIPAL="$2"
            shift 2
            ;;
        --admin-principal=*) ADMIN_PRINCIPAL="${1#*=}"; shift ;;
        --skip-packages) SKIP_PACKAGES=true; shift ;;
        --help|-h)
            usage
            exit 0
            ;;
        *) die "unknown argument: $1" ;;
    esac
done

preserve_existing_allowed_root() {
    $ALLOWED_ROOT_EXPLICIT && return
    local existing
    if existing="$(configured_allowed_root)"; then
        ALLOWED_ROOT="$existing"
        info "Existing allowed root preserved: $ALLOWED_ROOT"
    fi
}

is_ipv4_address() {
    local address="$1"
    local first second third fourth octet
    [[ "$address" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || return 1
    IFS=. read -r first second third fourth <<<"$address"
    for octet in "$first" "$second" "$third" "$fourth"; do
        [ "$octet" -le 255 ] 2>/dev/null || return 1
    done
}

detect_server_address() {
    local candidate=""
    if command -v ip >/dev/null 2>&1; then
        candidate="$(ip -4 route get 1.1.1.1 2>/dev/null |
            awk '{ for (i = 1; i <= NF; i++) if ($i == "src") { print $(i + 1); exit } }')"
    fi
    if ! is_ipv4_address "$candidate" && command -v hostname >/dev/null 2>&1; then
        local address
        for address in $(hostname -I 2>/dev/null || true); do
            if is_ipv4_address "$address" && [[ "$address" != 127.* ]]; then
                candidate="$address"
                break
            fi
        done
    fi
    if is_ipv4_address "$candidate"; then
        printf '%s\n' "$candidate"
        return
    fi
    return 1
}

configure_listen_mode() {
    if $PUBLIC; then
        LISTEN_ADDRESS="0.0.0.0"
        ACCESS_HOST="$(detect_server_address || true)"
        [ -n "$ACCESS_HOST" ] || ACCESS_HOST="server-ip"
        warn "Public test mode exposes credentials over cleartext HTTP"
    else
        LISTEN_ADDRESS="127.0.0.1"
        ACCESS_HOST="127.0.0.1"
        info "Loopback mode enabled; terminate HTTPS at the company reverse proxy"
    fi
    if ! $ALLOW_ALL && [ "${#ALLOW_CIDRS[@]}" -eq 0 ]; then
        info "No client-IP allowlist configured; application credentials remain required"
    fi
}

validate_port() {
    local name="$1"
    local port="$2"
    case "$port" in
        ''|*[!0-9]*) die "$name must be an integer" ;;
    esac
    [ "$port" -ge 1 ] && [ "$port" -le 65535 ] || die "$name must be between 1 and 65535"
}

validate_inputs() {
    [ "$(uname -s)" = "Linux" ] || die "this installer supports Linux only"
    [ "${EUID:-$(id -u)}" -eq 0 ] || die "run this script as root (sudo bash ...)"
    [[ "$REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] ||
        die "--repo must be OWNER/REPO"
    [[ "$VERSION" =~ ^[A-Za-z0-9._-]+$ ]] || die "--version contains invalid characters"
    [[ "$ADMIN_PRINCIPAL" =~ ^[A-Za-z0-9@._:-]+$ ]] ||
        die "--admin-principal contains invalid characters"
    [[ "$ALLOWED_ROOT" = /* ]] || die "--allowed-root must be an absolute path"
    validate_port "--port" "$PUBLIC_PORT"
    validate_port "--backend-port" "$BACKEND_PORT"
    validate_port "--ui-port" "$UI_BACKEND_PORT"
    [ "$PUBLIC_PORT" -ne "$BACKEND_PORT" ] && [ "$PUBLIC_PORT" -ne "$UI_BACKEND_PORT" ] &&
        [ "$BACKEND_PORT" -ne "$UI_BACKEND_PORT" ] || die "public, MCP, and UI ports must differ"
    case "$ALLOWED_ROOT" in
        *$'\n'*|*$'\r'*|*\"*) die "--allowed-root contains unsafe characters" ;;
    esac

    if $ALLOW_ALL; then
        [ "${#ALLOW_CIDRS[@]}" -eq 0 ] || die "--allow-all cannot be combined with --allow-cidr"
    fi
    local cidr
    for cidr in "${ALLOW_CIDRS[@]}"; do
        [[ "$cidr" =~ ^[0-9A-Fa-f:./]+$ ]] || die "invalid CIDR: $cidr"
    done
}

install_packages() {
    if $SKIP_PACKAGES; then
        info "Skipping operating-system package installation"
        return
    fi
    command -v apt-get >/dev/null 2>&1 ||
        die "automatic package installation currently supports Ubuntu/Debian; use --skip-packages after installing dependencies"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y nginx curl ca-certificates git logrotate openssl apache2-utils \
        openssh-client
}

require_commands() {
    local command_name
    for command_name in awk chmod chown curl getent grep groupadd htpasswd install logrotate \
        mktemp nginx openssl readlink runuser sed sha256sum ssh ssh-keygen systemctl \
        systemd-analyze tar useradd; do
        command -v "$command_name" >/dev/null 2>&1 || die "required command not found: $command_name"
    done
}

create_private_temp_dir() {
    TEMP_DIR="$(mktemp -d)"
    chmod 0700 "$TEMP_DIR"
}

ensure_account_and_directories() {
    if ! getent group "$SERVICE_GROUP" >/dev/null 2>&1; then
        groupadd --system "$SERVICE_GROUP"
    fi
    if ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
        local nologin_shell="/usr/sbin/nologin"
        [ -x "$nologin_shell" ] || nologin_shell="/sbin/nologin"
        [ -x "$nologin_shell" ] || nologin_shell="/bin/false"
        useradd --system --gid "$SERVICE_GROUP" --home-dir "$STATE_DIR" \
            --shell "$nologin_shell" "$SERVICE_USER"
    fi

    install -d -o "$SERVICE_USER" -g "$SERVICE_GROUP" -m 0750 \
        "$STATE_DIR" "$STATE_DIR/repos" "$LOG_DIR"
    install -d -o root -g "$SERVICE_GROUP" -m 0750 "$CONFIG_DIR"

    if [ ! -e "$ALLOWED_ROOT" ]; then
        install -d -o "$SERVICE_USER" -g "$SERVICE_GROUP" -m 0750 "$ALLOWED_ROOT"
    fi
    [ -d "$ALLOWED_ROOT" ] && [ ! -L "$ALLOWED_ROOT" ] ||
        die "allowed root must be a real directory: $ALLOWED_ROOT"
    runuser --user "$SERVICE_USER" -- test -r "$ALLOWED_ROOT" ||
        die "$SERVICE_USER cannot read allowed root: $ALLOWED_ROOT"
    runuser --user "$SERVICE_USER" -- test -x "$ALLOWED_ROOT" ||
        die "$SERVICE_USER cannot traverse allowed root: $ALLOWED_ROOT"
}

ensure_git_ssh_identity() {
    [ ! -L "$GIT_SSH_DIR" ] || die "Git SSH directory must not be a symbolic link"
    install -d -o "$SERVICE_USER" -g "$SERVICE_GROUP" -m 0700 "$GIT_SSH_DIR"

    if [ ! -e "$GIT_SSH_PRIVATE_KEY" ]; then
        [ ! -e "$GIT_SSH_PUBLIC_KEY" ] ||
            die "Git SSH public key exists without its private key: $GIT_SSH_PUBLIC_KEY"
        runuser --user "$SERVICE_USER" -- ssh-keygen -q -t ed25519 -N "" \
            -C "codebase-memory-mcp deploy key" -f "$GIT_SSH_PRIVATE_KEY"
        ok "Git SSH deploy key generated"
    fi

    [ -f "$GIT_SSH_PRIVATE_KEY" ] && [ ! -L "$GIT_SSH_PRIVATE_KEY" ] ||
        die "Git SSH private key must be a regular file: $GIT_SSH_PRIVATE_KEY"
    runuser --user "$SERVICE_USER" -- ssh-keygen -y -P "" -f "$GIT_SSH_PRIVATE_KEY" \
        >/dev/null 2>&1 || die "Git SSH deploy key must not require a passphrase"
    chmod 0600 "$GIT_SSH_PRIVATE_KEY"
    chown "$SERVICE_USER:$SERVICE_GROUP" "$GIT_SSH_PRIVATE_KEY"

    if [ ! -e "$GIT_SSH_PUBLIC_KEY" ]; then
        local staged_public="$TEMP_DIR/git-deploy-key.pub"
        runuser --user "$SERVICE_USER" -- ssh-keygen -y -P "" -f "$GIT_SSH_PRIVATE_KEY" \
            > "$staged_public"
        install -o "$SERVICE_USER" -g "$SERVICE_GROUP" -m 0644 \
            "$staged_public" "$GIT_SSH_PUBLIC_KEY"
    fi
    [ -f "$GIT_SSH_PUBLIC_KEY" ] && [ ! -L "$GIT_SSH_PUBLIC_KEY" ] ||
        die "Git SSH public key must be a regular file: $GIT_SSH_PUBLIC_KEY"
    chmod 0644 "$GIT_SSH_PUBLIC_KEY"
    chown "$SERVICE_USER:$SERVICE_GROUP" "$GIT_SSH_PUBLIC_KEY"
}

release_asset_name() {
    local machine
    machine="$(uname -m)"
    case "$machine" in
        x86_64|amd64) printf '%s\n' "codebase-memory-mcp-ui-linux-amd64-portable.tar.gz" ;;
        aarch64|arm64) printf '%s\n' "codebase-memory-mcp-ui-linux-arm64-portable.tar.gz" ;;
        *) die "unsupported Linux architecture: $machine" ;;
    esac
}

download_file() {
    local url="$1"
    local output="$2"
    if [ -t 2 ]; then
        curl --fail --location --show-error --retry 3 --progress-bar \
            --output "$output" "$url"
    else
        curl --fail --location --silent --show-error --retry 3 \
            --output "$output" "$url"
    fi
}

resolve_release_version() {
    if [ "$VERSION" != "latest" ]; then
        RESOLVED_VERSION="$VERSION"
        return
    fi

    local release_json resolved tag_pattern
    if ! release_json="$(curl --fail --location --silent --show-error --retry 3 \
        --header 'Accept: application/vnd.github+json' \
        --header 'X-GitHub-Api-Version: 2022-11-28' \
        "https://api.github.com/repos/$REPOSITORY/releases/latest")"; then
        warn "Cannot resolve the latest release version; continuing with a normal download"
        RESOLVED_VERSION="latest"
        return
    fi
    tag_pattern='"tag_name"[[:space:]]*:[[:space:]]*"([^"]+)"'
    if [[ "$release_json" =~ $tag_pattern ]]; then
        resolved="${BASH_REMATCH[1]}"
    else
        resolved=""
    fi
    if [[ ! "$resolved" =~ ^[A-Za-z0-9._-]+$ ]]; then
        warn "Latest release metadata has no valid tag; continuing with a normal download"
        RESOLVED_VERSION="latest"
        return
    fi
    RESOLVED_VERSION="$resolved"
    info "Latest release: $RESOLVED_VERSION"
}

binary_version() {
    local binary="$1"
    local output version
    output="$("$binary" --version 2>/dev/null)" || return 1
    version="$(awk 'NR == 1 && $1 == "codebase-memory-mcp" { print $2; exit }' <<<"$output")"
    [[ "$version" =~ ^[A-Za-z0-9._-]+$ ]] || return 1
    printf '%s\n' "${version#v}"
}

download_release_binary() {
    local asset base expected actual extracted
    asset="$(release_asset_name)"
    if [ "$RESOLVED_VERSION" = "latest" ]; then
        base="https://github.com/$REPOSITORY/releases/latest/download"
    else
        base="https://github.com/$REPOSITORY/releases/download/$RESOLVED_VERSION"
    fi

    info "Downloading $asset from $REPOSITORY ($RESOLVED_VERSION)"
    download_file "$base/$asset" "$TEMP_DIR/$asset"
    info "Downloading checksum manifest"
    download_file "$base/checksums.txt" "$TEMP_DIR/checksums.txt"

    expected="$(awk -v file="$asset" '$2 == file || $2 == ("*" file) { print $1; exit }' \
        "$TEMP_DIR/checksums.txt")"
    [[ "$expected" =~ ^[0-9A-Fa-f]{64}$ ]] || die "$asset is missing from checksums.txt"
    actual="$(sha256sum "$TEMP_DIR/$asset" | awk '{ print $1 }')"
    [ "${actual,,}" = "${expected,,}" ] || die "SHA-256 mismatch for $asset"
    ok "Release checksum verified"

    mkdir "$TEMP_DIR/extracted"
    tar -xzf "$TEMP_DIR/$asset" -C "$TEMP_DIR/extracted" codebase-memory-mcp
    extracted="$TEMP_DIR/extracted/codebase-memory-mcp"
    [ -f "$extracted" ] && [ ! -L "$extracted" ] || die "release archive has no regular binary"
    SOURCE_BINARY="$extracted"
}

probe_visual_console_binary() {
    local staged="$1"
    local probe_port probe_dir probe_log probe_cache ready=false
    probe_dir="$(mktemp -d "$TEMP_DIR/console-probe.XXXXXX")"
    probe_log="$probe_dir/console.log"
    probe_cache="$probe_dir/cache"
    mkdir "$probe_cache"

    for _ in 1 2 3 4 5; do
        probe_port=$((30000 + RANDOM % 20000))
        if ! curl --silent --output /dev/null --max-time 1 \
            "http://127.0.0.1:$probe_port/"; then
            break
        fi
    done

    CBM_CACHE_DIR="$probe_cache" "$staged" console --no-open "--port=$probe_port" \
        >"$probe_log" 2>&1 &
    PROBE_PID=$!
    for _ in {1..50}; do
        if curl --silent --fail --output /dev/null --max-time 1 \
            "http://127.0.0.1:$probe_port/"; then
            ready=true
            break
        fi
        kill -0 "$PROBE_PID" 2>/dev/null || break
        sleep 0.1
    done
    kill "$PROBE_PID" 2>/dev/null || true
    wait "$PROBE_PID" 2>/dev/null || true
    PROBE_PID=""

    if ! $ready; then
        sed -n '1,20p' "$probe_log" >&2 || true
        return 1
    fi
}

verify_visual_console_binary() {
    local staged="$1"
    probe_visual_console_binary "$staged" ||
        die "installed binary does not contain a working embedded visual console"
    ok "Embedded visual console verified"
}

installed_release_is_current() {
    local target="$1"
    [ "$target" != "latest" ] || return 1
    [ -f "$BINARY_PATH" ] && [ ! -L "$BINARY_PATH" ] && [ -x "$BINARY_PATH" ] || return 1

    local installed_version binary_help
    installed_version="$(binary_version "$BINARY_PATH")" || return 1
    [ "$installed_version" = "${target#v}" ] || {
        info "Installed version $installed_version differs from target ${target#v}"
        return 1
    }

    binary_help="$("$BINARY_PATH" --help 2>&1 || true)"
    grep -Fq 'serve --bind' <<<"$binary_help" || return 1
    grep -Fq 'remote-key' <<<"$binary_help" || return 1
    if ! probe_visual_console_binary "$BINARY_PATH"; then
        info "Installed $installed_version binary has no working visual console"
        return 1
    fi
    return 0
}

install_binary() {
    if [ -n "$LOCAL_BINARY" ]; then
        [ -f "$LOCAL_BINARY" ] && [ ! -L "$LOCAL_BINARY" ] ||
            die "--binary must reference a regular, trusted file"
        info "Installing trusted local binary: $LOCAL_BINARY"
        SOURCE_BINARY="$LOCAL_BINARY"
    else
        resolve_release_version
        if installed_release_is_current "$RESOLVED_VERSION"; then
            ok "codebase-memory-mcp ${RESOLVED_VERSION#v} with UI is already installed; download skipped"
            return
        fi
        download_release_binary
    fi

    local staged="$TEMP_DIR/codebase-memory-mcp.candidate"
    install -o root -g root -m 0755 "$SOURCE_BINARY" "$staged"
    "$staged" --version
    local binary_help
    binary_help="$("$staged" --help 2>&1 || true)"
    grep -Fq 'serve --bind' <<<"$binary_help" ||
        die "installed binary does not support remote MCP serve; publish a newer release or use --binary"
    grep -Fq 'remote-key' <<<"$binary_help" ||
        die "installed binary does not support managed remote keys"
    verify_visual_console_binary "$staged"
    install -o root -g root -m 0755 "$staged" "$BINARY_PATH"
    ok "Binary installed at $BINARY_PATH"
}

install_management_command() {
    local installer_source
    installer_source="$(readlink -f -- "$0")" || die "cannot resolve installer source"
    [ -f "$installer_source" ] && [ ! -L "$installer_source" ] ||
        die "installer must be a regular file to register cbm-server"
    install -o root -g root -m 0755 "$installer_source" "$MANAGER_PATH"
    ok "Management command installed at $MANAGER_PATH"
}

env_quote() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    printf '"%s"' "$value"
}

write_environment() {
    local staged="$TEMP_DIR/server.env"
    {
        printf 'CBM_MCP_AUTH_STORE=%s\n' "$(env_quote "$AUTH_STORE")"
        printf 'CBM_MCP_AUDIT_LOG=%s\n' "$(env_quote "$AUDIT_LOG")"
        printf 'CBM_CACHE_DIR=%s\n' "$(env_quote "$STATE_DIR")"
        printf 'CBM_ALLOWED_ROOT=%s\n' "$(env_quote "$ALLOWED_ROOT")"
        printf 'HOME=%s\n' "$(env_quote "$STATE_DIR")"
        printf 'CBM_MCP_TRUSTED_PROXIES="127.0.0.1/32"\n'
        printf 'CBM_MCP_SESSION_TTL_SEC=1800\n'
        printf 'CBM_MCP_MAX_SESSIONS=64\n'
        printf 'CBM_MCP_MAX_WORKERS=16\n'
        printf 'CBM_LOG_LEVEL=info\n'
    } > "$staged"
    install -o root -g "$SERVICE_GROUP" -m 0640 "$staged" "$ENV_FILE"
}

write_systemd_units() {
    local staged="$TEMP_DIR/codebase-memory-mcp.service"
    cat > "$staged" <<EOF
[Unit]
Description=Codebase Memory remote MCP server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_GROUP
WorkingDirectory=$STATE_DIR
EnvironmentFile=$ENV_FILE
ExecStart=$BINARY_PATH serve --bind=127.0.0.1 --port=$BACKEND_PORT
Restart=on-failure
RestartSec=3
TimeoutStopSec=30
UMask=0077
LimitNOFILE=65536
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=read-only
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
LockPersonality=true
RestrictSUIDSGID=true
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
ReadWritePaths="$STATE_DIR"
ReadWritePaths="$LOG_DIR"
ReadWritePaths="$ALLOWED_ROOT"

[Install]
WantedBy=multi-user.target
EOF
    systemd-analyze verify "$staged"
    install -o root -g root -m 0644 "$staged" "$UNIT_FILE"

    staged="$TEMP_DIR/codebase-memory-mcp-ui.service"
    cat > "$staged" <<EOF
[Unit]
Description=Codebase Memory browser visual console
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_GROUP
WorkingDirectory=$STATE_DIR
EnvironmentFile=$ENV_FILE
ExecStart=$BINARY_PATH console --no-open --port=$UI_BACKEND_PORT
Restart=on-failure
RestartSec=3
TimeoutStopSec=30
UMask=0077
LimitNOFILE=65536
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=read-only
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
LockPersonality=true
RestrictSUIDSGID=true
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
ReadWritePaths="$STATE_DIR"
ReadWritePaths="$LOG_DIR"
ReadWritePaths="$ALLOWED_ROOT"

[Install]
WantedBy=multi-user.target
EOF
    systemd-analyze verify "$staged"
    install -o root -g root -m 0644 "$staged" "$UI_UNIT_FILE"

    # A previous `cbm-server set-allowed-root` may have installed these managed
    # overrides. The freshly generated base units now contain the same current
    # root, so remove only our own stale override files.
    rm -f -- \
        /etc/systemd/system/codebase-memory-mcp.service.d/allowed-root.conf \
        /etc/systemd/system/codebase-memory-mcp-ui.service.d/allowed-root.conf
}

write_nginx_config() {
    local staged="$TEMP_DIR/codebase-memory-mcp.nginx.conf"
    {
        cat <<EOF
server {
    listen $LISTEN_ADDRESS:$PUBLIC_PORT;
    server_name _;
    server_tokens off;

    location = /mcp {
        allow 127.0.0.1;
        allow ::1;
EOF
        if $ALLOW_ALL || [ "${#ALLOW_CIDRS[@]}" -eq 0 ]; then
            printf '        allow all;\n'
        else
            local cidr
            for cidr in "${ALLOW_CIDRS[@]}"; do
                printf '        allow %s;\n' "$cidr"
            done
            printf '        deny all;\n'
        fi
        cat <<EOF

        proxy_pass http://127.0.0.1:$BACKEND_PORT/mcp;
        proxy_http_version 1.1;
        proxy_set_header Host \$host;
        proxy_set_header Authorization \$http_authorization;
        proxy_set_header X-Forwarded-For \$remote_addr;
        proxy_read_timeout 180s;
        proxy_send_timeout 180s;
        client_max_body_size 1m;
    }

    location = /mcp/ {
        return 404;
    }

    location = /admin/v1 {
        return 404;
    }

    location ^~ /admin/v1/ {
        return 404;
    }

    location / {
        allow all;

        auth_basic "Codebase Memory visual console";
        auth_basic_user_file "$UI_AUTH_FILE";

        proxy_pass http://127.0.0.1:$UI_BACKEND_PORT;
        proxy_http_version 1.1;
        proxy_set_header Host 127.0.0.1:$UI_BACKEND_PORT;
        proxy_set_header Authorization "";
        proxy_set_header X-Forwarded-For \$remote_addr;
        proxy_set_header X-Forwarded-Proto \$scheme;
        proxy_read_timeout 180s;
        proxy_send_timeout 180s;
        client_max_body_size 10m;
    }
}
EOF
    } > "$staged"
    install -o root -g root -m 0644 "$staged" "$NGINX_FILE"
}

remove_legacy_tls_material() {
    local legacy_tls_dir="$CONFIG_DIR/tls"
    rm -f -- "$legacy_tls_dir/server.crt" "$legacy_tls_dir/server.key"
    rmdir -- "$legacy_tls_dir" 2>/dev/null || true
}

write_logrotate_config() {
    local staged="$TEMP_DIR/codebase-memory-mcp.logrotate"
    cat > "$staged" <<EOF
$AUDIT_LOG {
    daily
    rotate 30
    compress
    delaycompress
    missingok
    notifempty
    create 0600 $SERVICE_USER $SERVICE_GROUP
    su $SERVICE_USER $SERVICE_GROUP
}
EOF
    logrotate --debug "$staged" >/dev/null
    install -o root -g root -m 0644 "$staged" "$LOGROTATE_FILE"
}

ensure_admin_key() {
    if [ -e "$AUTH_STORE" ]; then
        [ -f "$AUTH_STORE" ] && [ ! -L "$AUTH_STORE" ] || die "Key Store is not a regular file"
        chown "$SERVICE_USER:$SERVICE_GROUP" "$AUTH_STORE"
        chmod 0600 "$AUTH_STORE"
        info "Existing managed Key Store preserved"
        return
    fi

    printf '\n=== INITIAL ADMIN KEY (shown once) ===\n'
    runuser --user "$SERVICE_USER" -- "$BINARY_PATH" remote-key create \
        --store="$AUTH_STORE" --principal="$ADMIN_PRINCIPAL" --kind=admin \
        --projects='*' --source-read --index --delete --admin
    printf '=== STORE THIS KEY IN A PASSWORD MANAGER ===\n\n'
    chmod 0600 "$AUTH_STORE"
}

ensure_ui_credentials() {
    if [ -e "$UI_AUTH_FILE" ]; then
        [ -f "$UI_AUTH_FILE" ] && [ ! -L "$UI_AUTH_FILE" ] ||
            die "visual-console password file is not a regular file"
        chown "root:$(nginx_runtime_group)" "$UI_AUTH_FILE"
        chmod 0640 "$UI_AUTH_FILE"
        info "Existing visual-console password preserved"
        return
    fi

    INITIAL_UI_PASSWORD="$(openssl rand -hex 16)"
    local staged="$TEMP_DIR/ui.htpasswd"
    printf '%s\n' "$INITIAL_UI_PASSWORD" | htpasswd -ciB "$staged" "$UI_USER" >/dev/null
    install -o root -g "$(nginx_runtime_group)" -m 0640 "$staged" "$UI_AUTH_FILE"
}

start_and_verify() {
    systemctl daemon-reload
    systemctl enable codebase-memory-mcp >/dev/null
    systemctl enable codebase-memory-mcp-ui >/dev/null
    if ! systemctl restart codebase-memory-mcp; then
        journalctl -u codebase-memory-mcp -n 30 --no-pager >&2 || true
        die "remote MCP service failed to start"
    fi
    systemctl is-active --quiet codebase-memory-mcp || die "remote MCP service is not active"
    if ! systemctl restart codebase-memory-mcp-ui; then
        journalctl -u codebase-memory-mcp-ui -n 30 --no-pager >&2 || true
        die "visual-console service failed to start"
    fi
    systemctl is-active --quiet codebase-memory-mcp-ui ||
        die "visual-console service is not active"

    nginx -t
    systemctl enable nginx >/dev/null
    systemctl reload nginx 2>/dev/null || systemctl restart nginx

    local backend_status ui_status entry_status entry_ui_status
    backend_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
        --max-time 10 "http://127.0.0.1:$BACKEND_PORT/mcp" || true)"
    [ "$backend_status" = "401" ] || die "backend smoke check returned HTTP $backend_status (expected 401)"
    ui_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
        --max-time 10 "http://127.0.0.1:$UI_BACKEND_PORT/" || true)"
    [ "$ui_status" = "200" ] || die "visual-console smoke check returned HTTP $ui_status (expected 200)"
    entry_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
        --max-time 10 "http://127.0.0.1:$PUBLIC_PORT/mcp" || true)"
    [ "$entry_status" = "401" ] ||
        die "HTTP entry smoke check returned HTTP $entry_status (expected 401)"
    entry_ui_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
        --max-time 10 "http://127.0.0.1:$PUBLIC_PORT/" || true)"
    [ "$entry_ui_status" = "401" ] ||
        die "visual-console authentication check returned HTTP $entry_ui_status (expected 401)"
    if [ -n "$INITIAL_UI_PASSWORD" ]; then
        entry_ui_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
            --user "$UI_USER:$INITIAL_UI_PASSWORD" --max-time 10 \
            "http://127.0.0.1:$PUBLIC_PORT/" || true)"
        [ "$entry_ui_status" = "200" ] ||
            die "authenticated visual-console check returned HTTP $entry_ui_status (expected 200)"
    fi
    remove_legacy_tls_material
    ok "Remote MCP, visual console, and HTTP entry are active"
}

print_summary() {
    printf '\nDeployment complete\n'
    printf '  Listen:        http://%s:%s\n' "$LISTEN_ADDRESS" "$PUBLIC_PORT"
    if $PUBLIC; then
        printf '  Console URL:   http://%s:%s/\n' "$ACCESS_HOST" "$PUBLIC_PORT"
        printf '  MCP URL:       http://%s:%s/mcp\n' "$ACCESS_HOST" "$PUBLIC_PORT"
    else
        printf '  Console upstream: http://127.0.0.1:%s/\n' "$PUBLIC_PORT"
        printf '  MCP upstream:     http://127.0.0.1:%s/mcp\n' "$PUBLIC_PORT"
    fi
    printf '  Backend:       http://127.0.0.1:%s/mcp (loopback only)\n' "$BACKEND_PORT"
    printf '  UI backend:    http://127.0.0.1:%s/ (loopback only)\n' "$UI_BACKEND_PORT"
    printf '  Allowed root:  %s\n' "$ALLOWED_ROOT"
    printf '  Key Store:     %s\n' "$AUTH_STORE"
    printf '  Audit log:     %s\n' "$AUDIT_LOG"
    printf '  Management:    sudo cbm-server --help\n'
    printf '  Change root:   sudo cbm-server set-allowed-root /path/to/projects\n'
    printf '  Git SSH key:   sudo cbm-server git-key\n'
    printf '  UI username:   %s\n' "$UI_USER"
    if [ -n "$INITIAL_UI_PASSWORD" ]; then
        printf '  UI password:   %s (shown once)\n' "$INITIAL_UI_PASSWORD"
    else
        printf '  UI password:   preserved; reset with sudo cbm-server ui-password-reset\n'
    fi
    if $PUBLIC; then
        printf '\nTemporary test client configuration:\n'
        cat <<EOF
[mcp_servers.codebase_memory_remote]
url = "http://$ACCESS_HOST:$PUBLIC_PORT/mcp"
bearer_token_env_var = "CBM_REMOTE_MCP_TOKEN"
startup_timeout_sec = 10
tool_timeout_sec = 180
EOF
        printf '\nFirewall policy was not changed. Allow TCP %s only from intended test clients.\n' \
            "$PUBLIC_PORT"
        warn "Plain HTTP public mode is for temporary testing only; credentials are not encrypted."
    else
        printf '\nConfigure clients with the HTTPS URL published by the company proxy, for example:\n'
        cat <<'EOF'
[mcp_servers.codebase_memory_remote]
url = "https://codebase-memory-mcp.company.example/mcp"
bearer_token_env_var = "CBM_REMOTE_MCP_TOKEN"
startup_timeout_sec = 10
tool_timeout_sec = 180
EOF
        printf '\nLoopback-only mode is active. Configure the company HTTPS proxy to forward to '\
        printf 'http://127.0.0.1:%s.\n' "$PUBLIC_PORT"
    fi
}

configure_listen_mode
preserve_existing_allowed_root
validate_inputs
install_packages
require_commands
create_private_temp_dir
ensure_account_and_directories
ensure_git_ssh_identity
install_binary
install_management_command
write_environment
write_systemd_units
write_nginx_config
write_logrotate_config
ensure_ui_credentials
nginx -t
ensure_admin_key
start_and_verify
print_summary
