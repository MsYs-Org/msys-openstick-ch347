#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SESSION_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
STATE_ENTRY="$SESSION_ROOT/scripts/msys_display_session_state.py"
if [ -n "${MSYS_X11DISPLAY_ROOT:-}" ]; then
    X11DISPLAY_ROOT="$MSYS_X11DISPLAY_ROOT"
elif [ -n "${MSYS_PACKAGE_ROOT:-}" ] && [ -d "$MSYS_PACKAGE_ROOT/files/x11display" ]; then
    X11DISPLAY_ROOT="$MSYS_PACKAGE_ROOT/files/x11display"
elif [ -d "$SESSION_ROOT/files/x11display" ]; then
    X11DISPLAY_ROOT="$SESSION_ROOT/files/x11display"
else
    # Development compatibility only. A production OpenStick driver package
    # carries this tree under files/x11display and never reaches this fallback.
    X11DISPLAY_ROOT=/root/x11display
fi
START_SCRIPT="${CH347_START_SCRIPT:-$X11DISPLAY_ROOT/scripts/start_ch347_dirty_usb_x11.sh}"
STOP_SCRIPT="${CH347_STOP_SCRIPT:-$X11DISPLAY_ROOT/scripts/stop_ch347_dirty_usb_x11.sh}"
DISPLAY_CONFIG_HELPER="$X11DISPLAY_ROOT/scripts/ch347_display_config.sh"
RUN_DIR="${RUN_DIR:-/tmp/ch347_dirty_usb_x11}"
PID_FILE="$RUN_DIR/pids"
LOG_FILE="$RUN_DIR/live.log"
APPLIED_CONFIG_FILE="${MSYS_CH347_APPLIED_CONFIG_FILE:-$RUN_DIR/display-config.applied.env}"
READY_FILE="${MSYS_X11_READY_FILE:-$RUN_DIR/msys.ready}"
if [ -n "${MSYS_DISPLAY_SESSION_STATE_FILE:-}" ]; then
    SESSION_STATE_FILE="$MSYS_DISPLAY_SESSION_STATE_FILE"
elif [ -n "${MSYS_RUNTIME_DIR:-}" ]; then
    SESSION_STATE_FILE="$MSYS_RUNTIME_DIR/display-session.json"
else
    SESSION_STATE_FILE="$READY_FILE"
fi
SESSION_OWNER_FILE="$SESSION_STATE_FILE.owner"
OWNER_FILE="$RUN_DIR/msys.provider.owner"
OWNER_TOKEN="${MSYS_GENERATION:-0}:$$:$(date +%s)"
# The detached x11display daemon outlives the short start script. Give it the
# same generation lease so a superseded daemon cannot remove a replacement's
# shared pid file or rebind its USB interface during a delayed EXIT trap.
export CH347_STACK_OWNER_FILE="$OWNER_FILE"
export CH347_STACK_OWNER_TOKEN="$OWNER_TOKEN"
TOUCH_MODE_FILE="${CH347_TOUCH_MODE_FILE:-$RUN_DIR/touch_mode}"
EFFECTIVE_INPUT_MODE=ch347-direct
CLEANED=0
DISPLAY_READY=0
PUBLISHED_DISPLAY_SIGNATURE=""
PUBLISHED_INPUT_MODE=""
MONITOR_INTERVAL="${MSYS_CH347_MONITOR_INTERVAL:-0.5}"
DISPLAY_FAIL_LIMIT="${MSYS_CH347_DISPLAY_FAIL_LIMIT:-3}"
MISSING_PID_LIMIT="${MSYS_CH347_MISSING_PID_LIMIT:-3}"
LOG_MAX_BYTES="${MSYS_CH347_LOG_MAX_BYTES:-1048576}"
LOG_CHECK_TICKS="${MSYS_CH347_LOG_CHECK_TICKS:-10}"

[ -f "$DISPLAY_CONFIG_HELPER" ] || {
    echo "msys-ch347-provider: display config helper missing: $DISPLAY_CONFIG_HELPER" >&2
    exit 1
}
# shellcheck disable=SC1090
. "$DISPLAY_CONFIG_HELPER"

case "$MONITOR_INTERVAL" in
    ''|*[!0-9.]*|*.*.*) MONITOR_INTERVAL=0.5 ;;
esac
case "$DISPLAY_FAIL_LIMIT" in
    ''|*[!0-9]*) DISPLAY_FAIL_LIMIT=3 ;;
