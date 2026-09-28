#!/usr/bin/env bash
set -euo pipefail

STATE_DIR="${MOSH_WIN_SSHD_STATE_DIR:-/tmp/mosh-win-sshd}"
ACTION="${1:-}"
PORT="${2:-22222}"
AUTHORIZED_KEY_B64="${3:-}"
LOGIN_USER="${4:-}"

die() {
  printf 'sshd-fixture: %s\n' "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "missing $1; install openssh-server in this WSL distribution"
}

stop_fixture() {
  if [[ -f "$STATE_DIR/sshd.pid" ]]; then
    local pid
    pid="$(cat "$STATE_DIR/sshd.pid")"
    if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid"
      for _ in {1..50}; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
      done
    fi
  fi
  # Clean up a listener from an interrupted older fixture that failed before
  # recording its PID.  The pattern is scoped to this fixture's config path.
  pkill -f "sshd.*${STATE_DIR}/sshd_config" 2>/dev/null || true
  rm -rf "$STATE_DIR"
}

start_fixture() {
  require_command sshd
  require_command ssh-keygen
  require_command setsid
  (( EUID == 0 )) || die "run this fixture as root"
  [[ "$PORT" =~ ^[0-9]+$ ]] || die "port must be numeric"
  (( PORT >= 1024 && PORT <= 65535 )) || die "port must be between 1024 and 65535"
  [[ -n "$AUTHORIZED_KEY_B64" ]] || die "an authorized public key is required"
  [[ -n "$LOGIN_USER" ]] || die "a non-root login user is required"
  id "$LOGIN_USER" >/dev/null 2>&1 || die "unknown login user: $LOGIN_USER"

  stop_fixture
  install -d -m 0755 "$STATE_DIR"
  printf '%s' "$AUTHORIZED_KEY_B64" | base64 -d >"$STATE_DIR/authorized_keys"
  chmod 0644 "$STATE_DIR/authorized_keys"
  ssh-keygen -q -t ed25519 -N '' -f "$STATE_DIR/ssh_host_ed25519_key"

  cat >"$STATE_DIR/sshd_config" <<EOF
Port $PORT
ListenAddress 0.0.0.0
AddressFamily inet
HostKey $STATE_DIR/ssh_host_ed25519_key
PidFile $STATE_DIR/sshd.pid
AuthorizedKeysFile $STATE_DIR/authorized_keys
PasswordAuthentication no
KbdInteractiveAuthentication no
ChallengeResponseAuthentication no
PubkeyAuthentication yes
PermitRootLogin no
UsePAM no
SetEnv LANG=C.UTF-8 LC_ALL=C.UTF-8
StrictModes no
AllowUsers $LOGIN_USER
PrintMotd no
PrintLastLog no
LogLevel VERBOSE
EOF
  chmod 0600 "$STATE_DIR/sshd_config"
  install -d -m 0755 /run/sshd
  /usr/sbin/sshd -t -f "$STATE_DIR/sshd_config"
  setsid /usr/sbin/sshd -D -f "$STATE_DIR/sshd_config" \
    -E "$STATE_DIR/sshd.log" </dev/null >/dev/null 2>&1 &

  local pid
  pid="$!"
  printf '%s\n' "$pid" >"$STATE_DIR/sshd.pid"
  sleep 0.1
  kill -0 "$pid" 2>/dev/null || die "sshd did not remain running; inspect $STATE_DIR/sshd.log"
  printf 'MOSH_TEST_SSHD_PID=%s\n' "$pid"
  printf 'MOSH_TEST_SSHD_PORT=%s\n' "$PORT"
  printf 'MOSH_TEST_USER=%s\n' "$LOGIN_USER"
  hostname -I | awk '{print "MOSH_TEST_HOST=" $1}'
}

case "$ACTION" in
  start)
    start_fixture
    ;;
  stop)
    stop_fixture
    ;;
  status)
    [[ -f "$STATE_DIR/sshd.pid" ]] || exit 1
    pid="$(cat "$STATE_DIR/sshd.pid")"
    kill -0 "$pid"
    ;;
  *)
    die "usage: $0 {start PORT AUTHORIZED_KEY_BASE64 LOGIN_USER|stop|status}"
    ;;
esac
