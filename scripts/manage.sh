#!/bin/sh
set -eu
umask 077

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
BIN="$ROOT/build/mishmeret"
DATA=${MISHMERET_DATA_DIR:-"$ROOT/data"}
PORT=${MISHMERET_PORT:-8080}
ORIGIN=${MISHMERET_ORIGIN:-"http://127.0.0.1:$PORT"}

fail() { printf '%s\n' "$*" >&2; exit 1; }
usage() {
    printf '%s\n' 'Usage: scripts/manage.sh init | start | backup | restore BACKUP'
}

[ "$#" -gt 0 ] || { usage; exit 1; }
ACTION=$1
shift
case "$ACTION" in
    init|start|backup) [ "$#" -eq 0 ] || { usage; exit 1; } ;;
    restore) [ "$#" -eq 1 ] || { usage; exit 1; } ;;
    *) usage; exit 1 ;;
esac
[ -x "$BIN" ] || fail 'Build first: make'
mkdir -p -- "$DATA"
DATA=$(CDPATH= cd -- "$DATA" && pwd -P)
chmod 700 "$DATA"
DB="$DATA/mishmeret.db"
ACCOUNTS="$DATA/initial-accounts.json"

initialize() {
    [ ! -e "$DB" ] || fail "Database already exists: $DB"
    [ ! -e "$ACCOUNTS" ] || fail "Initial account file already exists: $ACCOUNTS"
    account_tmp=$(mktemp "$DATA/.initial-accounts.XXXXXX")
    if "$BIN" --init --db "$DB" > "$account_tmp"; then
        if ln "$account_tmp" "$ACCOUNTS"; then
            rm -- "$account_tmp"
            printf 'Initial account passwords: %s\n' "$ACCOUNTS" >&2
            printf '%s\n' 'Each account must change its password at first login.' >&2
        else
            fail "Account file was preserved at: $account_tmp"
        fi
    else
        rm -- "$account_tmp"
        fail 'Initialization failed; no existing account file was overwritten.'
    fi
}

case "$ACTION" in
    init) initialize ;;
    start)
        [ -e "$DB" ] || initialize
        printf 'Open %s (Ctrl-C to stop).\n' "$ORIGIN" >&2
        set --
        case "${MISHMERET_TRUST_TAILSCALE_PROXY:-0}" in
            0) ;;
            1) set -- --trust-tailscale-proxy ;;
            *) fail "MISHMERET_TRUST_TAILSCALE_PROXY must be 0 or 1" ;;
        esac
        exec "$BIN" "$@" --db "$DB" --web "$ROOT/web" --port "$PORT" --origin "$ORIGIN"
        ;;
    backup)
        [ -f "$DB" ] || fail "Database not found: $DB"
        mkdir -p -- "$DATA/backups"
        chmod 700 "$DATA/backups"
        snapshot="$DATA/backups/mishmeret-$(date -u +%Y%m%dT%H%M%SZ)-$$.db"
        "$BIN" --backup "$snapshot" --db "$DB"
        printf '%s\n' "$snapshot"
        ;;
    restore)
        [ -f "$DB" ] || fail "Destination database not found: $DB"
        [ -f "$1" ] || fail "Backup not found: $1"
        "$BIN" --restore "$1" --db "$DB"
        printf '%s\n' 'Restored. Start the application; users must sign in again.' >&2
        ;;
esac