esac
case "$MISSING_PID_LIMIT" in
    ''|*[!0-9]*) MISSING_PID_LIMIT=3 ;;
esac
case "$LOG_MAX_BYTES" in
    ''|*[!0-9]*|?????????*) LOG_MAX_BYTES=1048576 ;;
esac
case "$LOG_CHECK_TICKS" in
    ''|*[!0-9]*|????*) LOG_CHECK_TICKS=10 ;;
esac
LOG_MAX_BYTES=$((10#$LOG_MAX_BYTES))
LOG_CHECK_TICKS=$((10#$LOG_CHECK_TICKS))
[ "$LOG_MAX_BYTES" -ge 1024 ] && [ "$LOG_MAX_BYTES" -le 16777216 ] || LOG_MAX_BYTES=1048576
[ "$LOG_CHECK_TICKS" -ge 1 ] && [ "$LOG_CHECK_TICKS" -le 120 ] || LOG_CHECK_TICKS=10
[ "$DISPLAY_FAIL_LIMIT" -ge 2 ] || DISPLAY_FAIL_LIMIT=2
[ "$MISSING_PID_LIMIT" -ge 2 ] || MISSING_PID_LIMIT=2

atomic_write_line()
{
    local target="$1"
    local value="$2"
    local directory
    local tmp

    if { [ -e "$target" ] || [ -L "$target" ]; } &&
            { [ -L "$target" ] || [ ! -f "$target" ]; }; then
        echo "msys-ch347-provider: unsafe state target: $target" >&2
        return 1
    fi
    directory="$(dirname -- "$target")"
    mkdir -p "$directory"
    [ -d "$directory" ] && [ ! -L "$directory" ] || return 1
    tmp=$(mktemp "$directory/.msys-ch347-state.XXXXXX") || return 1
    (
        trap 'rm -f "$tmp"' EXIT HUP INT TERM
        printf '%s\n' "$value" > "$tmp"
        chmod 600 "$tmp"
        mv -f "$tmp" "$target"
        trap - EXIT HUP INT TERM
    )
}

atomic_seed_file()
{
    local source="$1"
    local target="$2"
    local directory
    local tmp

    [ -f "$source" ] && [ ! -L "$source" ] || return 1
    if [ -e "$target" ] || [ -L "$target" ]; then
        [ -f "$target" ] && [ ! -L "$target" ]
        return
    fi
    directory="$(dirname -- "$target")"
    mkdir -p "$directory"
    [ -d "$directory" ] && [ ! -L "$directory" ] || return 1
    tmp=$(mktemp "$directory/.msys-ch347-seed.XXXXXX") || return 1
    (
        trap 'rm -f "$tmp"' EXIT HUP INT TERM
        cp -- "$source" "$tmp"
        chmod 600 "$tmp"
        mv -f "$tmp" "$target"
        trap - EXIT HUP INT TERM
    )
}

prepare_mutable_config()
{
    local state_root="${MSYS_APP_STATE_DIR:-}"
    local source
    local target
    [ -n "$state_root" ] || return 0
    mkdir -p "$state_root/ch347"
    if [ -z "${CH347_FPS_FILE:-}" ]; then
        source="$X11DISPLAY_ROOT/ch347/fps.env"
        target="$state_root/ch347/fps.env"
        atomic_seed_file "$source" "$target" || return 1
        export CH347_FPS_FILE="$target"
    fi
    if [ -z "${CH347_TOUCH_CAL_FILE:-}" ]; then
        source="$X11DISPLAY_ROOT/ch347/touch_calibration.env"
        target="$state_root/ch347/touch_calibration.env"
        atomic_seed_file "$source" "$target" || return 1
        export CH347_TOUCH_CAL_FILE="$target"
    fi
    if [ -z "${CH347_ROTATION_FILE:-}" ]; then
        source="$X11DISPLAY_ROOT/ch347/rotation.env"
        target="$state_root/ch347/rotation.env"
        atomic_seed_file "$source" "$target" || return 1
        export CH347_ROTATION_FILE="$target"
    fi
}

load_display_rotation()
{
    local config="${CH347_ROTATION_FILE:-$X11DISPLAY_ROOT/ch347/rotation.env}"
    local line
    local rotation=""

    [ -f "$config" ] || {
        export CH347_DISPLAY_ROTATION=normal
        return 0
    }
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            ''|'#'*) ;;
            CH347_DISPLAY_ROTATION=normal|CH347_DISPLAY_ROTATION=right|\
            CH347_DISPLAY_ROTATION=inverted|CH347_DISPLAY_ROTATION=left)
                [ -z "$rotation" ] || {
                    echo "msys-ch347-provider: duplicate display rotation" >&2
                    return 1
                }
                rotation="${line#CH347_DISPLAY_ROTATION=}"
                ;;
            *)
                echo "msys-ch347-provider: invalid display rotation config" >&2
                return 1
                ;;
        esac
    done < "$config"
    [ -n "$rotation" ] || {
        echo "msys-ch347-provider: display rotation is missing" >&2
        return 1
    }
    export CH347_DISPLAY_ROTATION="$rotation"
    echo "msys-ch347-provider: display rotation=$rotation"
}

migrate_legacy_idle_capture_default()
{
    local fps_file="${CH347_FPS_FILE:-}"

    # Early packages persisted the factory 60/60/1 profile.  Its one-frame
    # per-second heartbeat is now known to be harmful on SPI panels, so move
    # only that exact legacy default to the event-driven 60/60/0 profile.
    # A deliberately retained heartbeat can opt out explicitly.
    [ "${MSYS_CH347_KEEP_IDLE_FPS:-0}" = "1" ] && return 0
    [ -n "$fps_file" ] && [ -f "$fps_file" ] || return 0
    ch347_read_display_config "$fps_file" || return 1
    [ "$CH347_CONFIG_FPS" = 60 ] || return 0
    [ "$CH347_CONFIG_MAX_FPS" = 60 ] || return 0
    [ "$CH347_CONFIG_IDLE_FPS" = 1 ] || return 0
    ch347_write_display_config "$fps_file" "$CH347_CONFIG_DEBUG" 60 60 0
    echo "msys-ch347-provider: migrated legacy idle capture heartbeat to 0 fps"
}

migrate_legacy_debug_default()
{
    local fps_file="${CH347_FPS_FILE:-}"

    [ -n "$fps_file" ] && [ -f "$fps_file" ] || return 0
    ch347_read_display_config "$fps_file" || return 1
    grep -q '^DEBUG=' "$fps_file" && return 0
    ch347_write_display_config "$fps_file" 0 "$CH347_CONFIG_FPS" \
        "$CH347_CONFIG_MAX_FPS" "$CH347_CONFIG_IDLE_FPS"
    echo "msys-ch347-provider: provisioned persistent DEBUG=0"
}

load_display_config()
{
    local config="${CH347_FPS_FILE:-$X11DISPLAY_ROOT/ch347/fps.env}"

    ch347_read_display_config "$config" || {
        echo "msys-ch347-provider: invalid display config: $config" >&2
        return 1
    }
    # The package manifest carries safe factory defaults.  The package-owned
    # mutable document is authoritative on every managed start, so persisted
    # Settings changes survive both provider and device reboots.
    export DEBUG="$CH347_CONFIG_DEBUG"
    export FPS="$CH347_CONFIG_FPS"
    export XCAP_MAX_FPS="$CH347_CONFIG_MAX_FPS"
    export XCAP_IDLE_FPS="$CH347_CONFIG_IDLE_FPS"
    echo "msys-ch347-provider: display config debug=$DEBUG fps=$FPS max_fps=$XCAP_MAX_FPS idle_fps=$XCAP_IDLE_FPS"
}

publish_applied_display_config()
{
    local generation="${MSYS_GENERATION:-0}"

    ch347_write_display_config "$APPLIED_CONFIG_FILE" "$DEBUG" "$FPS" \
        "$XCAP_MAX_FPS" "$XCAP_IDLE_FPS" "$generation"
}

bound_live_log()
{
    local size

    owns_stack || return 0
    [ -f "$LOG_FILE" ] && [ ! -L "$LOG_FILE" ] || return 0
    size=$(stat -c %s -- "$LOG_FILE" 2>/dev/null) || return 0
    case "$size" in
        ''|*[!0-9]*) return 0 ;;
    esac
    [ "$size" -gt "$LOG_MAX_BYTES" ] || return 0
    # Truncate the existing inode. The daemon and sink hold O_APPEND file
    # descriptors to this inode; rename-based rotation would strand their
    # future DEBUG samples in an unlinked file and keep consuming disk.
    [ -f "$LOG_FILE" ] && [ ! -L "$LOG_FILE" ] || return 0
    : > "$LOG_FILE"
    echo "msys-ch347-provider: bounded live.log at ${size} bytes"
}

load_touch_calibration()
{
    local calibration="${CH347_TOUCH_CAL_FILE:-$X11DISPLAY_ROOT/ch347/touch_calibration.env}"
    if [ "${CH347_TOUCH:-0}" = "1" ] &&
            [ "${CH347_TOUCH_CALIBRATE:-0}" != "1" ] &&
            [ -f "$calibration" ] && [ -z "${CH347_TOUCH_X_MIN+x}" ]; then
        # Source once in the provider so the sink and the display-session
        # publisher receive the exact same swap/invert/calibration values.
        set -a
        # shellcheck disable=SC1090
        . "$calibration"
        set +a
        echo "msys-ch347-provider: loaded touch calibration=$calibration"
    fi
}

find_session_python()
{
    if [ -n "${MSYS_PYTHON:-}" ] && [ -x "$MSYS_PYTHON" ]; then
        printf '%s\n' "$MSYS_PYTHON"
    elif [ -x /opt/msys-dev/.runtime/python/bin/python3 ]; then
        printf '%s\n' /opt/msys-dev/.runtime/python/bin/python3
    elif command -v python3 >/dev/null 2>&1; then
        command -v python3
    elif command -v python >/dev/null 2>&1; then
        command -v python
    else
        return 1
    fi
}

publish_state_file()
{
    local state_file="$1"
    local python
    python=$(find_session_python) || {
        echo "msys-ch347-provider: Python runtime unavailable for display-session state" >&2
        return 1
    }
    [ -f "$STATE_ENTRY" ] || {
        echo "msys-ch347-provider: display-session publisher missing: $STATE_ENTRY" >&2
        return 1
    }
    MSYS_DISPLAY_INPUT_MODE="$EFFECTIVE_INPUT_MODE" \
        "$python" "$STATE_ENTRY" \
        --display "${DISPLAY_ID:-${DISPLAY:-:24}}" \
        --provider "${MSYS_COMPONENT_ID:-org.msys.openstick.ch347:x11-spi-touch-output}" \
        --state-file "$state_file"
}

claim_session_state()
{
    atomic_write_line "$SESSION_OWNER_FILE" "$OWNER_TOKEN" || return 1
}

owns_session_state()
{
    [ -f "$SESSION_OWNER_FILE" ] && [ "$(cat "$SESSION_OWNER_FILE" 2>/dev/null || true)" = "$OWNER_TOKEN" ]
}

publish_display_state()
{
    claim_session_state
    publish_state_file "$SESSION_STATE_FILE"
    if [ "$READY_FILE" != "$SESSION_STATE_FILE" ]; then
        publish_state_file "$READY_FILE"
    fi
}

display_signature()
{
    # State-file replacement is observed as a layout event.  Probe X11 for
    # liveness, but publish only when layout-relevant geometry or effective
    # input routing changes rather than using the state document as a timer.
    DISPLAY="${DISPLAY_ID:-${DISPLAY:-:24}}" xdpyinfo 2>/dev/null |
        awk '
            /^[[:space:]]*dimensions:[[:space:]]*[0-9]+x[0-9]+[[:space:]]+pixels/ { dimensions = $2 }
            /^[[:space:]]*depth of root window:[[:space:]]*[0-9]+[[:space:]]+planes/ { depth = $5 }
            END {
                if (dimensions != "" && depth != "") {
                    print dimensions "/" depth
                    exit 0
                }
                exit 1
            }
        '
}

remember_published_display_state()
{
    PUBLISHED_DISPLAY_SIGNATURE="$1"
    PUBLISHED_INPUT_MODE="$EFFECTIVE_INPUT_MODE"
}

native_touch_available()
{
    local names
    command -v xinput >/dev/null 2>&1 || return 1
    names=$(DISPLAY="${DISPLAY_ID:-${DISPLAY:-:24}}" xinput list --name-only 2>/dev/null || true)
    case "$names" in
        *"${MSYS_CH347_TOUCH_DEVICE:-CH347 XPT2046 Touchscreen}"*) return 0 ;;
        *) return 1 ;;
    esac
}

xtest_available()
{
    local extensions
    extensions=$(DISPLAY="${DISPLAY_ID:-${DISPLAY:-:24}}" xdpyinfo -queryExtensions 2>/dev/null || true)
    case "$extensions" in
        *XTEST*) return 0 ;;
        *) return 1 ;;
    esac
}

set_touch_mouse_mode()
{
    atomic_write_line "$TOUCH_MODE_FILE" mouse
}

observe_touch_mode()
{
    local mode=touch
    if [ "${CH347_TOUCH:-0}" != "1" ]; then
        EFFECTIVE_INPUT_MODE=none
        return
    fi
    if [ -f "$TOUCH_MODE_FILE" ]; then
        read -r mode < "$TOUCH_MODE_FILE" || mode=touch
    elif [ -n "${CH347_TOUCH_MODE:-}" ]; then
        mode="$CH347_TOUCH_MODE"
    fi
    mode="${mode,,}"
    if [ "$mode" = "mouse" ]; then
        EFFECTIVE_INPUT_MODE=ch347-xtest
    else
        EFFECTIVE_INPUT_MODE=ch347-direct
    fi
}

configure_touch_fallback()
{
    local probes="${MSYS_CH347_NATIVE_TOUCH_PROBES:-20}"
    local count=0

    observe_touch_mode
    if [ "$EFFECTIVE_INPUT_MODE" != "ch347-direct" ] ||
            [ "${MSYS_CH347_XTEST_FALLBACK:-0}" != "1" ]; then
        return 0
    fi
    case "$probes" in
        ''|*[!0-9]*) probes=20 ;;
    esac
    while [ "$count" -lt "$probes" ]; do
        if native_touch_available; then
            echo "msys-ch347-provider: native XInput touch detected"
            return 0
        fi
        count=$((count + 1))
        sleep 0.1
    done
    if ! xtest_available; then
        echo "msys-ch347-provider: native touch missing and XTEST extension unavailable" >&2
        return 1
    fi
    set_touch_mouse_mode
    EFFECTIVE_INPUT_MODE=ch347-xtest
    echo "msys-ch347-provider: native touch missing; enabled optional XTest fallback"
}

claim_ownership()
{
    atomic_write_line "$OWNER_FILE" "$OWNER_TOKEN"
}

owns_stack()
{
    [ -f "$OWNER_FILE" ] && [ "$(cat "$OWNER_FILE" 2>/dev/null || true)" = "$OWNER_TOKEN" ]
}

superseded()
{
    if owns_stack; then
        return 1
    fi
    echo "msys-ch347-provider: ownership moved; provider superseded"
    return 0
}

stop_stack()
{
    (
        set +e
        if [ -x "$STOP_SCRIPT" ]; then
            "$STOP_SCRIPT"
        elif [ -f "$STOP_SCRIPT" ]; then
            bash "$STOP_SCRIPT"
        fi
    )
}

cleanup()
{
    if [ "$CLEANED" = "1" ]; then
        return
    fi
    CLEANED=1

    # A previous msysd generation may finish after its replacement has
    # already started.  Only the generation that currently owns the stack is
    # allowed to run the global stop script; otherwise the old EXIT trap can
    # tear down the replacement's X server.
    if owns_stack; then
        stop_stack
        if owns_session_state; then
            rm -f "$SESSION_STATE_FILE" "$SESSION_OWNER_FILE"
        fi
        rm -f "$READY_FILE" "$OWNER_FILE" "$APPLIED_CONFIG_FILE"
    else
        echo "msys-ch347-provider: ownership moved; leaving replacement stack running"
    fi
}

handle_term()
{
    cleanup
    exit 0
}

trap handle_term INT TERM
trap cleanup EXIT

# Claim before inspecting/stopping an existing stack.  This also prevents an
# old provider's delayed EXIT trap from stopping the stack we are about to
# replace.
claim_ownership
# READY_FILE is shared by all generations.  A process must own the stack
# before removing its predecessor's readiness edge, and must never use this
# globally replaceable file as proof that its own publish call succeeded.
rm -f "$READY_FILE" "$APPLIED_CONFIG_FILE"

if [ -f "$PID_FILE" ]; then
    echo "msys-ch347-provider: stale or existing pid file found; stopping previous stack"
    stop_stack
    sleep 1
fi

prepare_mutable_config
migrate_legacy_idle_capture_default
migrate_legacy_debug_default
load_display_config
load_display_rotation
load_touch_calibration

if [ -x "$START_SCRIPT" ]; then
    if ! "$START_SCRIPT"; then
        if superseded; then exit 0; fi
        echo "msys-ch347-provider: start script failed" >&2
        exit 1
    fi
elif ! bash "$START_SCRIPT"; then
    if superseded; then exit 0; fi
    echo "msys-ch347-provider: start script failed" >&2
    exit 1
fi

echo "msys-ch347-provider: started"
echo "msys-ch347-provider: pid-file=$PID_FILE"
echo "msys-ch347-provider: log=$LOG_FILE"

for _ in $(seq 1 80); do
    if signature=$(display_signature); then
        if ! configure_touch_fallback; then
            if superseded; then exit 0; fi
            echo "msys-ch347-provider: touch setup failed" >&2
            exit 1
        fi
        # Publish the generation-bound tuning receipt before exposing the X11
        # ready edge. Consumers that observe READY may immediately query HAL;
        # they must never see a new display generation with a missing/old
        # applied DEBUG receipt.
        if ! publish_applied_display_config; then
            if superseded; then exit 0; fi
            echo "msys-ch347-provider: applied display config publish failed" >&2
            exit 1
        fi
        if superseded; then exit 0; fi
        if ! publish_display_state; then
            if superseded; then exit 0; fi
            echo "msys-ch347-provider: display-session publish failed" >&2
            exit 1
        fi
        remember_published_display_state "$signature"
        DISPLAY_READY=1
        if superseded; then exit 0; fi
        echo "msys-ch347-provider: ready-file=$READY_FILE"
        break
    fi
    sleep 0.1
done

if [ "$DISPLAY_READY" != "1" ]; then
    if superseded; then exit 0; fi
    echo "msys-ch347-provider: display did not become ready"
    exit 1
fi

missing_pid_checks=0
display_fail_checks=0
log_check_count=0
while :; do
    # Replacement claims the stack owner before it stops/restarts the shared
    # X11 pipeline.  An older generation must leave promptly and successfully:
    # otherwise it can report a false failure and, more importantly, its
    # periodic refresh could reclaim display-session.json with stale
    # generation metadata after the replacement has become ready.
    if superseded; then exit 0; fi

    log_check_count=$((log_check_count + 1))
    if [ "$log_check_count" -ge "$LOG_CHECK_TICKS" ]; then
        log_check_count=0
        bound_live_log
    fi

    if [ ! -f "$PID_FILE" ]; then
        missing_pid_checks=$((missing_pid_checks + 1))
        if [ "$missing_pid_checks" -ge "$MISSING_PID_LIMIT" ]; then
            if superseded; then exit 0; fi
            echo "msys-ch347-provider: pid file missing; provider stopped"
            exit 1
        fi
    else
        missing_pid_checks=0
    fi

    alive=0
    if [ -f "$PID_FILE" ]; then
        while read -r pid; do
            if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
                alive=1
                break
            fi
        done < "$PID_FILE"
    fi

    if [ "$missing_pid_checks" = "0" ] && [ "$alive" = "0" ]; then
        if superseded; then exit 0; fi
        echo "msys-ch347-provider: no live child from pid file"
        tail -80 "$LOG_FILE" 2>/dev/null || true
        exit 1
    fi

    if signature=$(display_signature); then
        display_fail_checks=0
        observe_touch_mode
        if [ "$signature" != "$PUBLISHED_DISPLAY_SIGNATURE" ] ||
                [ "$EFFECTIVE_INPUT_MODE" != "$PUBLISHED_INPUT_MODE" ]; then
            if superseded; then exit 0; fi
            if ! publish_display_state >/dev/null; then
                if superseded; then exit 0; fi
                echo "msys-ch347-provider: display-session state update failed" >&2
                exit 1
            fi
            if superseded; then exit 0; fi
            remember_published_display_state "$signature"
        fi
    else
        display_fail_checks=$((display_fail_checks + 1))
        if [ "$display_fail_checks" -ge "$DISPLAY_FAIL_LIMIT" ]; then
            if superseded; then exit 0; fi
            echo "msys-ch347-provider: X11 display unavailable"
            exit 1
        fi
    fi

    # X11 clients lose their connection immediately when the detached server
    # dies.  Notice the shared failure domain before those clients can consume
    # five independent Core restart attempts and become quarantined.  A small
    # consecutive-failure threshold still filters a one-off xdpyinfo failure.
    sleep "$MONITOR_INTERVAL"
done
