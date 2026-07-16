#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-$(cd "$SCRIPT_DIR/.." && pwd)}"
BIN_DIR="${BIN_DIR:-$PROJECT_DIR/bin}"
CH347_DIR="${CH347_DIR:-$PROJECT_DIR/ch347}"
DISPLAY_CONFIG_HELPER="${CH347_DISPLAY_CONFIG_HELPER:-$SCRIPT_DIR/ch347_display_config.sh}"

FPS="${FPS:-30}"
CAPTURE="${CAPTURE:-xdamage}"
XCAP_MAX_FPS="${XCAP_MAX_FPS:-$FPS}"
XCAP_DEBUG="${XCAP_DEBUG:-0}"
XCAP_OUTPUT="${XCAP_OUTPUT:-frame}"
XCAP_FPS_FILE="${XCAP_FPS_FILE:-$PROJECT_DIR/ch347/fps.env}"
CH347_FPS_FILE="${CH347_FPS_FILE:-$XCAP_FPS_FILE}"
CH347_DEBUG_OVERLAY_FILE="${CH347_DEBUG_OVERLAY_FILE:-$PROJECT_DIR/ch347/debug_overlay.env}"
CH347_CURSOR_FILE="${CH347_CURSOR_FILE:-$PROJECT_DIR/ch347/cursor.env}"
CH347_ROTATION_FILE="${CH347_ROTATION_FILE:-$PROJECT_DIR/ch347/rotation.env}"
XCAP_NICE="${XCAP_NICE:--5}"
CH347_SINK_NICE="${CH347_SINK_NICE:--5}"
XVFB_NICE="${XVFB_NICE:-5}"
XSERVER="${XSERVER:-Xorg}"
XORG_CONFIG="${XORG_CONFIG:-$PROJECT_DIR/xorg/xorg.conf}"
APP_NICE="${APP_NICE:-12}"
MAX_FRAMES="${MAX_FRAMES:-0}"
DEPTH="${DEPTH:-8}"
APP="${APP:-glxgears}"
WM="${WM:-none}"
DISPLAY_ID="${DISPLAY_ID:-:24}"
WIDTH="${WIDTH:-320}"
HEIGHT="${HEIGHT:-480}"
DISPLAY_ROTATION="${XCAP_ROTATION:-${CH347_DISPLAY_ROTATION:-normal}}"
PIXFMT="${PIXFMT:-rgb565be}"
DEBUG="${DEBUG:-0}"
CH347_DEBUG_OVERLAY="${CH347_DEBUG_OVERLAY:-0}"
CH347_DEBUG_OVERLAY_ALPHA="${CH347_DEBUG_OVERLAY_ALPHA:-176}"
CH347_DEBUG_OVERLAY_SCALE="${CH347_DEBUG_OVERLAY_SCALE:-1}"
CH347_DEBUG_OVERLAY_ITEMS="${CH347_DEBUG_OVERLAY_ITEMS:-fps,dirty,bytes,cpu}"
CH347_DEBUG_OVERLAY_INTERVAL_MS="${CH347_DEBUG_OVERLAY_INTERVAL_MS:-1000}"
GATED="${GATED:-0}"
RENDER_MS="${RENDER_MS:-30}"
GLXGEARS_RENDER_MS="${GLXGEARS_RENDER_MS:-8}"
PACKET_US="${PACKET_US:-0}"
CH347_MODE="${CH347_MODE:-0}"
CH347_CLOCK="${CH347_CLOCK:-1}"
CH347_ARM="${CH347_ARM:-$BIN_DIR/ch347_st7796_test}"
CH347_SINK="${CH347_SINK:-$BIN_DIR/ch347_dirty_usb_sink}"
XCAP_BIN="${XCAP_BIN:-$BIN_DIR/xdamage_shm_capture}"
CH347_FULL_AREA_PCT="${CH347_FULL_AREA_PCT:-40}"
CH347_MAX_RECTS="${CH347_MAX_RECTS:-1}"
CH347_STALE_MS="${CH347_STALE_MS:-0}"
CH347_STALE_BUDGET="${CH347_STALE_BUDGET:-60}"
CH347_HOLD_CS="${CH347_HOLD_CS:-1}"
if [ "$CH347_MAX_RECTS" = "1" ] && [ "$CH347_STALE_MS" = "0" ]; then
    CH347_FULL_AREA_POLICY="inactive-single-bbox"
else
    CH347_FULL_AREA_POLICY="active"
fi
CH347_LATEST_ONLY="${CH347_LATEST_ONLY:-1}"
CH347_URB_TIMEOUT_MS="${CH347_URB_TIMEOUT_MS:-1500}"
CH347_USB_DEBUG="${CH347_USB_DEBUG:-0}"
CH347_GPIO_OVERLAY="${CH347_GPIO_OVERLAY:-0}"
CH347_GPIO_OVERLAY_MS="${CH347_GPIO_OVERLAY_MS:-200}"
CH347_RESTART_ON_FAIL="${CH347_RESTART_ON_FAIL:-1}"
CH347_RESTART_DELAY_SEC="${CH347_RESTART_DELAY_SEC:-2}"
# A loose USB/serial connection is an output-transport fault, not an X11
# session fault.  Zero means retry for as long as the existing X session is
# healthy.  A positive value remains available for bounded bench tests.
CH347_RESTART_MAX="${CH347_RESTART_MAX:-0}"
CH347_FORCE_RECONNECT="${CH347_FORCE_RECONNECT:-1}"
X_SESSION_FATAL_RC="${CH347_X_SESSION_FATAL_RC:-70}"
X_SESSION_PROBES="${CH347_X_SESSION_PROBES:-3}"
CH347_TOUCH="${CH347_TOUCH:-0}"
CH347_TOUCH_USE_IRQ="${CH347_TOUCH_USE_IRQ:-0}"
CH347_TOUCH_MODE="${CH347_TOUCH_MODE:-touch}"
CH347_CURSOR="${CH347_CURSOR:-0}"
CH347_TOUCH_CALIBRATE="${CH347_TOUCH_CALIBRATE:-0}"
CH347_TOUCH_CAL_FILE="${CH347_TOUCH_CAL_FILE:-$PROJECT_DIR/ch347/touch_calibration.env}"
CH347_TOUCH_CAL_EXIT="${CH347_TOUCH_CAL_EXIT:-1}"
CH347_TOUCH_CAL_MARGIN="${CH347_TOUCH_CAL_MARGIN:-32}"
CH347_TOUCH_POLL_MS="${CH347_TOUCH_POLL_MS:-32}"
CH347_TOUCH_SWAP_XY="${CH347_TOUCH_SWAP_XY:-0}"
CH347_TOUCH_INVERT_X="${CH347_TOUCH_INVERT_X:-0}"
CH347_TOUCH_INVERT_Y="${CH347_TOUCH_INVERT_Y:-0}"
CH347_TOUCH_X_MIN="${CH347_TOUCH_X_MIN:-200}"
CH347_TOUCH_X_MAX="${CH347_TOUCH_X_MAX:-3900}"
CH347_TOUCH_Y_MIN="${CH347_TOUCH_Y_MIN:-200}"
CH347_TOUCH_Y_MAX="${CH347_TOUCH_Y_MAX:-3900}"
CH347_TOUCH_WIDTH="${CH347_TOUCH_WIDTH:-$WIDTH}"
CH347_TOUCH_HEIGHT="${CH347_TOUCH_HEIGHT:-$HEIGHT}"
CH347_TOUCH_MOVE_THRESH="${CH347_TOUCH_MOVE_THRESH:-2}"
CH347_TOUCH_JUMP_THRESH="${CH347_TOUCH_JUMP_THRESH:-160}"
CH347_TOUCH_FILTER_WEIGHT="${CH347_TOUCH_FILTER_WEIGHT:-1}"
CH347_TOUCH_MAX_ERRORS="${CH347_TOUCH_MAX_ERRORS:-8}"
CH347_TOUCH_CLOCK="${CH347_TOUCH_CLOCK:-5}"
CH347_TOUCH_RELEASE_SAMPLES="${CH347_TOUCH_RELEASE_SAMPLES:-3}"
CH347_TOUCH_CAL_SAMPLES="${CH347_TOUCH_CAL_SAMPLES:-4}"
CH347_TOUCH_Z_MIN="${CH347_TOUCH_Z_MIN:-50}"
CH347_TOUCH_PRESSURE_MIN="${CH347_TOUCH_PRESSURE_MIN:-$CH347_TOUCH_Z_MIN}"
CH347_TOUCH_Z_STRICT="${CH347_TOUCH_Z_STRICT:-1}"
CH347_TOUCH_DEBUG="${CH347_TOUCH_DEBUG:-0}"
CH347_TOUCH_DISABLE_ON_ERRORS="${CH347_TOUCH_DISABLE_ON_ERRORS:-0}"
if [ -z "${XCAP_IDLE_FPS:-}" ]; then
    XCAP_IDLE_FPS=0
fi
case "$DISPLAY_ROTATION:$WIDTH:$HEIGHT" in
    normal:320:480|portrait:320:480|inverted:320:480|180:320:480|\
    right:480:320|clockwise:480:320|landscape:480:320|\
    left:480:320|counter-clockwise:480:320)
        ;;
    *)
        echo "rotation $DISPLAY_ROTATION is incompatible with logical size ${WIDTH}x${HEIGHT}" >&2
        exit 2
        ;;
esac
if [ "$DISPLAY_ROTATION" != "normal" ] &&
        [ "$DISPLAY_ROTATION" != "portrait" ] &&
        [ "$CAPTURE" != "xdamage" ]; then
    echo "rotated output currently requires CAPTURE=xdamage" >&2
    exit 2
fi
USB_SYS="${CH347_USB_SYS:-/sys/bus/usb/devices/1-1.1}"
IFACE_ID="${CH347_IFACE_ID:-1-1.1:1.4}"
CH347_DEVICE_NODE="${CH347_DEVICE_NODE:-/dev/ch34x_pis0}"
CH347_WAIT_SEC="${CH347_WAIT_SEC:-12}"
RUN_DIR="${RUN_DIR:-/tmp/ch347_dirty_usb_x11}"
PID_FILE="$RUN_DIR/pids"
CH347_LINK_STATE_FILE="${CH347_LINK_STATE_FILE:-$RUN_DIR/ch347-link-state.env}"
STACK_OWNER_FILE="${CH347_STACK_OWNER_FILE:-}"
STACK_OWNER_TOKEN="${CH347_STACK_OWNER_TOKEN:-}"
CH347_TOUCH_MODE_FILE="${CH347_TOUCH_MODE_FILE:-$RUN_DIR/touch_mode}"
FRAME_MAILBOX="${CH347_FRAME_MAILBOX:-$RUN_DIR/frame.mailbox}"
LOG_FILE="$RUN_DIR/live.log"
APPLIED_CONFIG_FILE="${MSYS_CH347_APPLIED_CONFIG_FILE:-}"
APPLIED_OVERLAY_FILE="${MSYS_CH347_APPLIED_OVERLAY_FILE:-}"
APPLIED_CURSOR_FILE="${MSYS_CH347_APPLIED_CURSOR_FILE:-}"
APPLIED_ROTATION_FILE="${MSYS_CH347_APPLIED_ROTATION_FILE:-}"
PROVIDER_GENERATION="${MSYS_GENERATION:-0}"
mkdir -p "$RUN_DIR"
if [ ! -f "$CH347_TOUCH_MODE_FILE" ]; then
    printf '%s\n' "$CH347_TOUCH_MODE" > "$CH347_TOUCH_MODE_FILE"
fi

XVFB_PID=""
WM_PID=""
APP_PID=""
GATE_PID=""
UNBOUND=0
STOP_REQUESTED=0
STREAM_CAP_PID=""
STREAM_SINK_PID=""
X_SESSION_LOST=0
PUBLISHED_CH347_LINK_STATE=""
RELOAD_REQUESTED=0
DISPLAY_CONFIG_LOADED=0

case "$CH347_RESTART_MAX" in
    ''|*[!0-9]*) CH347_RESTART_MAX=0 ;;
esac
case "$X_SESSION_FATAL_RC" in
    ''|*[!0-9]*) X_SESSION_FATAL_RC=70 ;;
esac
case "$X_SESSION_PROBES" in
    ''|*[!0-9]*) X_SESSION_PROBES=3 ;;
esac
[ "$X_SESSION_FATAL_RC" -ge 1 ] && [ "$X_SESSION_FATAL_RC" -le 255 ] || X_SESSION_FATAL_RC=70
[ "$X_SESSION_PROBES" -ge 1 ] && [ "$X_SESSION_PROBES" -le 20 ] || X_SESSION_PROBES=3

discover_ch347()
{
    local iface
    local base
    local devdir

    for iface in /sys/bus/usb/devices/*:1.4; do
        [ -e "$iface" ] || continue
        base="$(basename "$iface")"
        devdir="/sys/bus/usb/devices/${base%:*}"
        [ -r "$devdir/idVendor" ] || continue
        [ -r "$devdir/idProduct" ] || continue
        [ "$(cat "$devdir/idVendor")" = "1a86" ] || continue
        [ "$(cat "$devdir/idProduct")" = "55de" ] || continue

        USB_SYS="$devdir"
        IFACE_ID="$base"
        return 0
    done

    return 1
}

publish_ch347_link_state()
{
    local state="$1"
    local tmp="$CH347_LINK_STATE_FILE.$$.tmp"

    case "$state" in
        checking|healthy|degraded) ;;
        *) return 2 ;;
    esac
    # A superseded detached daemon must not overwrite the current
    # generation's transport health edge.
    owns_stack || return 0
    [ "$state" != "$PUBLISHED_CH347_LINK_STATE" ] || return 0
    printf 'MSYS_CH347_LINK_STATE=%s\n' "$state" > "$tmp"
    chmod 600 "$tmp" 2>/dev/null || true
    mv -f "$tmp" "$CH347_LINK_STATE_FILE"
    PUBLISHED_CH347_LINK_STATE="$state"
}

force_ch347_reconnect()
{
    local bus
    local dev
    local usb_dev
    local device_id
    local authorized

    [ "$CH347_FORCE_RECONNECT" = "1" ] || return 1
    if [ ! -r "$USB_SYS/busnum" ] || [ ! -r "$USB_SYS/devnum" ]; then
        discover_ch347 || return 1
    fi
    [ -r "$USB_SYS/busnum" ] && [ -r "$USB_SYS/devnum" ] || return 1
    bus="$(cat "$USB_SYS/busnum")"
    dev="$(cat "$USB_SYS/devnum")"
    usb_dev="$(printf '/dev/bus/usb/%03d/%03d' "$bus" "$dev")"
    device_id="$(basename "$USB_SYS")"
    echo "dirty_usb_x11_transport_reset begin device=$device_id usb=$usb_dev" >>"$LOG_FILE"
    if "$CH347_SINK" --usb-reset "$usb_dev" >>"$LOG_FILE" 2>&1; then
        echo "dirty_usb_x11_transport_reset complete method=usbdevfs device=$device_id" >>"$LOG_FILE"
        sleep 0.5
        return 0
    fi

    # Compatibility fallback for an older helper or a controller which
    # rejects USBDEVFS_RESET.  This never touches the parent hub, so the two
    # USB storage devices on neighbouring ports remain mounted.
    authorized="$USB_SYS/authorized"
    if [ -w "$authorized" ]; then
        printf '0' >"$authorized" 2>/dev/null || return 1
        sleep 0.25
        printf '1' >"$authorized" 2>/dev/null || return 1
        echo "dirty_usb_x11_transport_reset complete method=authorize device=$device_id" >>"$LOG_FILE"
        sleep 0.75
        return 0
    fi
    echo "dirty_usb_x11_transport_reset failed device=$device_id" >>"$LOG_FILE"
    return 1
}

wait_ch347_bound()
{
    local tries=$((CH347_WAIT_SEC * 4))
    local wrong_speed_checks=0
    local reset_attempted=0
    local binding

    if [ "$tries" -lt 4 ]; then
        tries=4
    fi

    for _ in $(seq 1 "$tries"); do
        discover_ch347 || true

        if [ -n "$IFACE_ID" ] && [ -d /sys/bus/usb/drivers/ch34x_pis ] &&
                [ ! -e "/sys/bus/usb/drivers/ch34x_pis/$IFACE_ID" ]; then
            printf '%s' "$IFACE_ID" > /sys/bus/usb/drivers/ch34x_pis/bind 2>/dev/null || true
        fi

        if [ -d "$USB_SYS" ] && [ -e "$CH347_DEVICE_NODE" ]; then
            binding=$(lsusb -t 2>/dev/null |
                grep 'If 4,.*Driver=ch34x_pis,' | head -n 1 || true)
            case "$binding" in
                *', 480M')
                    publish_ch347_link_state healthy
                    return 0
                    ;;
                '') wrong_speed_checks=0 ;;
                *)
                    wrong_speed_checks=$((wrong_speed_checks + 1))
                    if [ "$wrong_speed_checks" -ge 4 ] &&
                            [ "$reset_attempted" = "0" ]; then
                        echo "CH347 bound below 480M; forcing device reconnect." >&2
                        force_ch347_reconnect || true
                        reset_attempted=1
                        wrong_speed_checks=0
                    fi
                    ;;
            esac
        fi

        sleep 0.25
    done

    lsusb -t >&2 || true
    echo "CH347 did not settle as ch34x_pis at 480M within ${CH347_WAIT_SEC}s." >&2
    publish_ch347_link_state degraded
    echo "dirty_usb_x11_link_state state=degraded required_speed=480M" >>"$LOG_FILE"
    return 1
}

owns_stack()
{
    # Direct x11display development starts predate provider ownership and
    # retain their original cleanup behavior. Managed starts always supply
    # both values and must still own the exact generation token.
    if [ -z "$STACK_OWNER_FILE" ] || [ -z "$STACK_OWNER_TOKEN" ]; then
        return 0
    fi
    [ -f "$STACK_OWNER_FILE" ] &&
        [ "$(cat "$STACK_OWNER_FILE" 2>/dev/null || true)" = "$STACK_OWNER_TOKEN" ]
}

refresh_pid_file()
{
    local pid
    local tmp="$PID_FILE.$$.tmp"

    # A superseded daemon must never replace the new generation's registry.
    owns_stack || return 0
    {
        printf '%s\n' "$$"
        for pid in "$XVFB_PID" "$WM_PID" "$APP_PID" "$GATE_PID" \
                "$STREAM_CAP_PID" "$STREAM_SINK_PID"; do
            [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && printf '%s\n' "$pid"
        done
    } > "$tmp"
    mv -f "$tmp" "$PID_FILE"
}

stop_stream()
{
    set +e
    [ -n "$STREAM_SINK_PID" ] && kill "$STREAM_SINK_PID" 2>/dev/null || true
    [ -n "$STREAM_CAP_PID" ] && kill "$STREAM_CAP_PID" 2>/dev/null || true
    [ -n "$STREAM_SINK_PID" ] && wait "$STREAM_SINK_PID" 2>/dev/null || true
    [ -n "$STREAM_CAP_PID" ] && wait "$STREAM_CAP_PID" 2>/dev/null || true
    STREAM_SINK_PID=""
    STREAM_CAP_PID=""
    refresh_pid_file
}

stop_x_stack()
{
    set +e
    [ -n "$GATE_PID" ] && kill "$GATE_PID" 2>/dev/null || true
    [ -n "$APP_PID" ] && kill -CONT "$APP_PID" 2>/dev/null || true
    [ -n "$APP_PID" ] && kill "$APP_PID" 2>/dev/null || true
    [ -n "$WM_PID" ] && kill "$WM_PID" 2>/dev/null || true
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
    GATE_PID=""
    APP_PID=""
    WM_PID=""
    XVFB_PID=""
}

cleanup()
{
    set +e
    if [ "${CLEANED:-0}" = "1" ]; then
        return
    fi
    CLEANED=1
    stop_stream

    if owns_stack; then
        stop_x_stack
        if [ "$UNBOUND" = "1" ] &&
                [ -d /sys/bus/usb/drivers/ch34x_pis ] &&
                [ ! -e "/sys/bus/usb/drivers/ch34x_pis/$IFACE_ID" ]; then
            printf '%s' "$IFACE_ID" > /sys/bus/usb/drivers/ch34x_pis/bind 2>/dev/null || true
        fi
        rm -f "$PID_FILE"
    else
        echo "dirty_usb_x11_cleanup superseded owner; shared state preserved" >>"$LOG_FILE"
    fi
}

handle_term()
{
    STOP_REQUESTED=1
    cleanup
    exit 0
}

request_runtime_reload()
{
    RELOAD_REQUESTED=1
}

trap cleanup EXIT
trap handle_term INT TERM
trap request_runtime_reload USR1

x_root_dimensions()
{
    # Do not exit awk after the first match. With `set -o pipefail`, an early
    # reader exit can give xdpyinfo SIGPIPE (141) and make this assignment
    # terminate the daemon even though Xorg is healthy.
    DISPLAY="$DISPLAY_ID" xdpyinfo 2>/dev/null |
        awk '
            /^[[:space:]]*dimensions:[[:space:]]*[0-9]+x[0-9]+[[:space:]]+pixels/ && dimensions == "" {
                dimensions = $2
            }
            END {
                if (dimensions != "") {
                    print dimensions
                    exit 0
                }
                exit 1
            }
        '
}

validate_x_root_dimensions()
{
    local dimensions
    if ! dimensions="$(x_root_dimensions)"; then
        echo "X root size unavailable expected=${WIDTH}x${HEIGHT}" >>"$LOG_FILE"
        return 1
    fi
    if [ "$dimensions" != "${WIDTH}x${HEIGHT}" ]; then
        echo "X root size mismatch expected=${WIDTH}x${HEIGHT} actual=$dimensions" >>"$LOG_FILE"
        return 1
    fi
}

rotation_geometry()
{
    case "$1" in
        normal|inverted) printf '320 480\n' ;;
        right|left) printf '480 320\n' ;;
        *) return 1 ;;
    esac
}

publish_applied_runtime_config()
{
    [ -z "$APPLIED_CONFIG_FILE" ] ||
        ch347_write_display_config "$APPLIED_CONFIG_FILE" "$DEBUG" "$FPS" \
            "$XCAP_MAX_FPS" "$XCAP_IDLE_FPS" "$PROVIDER_GENERATION" || return 1
    [ -z "$APPLIED_OVERLAY_FILE" ] ||
        ch347_write_debug_overlay_config "$APPLIED_OVERLAY_FILE" \
            "$CH347_CONFIG_OVERLAY_ENABLED" "$CH347_CONFIG_OVERLAY_ALPHA" \
            "$CH347_CONFIG_OVERLAY_SCALE" "$CH347_CONFIG_OVERLAY_ITEMS" \
            "$CH347_CONFIG_OVERLAY_INTERVAL_MS" "$PROVIDER_GENERATION" || return 1
    [ -z "$APPLIED_CURSOR_FILE" ] ||
        ch347_write_cursor_config "$APPLIED_CURSOR_FILE" \
            "$CH347_CONFIG_CURSOR_ENABLED" "$PROVIDER_GENERATION" || return 1
    [ -z "$APPLIED_ROTATION_FILE" ] ||
        ch347_write_rotation_config "$APPLIED_ROTATION_FILE" \
            "$DISPLAY_ROTATION" "$PROVIDER_GENERATION" || return 1
}

pause_capture_for_rotation()
{
    local state=""

    [ -n "$STREAM_CAP_PID" ] || return 1
    kill -STOP "$STREAM_CAP_PID" 2>/dev/null || return 1
    for _ in $(seq 1 50); do
        state="$(awk '/^State:/ { print $2 }' \
            "/proc/$STREAM_CAP_PID/status" 2>/dev/null || true)"
        case "$state" in
            T|t)
                # XShmGetImage is synchronous. Give Xorg one bounded moment
                # to finish a request that was already sent before SIGSTOP,
                # then use a second client round-trip as the resize barrier.
                sleep 0.03
                DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null 2>&1 || {
                    kill -CONT "$STREAM_CAP_PID" 2>/dev/null || true
                    return 1
                }
                return 0
                ;;
        esac
        sleep 0.01
    done
    kill -CONT "$STREAM_CAP_PID" 2>/dev/null || true
    return 1
}

resume_capture_after_rotation()
{
    local reload="${1:-0}"

    if [ "$reload" = 1 ]; then
        kill -USR1 "$STREAM_CAP_PID" 2>/dev/null || {
            kill -CONT "$STREAM_CAP_PID" 2>/dev/null || true
            return 1
        }
    fi
    kill -CONT "$STREAM_CAP_PID" 2>/dev/null
}

apply_runtime_config()
{
    local new_debug new_fps new_max_fps new_idle_fps
    local new_overlay new_alpha new_scale new_items new_interval
    local new_cursor new_rotation new_width new_height geometry
    local capture_reload_sent=0

    RELOAD_REQUESTED=0
    if [ "$DISPLAY_CONFIG_LOADED" != 1 ]; then
        if [ ! -f "$DISPLAY_CONFIG_HELPER" ]; then
            echo "dirty_usb_x11_control_reload rejected=config-helper" >>"$LOG_FILE"
            return 1
        fi
        # shellcheck disable=SC1090
        . "$DISPLAY_CONFIG_HELPER"
        DISPLAY_CONFIG_LOADED=1
    fi
    if ! ch347_read_display_config "$CH347_FPS_FILE"; then
        echo "dirty_usb_x11_control_reload rejected=display-config" >>"$LOG_FILE"
        return 1
    fi
    new_debug="$CH347_CONFIG_DEBUG"
    new_fps="$CH347_CONFIG_FPS"
    new_max_fps="$CH347_CONFIG_MAX_FPS"
    new_idle_fps="$CH347_CONFIG_IDLE_FPS"
    if ! ch347_read_debug_overlay_config "$CH347_DEBUG_OVERLAY_FILE"; then
        echo "dirty_usb_x11_control_reload rejected=debug-overlay" >>"$LOG_FILE"
        return 1
    fi
    new_overlay="$CH347_CONFIG_OVERLAY_ENABLED"
    new_alpha="$CH347_CONFIG_OVERLAY_ALPHA"
    new_scale="$CH347_CONFIG_OVERLAY_SCALE"
    new_items="$CH347_CONFIG_OVERLAY_ITEMS"
    new_interval="$CH347_CONFIG_OVERLAY_INTERVAL_MS"
    if ! ch347_read_cursor_config "$CH347_CURSOR_FILE"; then
        echo "dirty_usb_x11_control_reload rejected=cursor" >>"$LOG_FILE"
        return 1
    fi
    new_cursor="$CH347_CONFIG_CURSOR_ENABLED"
    if ! ch347_read_rotation_config "$CH347_ROTATION_FILE"; then
        echo "dirty_usb_x11_control_reload rejected=rotation" >>"$LOG_FILE"
        return 1
    fi
    new_rotation="$CH347_CONFIG_ROTATION"
    geometry="$(rotation_geometry "$new_rotation")" || return 1
    read -r new_width new_height <<EOF
$geometry
EOF

    if [ "$new_rotation" != "$DISPLAY_ROTATION" ] ||
            [ "$new_width" != "$WIDTH" ] || [ "$new_height" != "$HEIGHT" ]; then
        if [ "$CAPTURE" != xdamage ]; then
            echo "dirty_usb_x11_control_reload rejected=rotation-capture-$CAPTURE" >>"$LOG_FILE"
            return 1
        fi
        if ! pause_capture_for_rotation; then
            echo "dirty_usb_x11_control_reload rejected=capture-pause" >>"$LOG_FILE"
            return 1
        fi
        if ! DISPLAY="$DISPLAY_ID" xrandr --size "${new_width}x${new_height}" \
                >>"$LOG_FILE" 2>&1; then
            resume_capture_after_rotation 0 || true
            echo "dirty_usb_x11_control_reload rejected=xrandr-${new_width}x${new_height}" >>"$LOG_FILE"
            return 1
        fi
        WIDTH="$new_width"
        HEIGHT="$new_height"
        DISPLAY_ROTATION="$new_rotation"
        validate_x_root_dimensions || {
            resume_capture_after_rotation 1 || true
            echo "dirty_usb_x11_control_reload rejected=root-geometry" >>"$LOG_FILE"
            return 1
        }
        if ! resume_capture_after_rotation 1; then
            echo "dirty_usb_x11_control_reload rejected=capture-resume" >>"$LOG_FILE"
            return 1
        fi
        capture_reload_sent=1
    fi

    DEBUG="$new_debug"
    FPS="$new_fps"
    XCAP_MAX_FPS="$new_max_fps"
    XCAP_IDLE_FPS="$new_idle_fps"
    CH347_DEBUG_OVERLAY="$new_overlay"
    CH347_DEBUG_OVERLAY_ALPHA="$new_alpha"
    CH347_DEBUG_OVERLAY_SCALE="$new_scale"
    CH347_CONFIG_OVERLAY_ITEMS="$new_items"
    CH347_DEBUG_OVERLAY_ITEMS="$(ch347_debug_overlay_items_text "$new_items")"
    CH347_DEBUG_OVERLAY_INTERVAL_MS="$new_interval"
    CH347_CURSOR="$new_cursor"
    CH347_CONFIG_CURSOR_ENABLED="$new_cursor"
    CH347_CONFIG_OVERLAY_ENABLED="$new_overlay"
    CH347_CONFIG_OVERLAY_ALPHA="$new_alpha"
    CH347_CONFIG_OVERLAY_SCALE="$new_scale"
    CH347_CONFIG_OVERLAY_INTERVAL_MS="$new_interval"

    if [ "$capture_reload_sent" != 1 ]; then
        [ -n "$STREAM_CAP_PID" ] && kill -USR1 "$STREAM_CAP_PID" 2>/dev/null || return 1
    fi
    [ -n "$STREAM_SINK_PID" ] && kill -USR1 "$STREAM_SINK_PID" 2>/dev/null || return 1
    publish_applied_runtime_config || {
        echo "dirty_usb_x11_control_reload rejected=receipt" >>"$LOG_FILE"
        return 1
    }
    echo "dirty_usb_x11_control_reload applied generation=$PROVIDER_GENERATION debug=$DEBUG fps=$FPS idle_fps=$XCAP_IDLE_FPS overlay=$CH347_DEBUG_OVERLAY cursor=$CH347_CURSOR rotation=$DISPLAY_ROTATION logical=${WIDTH}x${HEIGHT}" >>"$LOG_FILE"
}

start_x_stack()
{
    local xnum
    local period
    local render_s
    local rest_s
    local gate_render_ms

    xnum="${DISPLAY_ID#:}"
    xnum="${xnum%%.*}"
    if [ "$XSERVER" = "Xvfb" ]; then
        nice -n "$XVFB_NICE" Xvfb "$DISPLAY_ID" -screen 0 "${WIDTH}x${HEIGHT}x24" -nolisten tcp -dumbSched >>"$LOG_FILE" 2>&1 &
    else
        nice -n "$XVFB_NICE" Xorg "$DISPLAY_ID" -noreset -nolisten tcp -novtswitch -sharevts -config "$XORG_CONFIG" -logfile "$RUN_DIR/Xorg.log" >>"$LOG_FILE" 2>&1 &
    fi

    XVFB_PID="$!"

    for _ in $(seq 1 40); do
        if DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done

    if ! DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null 2>&1; then
        echo "X server failed to become ready on $DISPLAY_ID" >>"$LOG_FILE"
        return 1
    fi

    if [ "$XSERVER" != "Xvfb" ]; then
        if ! DISPLAY="$DISPLAY_ID" xrandr --size "${WIDTH}x${HEIGHT}" >>"$LOG_FILE" 2>&1; then
            echo "Xorg cannot select logical size ${WIDTH}x${HEIGHT}" >>"$LOG_FILE"
            return 1
        fi
    fi
    validate_x_root_dimensions || return 1

    case "$WM" in
        none)
            WM_PID=""
            ;;
        openbox)
            DISPLAY="$DISPLAY_ID" nice -n "$APP_NICE" openbox >>"$LOG_FILE" 2>&1 &
            WM_PID="$!"
            ;;
        *)
            DISPLAY="$DISPLAY_ID" nice -n "$APP_NICE" bash -lc "$WM" >>"$LOG_FILE" 2>&1 &
            WM_PID="$!"
            ;;
    esac
    case "$APP" in
        glxgears)
            DISPLAY="$DISPLAY_ID" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GALLIUM_THREAD=0 vblank_mode=0 \
                nice -n "$APP_NICE" \
                glxgears -geometry "${WIDTH}x${HEIGHT}+0+0" >>"$LOG_FILE" 2>&1 &
            APP_PID="$!"
            ;;
        none)
            APP_PID=""
            ;;
        *)
            DISPLAY="$DISPLAY_ID" nice -n "$APP_NICE" bash -lc "$APP" >>"$LOG_FILE" 2>&1 &
            APP_PID="$!"
            ;;
    esac

    sleep 2

    if [ "$GATED" = "1" ] && [ -n "$APP_PID" ]; then
        kill -STOP "$APP_PID" 2>/dev/null || true

        period="$(awk -v fps="$FPS" 'BEGIN { if (fps <= 0) fps = 1; printf "%.6f", 1.0 / fps }')"
        gate_render_ms="$RENDER_MS"
        if [ "$APP" = "glxgears" ]; then
            gate_render_ms="$GLXGEARS_RENDER_MS"
        fi
        render_s="$(awk -v ms="$gate_render_ms" 'BEGIN { if (ms < 1) ms = 1; printf "%.6f", ms / 1000.0 }')"
        rest_s="$(awk -v p="$period" -v r="$render_s" 'BEGIN { v = p - r; if (v < 0.001) v = 0.001; printf "%.6f", v }')"

        (
            while :; do
                kill -CONT "$APP_PID" 2>/dev/null || exit 0
                sleep "$render_s"
                kill -STOP "$APP_PID" 2>/dev/null || exit 0
                sleep "$rest_s"
            done
        ) &
        GATE_PID="$!"
    fi
    refresh_pid_file
}

ensure_x_stack()
{
    local probe=0

    # Never silently replace Xorg here.  Replacing it disconnects every X11
    # client and makes Core mistake a new session for a recovered transport.
    if [ -z "$XVFB_PID" ] || ! kill -0 "$XVFB_PID" 2>/dev/null; then
        X_SESSION_LOST=1
        echo "dirty_usb_x11_x_session_lost display=$DISPLAY_ID reason=server-exited" >>"$LOG_FILE"
        return "$X_SESSION_FATAL_RC"
    fi
    while [ "$probe" -lt "$X_SESSION_PROBES" ]; do
        if DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null 2>&1 &&
                validate_x_root_dimensions; then
            return 0
        fi
        probe=$((probe + 1))
        [ "$probe" -ge "$X_SESSION_PROBES" ] || sleep 0.1
    done
    X_SESSION_LOST=1
    echo "dirty_usb_x11_x_session_lost display=$DISPLAY_ID reason=probe-failed" >>"$LOG_FILE"
    return "$X_SESSION_FATAL_RC"
}

# MSYS_CH347_DAEMON_MAIN
mkdir -p "$RUN_DIR"
echo "$$" > "$PID_FILE"
publish_ch347_link_state checking

echo "dirty_usb_x11_start capture=$CAPTURE fps=$FPS xcap_max_fps=$XCAP_MAX_FPS xcap_idle_fps=$XCAP_IDLE_FPS xcap_output=$XCAP_OUTPUT rotation=$DISPLAY_ROTATION logical=${WIDTH}x${HEIGHT} transport=shm-mailbox max_frames=$MAX_FRAMES depth=$DEPTH app=$APP wm=$WM display=$DISPLAY_ID pixfmt=$PIXFMT gated=$GATED render_ms=$RENDER_MS packet_us=$PACKET_US clock=$CH347_CLOCK full_pct_config=$CH347_FULL_AREA_PCT full_pct_policy=$CH347_FULL_AREA_POLICY max_rects=$CH347_MAX_RECTS stale_ms=$CH347_STALE_MS stale_budget=$CH347_STALE_BUDGET hold_cs=$CH347_HOLD_CS latest_only=$CH347_LATEST_ONLY touch=$CH347_TOUCH touch_irq=$CH347_TOUCH_USE_IRQ cursor=$CH347_CURSOR calibrate=$CH347_TOUCH_CALIBRATE gpio_overlay=$CH347_GPIO_OVERLAY gpio_overlay_ms=$CH347_GPIO_OVERLAY_MS urb_timeout_ms=$CH347_URB_TIMEOUT_MS restart_on_fail=$CH347_RESTART_ON_FAIL sink=$CH347_SINK" >>"$LOG_FILE"
start_x_stack

run_stream_once()
{
    local bus
    local dev
    local usb_dev
    local ffmpeg_rc
    local cap_rc
    local sink_rc
    local cap_pid
    local sink_pid
    local finished_pid
    local first_rc

    X_SESSION_LOST=0
    ensure_x_stack || return "$?"
    wait_ch347_bound || return 1
    LD_LIBRARY_PATH="$CH347_DIR:${LD_LIBRARY_PATH:-}" \
        "$CH347_ARM" "$CH347_MODE" "$CH347_CLOCK" arm 480 0 raw >>"$LOG_FILE" 2>&1 || return 1
    wait_ch347_bound || return 1

    bus="$(cat "$USB_SYS/busnum")"
    dev="$(cat "$USB_SYS/devnum")"
    usb_dev="$(printf '/dev/bus/usb/%03d/%03d' "$bus" "$dev")"

    if [ -e "/sys/bus/usb/drivers/ch34x_pis/$IFACE_ID" ]; then
        printf '%s' "$IFACE_ID" > /sys/bus/usb/drivers/ch34x_pis/unbind
        UNBOUND=1
    fi

    case "$CAPTURE" in
        xdamage)
            rm -f "$FRAME_MAILBOX"
            MAX_FRAMES="$MAX_FRAMES" WIDTH="$WIDTH" HEIGHT="$HEIGHT" \
            XCAP_MAX_FPS="$XCAP_MAX_FPS" XCAP_IDLE_FPS="$XCAP_IDLE_FPS" \
            XCAP_DEBUG="$XCAP_DEBUG" XCAP_OUTPUT=frame \
            XCAP_ROTATION="$DISPLAY_ROTATION" \
            XCAP_FPS_FILE="$XCAP_FPS_FILE" \
            XCAP_ROTATION_FILE="$CH347_ROTATION_FILE" \
            XCAP_MAILBOX="$FRAME_MAILBOX" \
                nice -n "$XCAP_NICE" \
                "$XCAP_BIN" "$DISPLAY_ID" "$WIDTH" "$HEIGHT" \
                    "$XCAP_MAX_FPS" "$XCAP_IDLE_FPS" >>"$LOG_FILE" 2>&1 &
            cap_pid="$!"
            STREAM_CAP_PID="$cap_pid"

            CH347_USB_DEV="$usb_dev" CH347_DEBUG="$DEBUG" \
              CH347_FPS_FILE="$CH347_FPS_FILE" \
              CH347_DEBUG_OVERLAY_FILE="$CH347_DEBUG_OVERLAY_FILE" \
              CH347_CURSOR_FILE="$CH347_CURSOR_FILE" \
              CH347_ROTATION_FILE="$CH347_ROTATION_FILE" \
              CH347_DEBUG_OVERLAY="$CH347_DEBUG_OVERLAY" \
              CH347_DEBUG_OVERLAY_ALPHA="$CH347_DEBUG_OVERLAY_ALPHA" \
              CH347_DEBUG_OVERLAY_SCALE="$CH347_DEBUG_OVERLAY_SCALE" \
              CH347_DEBUG_OVERLAY_ITEMS="$CH347_DEBUG_OVERLAY_ITEMS" \
              CH347_DEBUG_OVERLAY_INTERVAL_MS="$CH347_DEBUG_OVERLAY_INTERVAL_MS" \
              CH347_PACKET_US="$PACKET_US" \
              CH347_FRAME_MAILBOX="$FRAME_MAILBOX" \
              CH347_FULL_AREA_PCT="$CH347_FULL_AREA_PCT" CH347_MAX_RECTS="$CH347_MAX_RECTS" \
              CH347_STALE_MS="$CH347_STALE_MS" CH347_STALE_BUDGET="$CH347_STALE_BUDGET" \
              CH347_HOLD_CS="$CH347_HOLD_CS" CH347_LATEST_ONLY=1 \
              CH347_URB_TIMEOUT_MS="$CH347_URB_TIMEOUT_MS" CH347_USB_DEBUG="$CH347_USB_DEBUG" \
              CH347_GPIO_OVERLAY="$CH347_GPIO_OVERLAY" CH347_GPIO_OVERLAY_MS="$CH347_GPIO_OVERLAY_MS" \
              CH347_TOUCH="$CH347_TOUCH" CH347_TOUCH_USE_IRQ="$CH347_TOUCH_USE_IRQ" \
              CH347_TOUCH_MODE="$CH347_TOUCH_MODE" CH347_TOUCH_MODE_FILE="$CH347_TOUCH_MODE_FILE" \
              CH347_CURSOR="$CH347_CURSOR" CH347_TOUCH_CALIBRATE="$CH347_TOUCH_CALIBRATE" \
              CH347_TOUCH_CAL_FILE="$CH347_TOUCH_CAL_FILE" CH347_TOUCH_CAL_EXIT="$CH347_TOUCH_CAL_EXIT" \
              CH347_TOUCH_CAL_MARGIN="$CH347_TOUCH_CAL_MARGIN" CH347_TOUCH_POLL_MS="$CH347_TOUCH_POLL_MS" \
              CH347_TOUCH_SWAP_XY="$CH347_TOUCH_SWAP_XY" CH347_TOUCH_INVERT_X="$CH347_TOUCH_INVERT_X" \
              CH347_TOUCH_INVERT_Y="$CH347_TOUCH_INVERT_Y" CH347_TOUCH_X_MIN="$CH347_TOUCH_X_MIN" \
              CH347_TOUCH_X_MAX="$CH347_TOUCH_X_MAX" CH347_TOUCH_Y_MIN="$CH347_TOUCH_Y_MIN" \
              CH347_TOUCH_Y_MAX="$CH347_TOUCH_Y_MAX" CH347_TOUCH_WIDTH="$CH347_TOUCH_WIDTH" \
              CH347_TOUCH_HEIGHT="$CH347_TOUCH_HEIGHT" CH347_TOUCH_MOVE_THRESH="$CH347_TOUCH_MOVE_THRESH" \
              CH347_TOUCH_JUMP_THRESH="$CH347_TOUCH_JUMP_THRESH" CH347_TOUCH_FILTER_WEIGHT="$CH347_TOUCH_FILTER_WEIGHT" \
              CH347_TOUCH_MAX_ERRORS="$CH347_TOUCH_MAX_ERRORS" CH347_TOUCH_CLOCK="$CH347_TOUCH_CLOCK" \
              CH347_TOUCH_RELEASE_SAMPLES="$CH347_TOUCH_RELEASE_SAMPLES" CH347_TOUCH_CAL_SAMPLES="$CH347_TOUCH_CAL_SAMPLES" \
              CH347_TOUCH_Z_MIN="$CH347_TOUCH_Z_MIN" CH347_TOUCH_PRESSURE_MIN="$CH347_TOUCH_PRESSURE_MIN" \
              CH347_TOUCH_Z_STRICT="$CH347_TOUCH_Z_STRICT" CH347_TOUCH_DEBUG="$CH347_TOUCH_DEBUG" \
              CH347_TOUCH_DISABLE_ON_ERRORS="$CH347_TOUCH_DISABLE_ON_ERRORS" DISPLAY_ID="$DISPLAY_ID" \
              CH347_DISPLAY_ROTATION="$DISPLAY_ROTATION" \
              nice -n "$CH347_SINK_NICE" "$CH347_SINK" "$DEPTH" "$MAX_FRAMES" &
            sink_pid="$!"
            STREAM_SINK_PID="$sink_pid"
            refresh_pid_file
            while :; do
                finished_pid=""
                wait -n -p finished_pid "$cap_pid" "$sink_pid"
                first_rc="$?"
                if [ "$RELOAD_REQUESTED" = 1 ] &&
                        kill -0 "$cap_pid" 2>/dev/null &&
                        kill -0 "$sink_pid" 2>/dev/null; then
                    apply_runtime_config || true
                    continue
                fi
                # Bash unsets the variable named by `wait -p` when a trapped
                # signal interrupts wait(1).  With `set -u`, expanding it
                # directly would terminate the daemon after a successful
                # runtime reload and tear down the still-healthy X session.
                if [ -n "${finished_pid:-}" ]; then
                    break
                fi
                # Bash may interrupt wait(1) for a trapped signal without
                # identifying a child. Continue waiting while both stream
                # processes remain alive; a control HUP is not a transport
                # failure and must never enter the reconnect path.
                if kill -0 "$cap_pid" 2>/dev/null &&
                        kill -0 "$sink_pid" 2>/dev/null; then
                    # Some Bash versions return 127 on the first wait after an
                    # interrupted `wait -n`.  Avoid a tight poll while keeping
                    # both children and the existing X session intact.
                    sleep 0.02
                    continue
                fi
                if ! kill -0 "$sink_pid" 2>/dev/null; then
                    finished_pid="$sink_pid"
                else
                    finished_pid="$cap_pid"
                fi
                wait "$finished_pid" 2>/dev/null
                first_rc="$?"
                break
            done
            if [ "${finished_pid:-}" = "$sink_pid" ]; then
                sink_rc="$first_rc"
                STREAM_SINK_PID=""
                kill "$cap_pid" 2>/dev/null || true
                wait "$cap_pid" 2>/dev/null
                cap_rc="$?"
                STREAM_CAP_PID=""
            else
                # A dead capture otherwise leaves the sink blocked forever on
                # a mailbox whose producer is gone. Treat it as a recoverable
                # stream fault while the same X session is still healthy.
                cap_rc="$first_rc"
                STREAM_CAP_PID=""
                kill "$sink_pid" 2>/dev/null || true
                wait "$sink_pid" 2>/dev/null || true
                STREAM_SINK_PID=""
                if [ "$cap_rc" = "0" ]; then sink_rc=1; else sink_rc="$cap_rc"; fi
            fi
            refresh_pid_file
            ffmpeg_rc="$cap_rc"
            ;;
        ffmpeg)
            ffmpeg -hide_banner -loglevel warning \
                -f x11grab -draw_mouse 0 -video_size "${WIDTH}x${HEIGHT}" -framerate "$FPS" \
                -i "${DISPLAY_ID}.0+0,0" \
                -vf "format=${PIXFMT}" -pix_fmt "$PIXFMT" -f rawvideo - \
            | CH347_USB_DEV="$usb_dev" CH347_DEBUG="$DEBUG" \
              CH347_DEBUG_OVERLAY="$CH347_DEBUG_OVERLAY" \
              CH347_DEBUG_OVERLAY_ALPHA="$CH347_DEBUG_OVERLAY_ALPHA" \
              CH347_DEBUG_OVERLAY_SCALE="$CH347_DEBUG_OVERLAY_SCALE" \
              CH347_DEBUG_OVERLAY_ITEMS="$CH347_DEBUG_OVERLAY_ITEMS" \
              CH347_DEBUG_OVERLAY_INTERVAL_MS="$CH347_DEBUG_OVERLAY_INTERVAL_MS" \
              CH347_PACKET_US="$PACKET_US" \
              CH347_FULL_AREA_PCT="$CH347_FULL_AREA_PCT" CH347_MAX_RECTS="$CH347_MAX_RECTS" \
              CH347_STALE_MS="$CH347_STALE_MS" CH347_STALE_BUDGET="$CH347_STALE_BUDGET" \
              CH347_HOLD_CS="$CH347_HOLD_CS" \
              CH347_LATEST_ONLY="$CH347_LATEST_ONLY" \
              CH347_URB_TIMEOUT_MS="$CH347_URB_TIMEOUT_MS" \
              CH347_USB_DEBUG="$CH347_USB_DEBUG" \
              CH347_GPIO_OVERLAY="$CH347_GPIO_OVERLAY" \
              CH347_GPIO_OVERLAY_MS="$CH347_GPIO_OVERLAY_MS" \
              CH347_TOUCH="$CH347_TOUCH" \
              CH347_TOUCH_USE_IRQ="$CH347_TOUCH_USE_IRQ" \
              CH347_TOUCH_MODE="$CH347_TOUCH_MODE" \
              CH347_TOUCH_MODE_FILE="$CH347_TOUCH_MODE_FILE" \
              CH347_CURSOR="$CH347_CURSOR" \
              CH347_TOUCH_CALIBRATE="$CH347_TOUCH_CALIBRATE" \
              CH347_TOUCH_CAL_FILE="$CH347_TOUCH_CAL_FILE" \
              CH347_TOUCH_CAL_EXIT="$CH347_TOUCH_CAL_EXIT" \
              CH347_TOUCH_CAL_MARGIN="$CH347_TOUCH_CAL_MARGIN" \
              CH347_TOUCH_POLL_MS="$CH347_TOUCH_POLL_MS" \
              CH347_TOUCH_SWAP_XY="$CH347_TOUCH_SWAP_XY" \
              CH347_TOUCH_INVERT_X="$CH347_TOUCH_INVERT_X" \
              CH347_TOUCH_INVERT_Y="$CH347_TOUCH_INVERT_Y" \
              CH347_TOUCH_X_MIN="$CH347_TOUCH_X_MIN" \
              CH347_TOUCH_X_MAX="$CH347_TOUCH_X_MAX" \
              CH347_TOUCH_Y_MIN="$CH347_TOUCH_Y_MIN" \
              CH347_TOUCH_Y_MAX="$CH347_TOUCH_Y_MAX" \
              CH347_TOUCH_WIDTH="$CH347_TOUCH_WIDTH" \
              CH347_TOUCH_HEIGHT="$CH347_TOUCH_HEIGHT" \
              CH347_TOUCH_MOVE_THRESH="$CH347_TOUCH_MOVE_THRESH" \
              CH347_TOUCH_JUMP_THRESH="$CH347_TOUCH_JUMP_THRESH" \
              CH347_TOUCH_FILTER_WEIGHT="$CH347_TOUCH_FILTER_WEIGHT" \
              CH347_TOUCH_MAX_ERRORS="$CH347_TOUCH_MAX_ERRORS" \
              CH347_TOUCH_CLOCK="$CH347_TOUCH_CLOCK" \
              CH347_TOUCH_RELEASE_SAMPLES="$CH347_TOUCH_RELEASE_SAMPLES" \
              CH347_TOUCH_CAL_SAMPLES="$CH347_TOUCH_CAL_SAMPLES" \
              CH347_TOUCH_Z_MIN="$CH347_TOUCH_Z_MIN" \
              CH347_TOUCH_PRESSURE_MIN="$CH347_TOUCH_PRESSURE_MIN" \
              CH347_TOUCH_Z_STRICT="$CH347_TOUCH_Z_STRICT" \
              CH347_TOUCH_DEBUG="$CH347_TOUCH_DEBUG" \
              CH347_TOUCH_DISABLE_ON_ERRORS="$CH347_TOUCH_DISABLE_ON_ERRORS" \
              DISPLAY_ID="$DISPLAY_ID" \
              nice -n "$CH347_SINK_NICE" \
              "$CH347_SINK" "$DEPTH" "$MAX_FRAMES"
            PIPE_RC=("${PIPESTATUS[@]}")
            cap_rc="${PIPE_RC[0]}"
            sink_rc="${PIPE_RC[1]}"
            ffmpeg_rc="$cap_rc"
            ;;
        *)
            echo "unknown CAPTURE=$CAPTURE" >>"$LOG_FILE"
            false
            ;;
    esac
    echo "dirty_usb_x11_stream_exit capture=$CAPTURE cap=$cap_rc ffmpeg=$ffmpeg_rc sink=$sink_rc" >>"$LOG_FILE"

    if [ -d /sys/bus/usb/drivers/ch34x_pis ] &&
            [ ! -e "/sys/bus/usb/drivers/ch34x_pis/$IFACE_ID" ]; then
        printf '%s' "$IFACE_ID" > /sys/bus/usb/drivers/ch34x_pis/bind 2>/dev/null || true
        UNBOUND=0
    fi

    return "$sink_rc"
}

restart_sleep()
{
    local left="$CH347_RESTART_DELAY_SEC"

    while [ "$left" -gt 0 ]; do
        [ -f "$PID_FILE" ] || {
            STOP_REQUESTED=1
            return 1
        }
        sleep 1
        left=$((left - 1))
    done

    return 0
}

RESTARTS=0
while :; do
    set +e
    run_stream_once
    rc="$?"
    set -e

    if [ "$rc" = "0" ]; then
        if [ "$CH347_RESTART_ON_FAIL" = "1" ] && [ "$MAX_FRAMES" = "0" ]; then
            # The production stream is unbounded. A zero-status sink exit is
            # therefore not completion (older sinks returned zero after a USB
            # write failure if at least one frame had already succeeded).
            echo "dirty_usb_x11_restart rc=0 reason=unexpected-stream-exit count=$RESTARTS" >>"$LOG_FILE"
            rc=1
        else
            echo "dirty_usb_x11_exit restarts=$RESTARTS status=0" >>"$LOG_FILE"
            exit 0
        fi
    fi

    if [ "$X_SESSION_LOST" = "1" ] && [ "$rc" = "$X_SESSION_FATAL_RC" ]; then
        echo "dirty_usb_x11_exit restarts=$RESTARTS status=x-session-lost rc=$rc" >>"$LOG_FILE"
        exit "$rc"
    fi

    echo "dirty_usb_x11_restart rc=$rc count=$RESTARTS" >>"$LOG_FILE"

    if [ "$STOP_REQUESTED" = "1" ] || [ ! -f "$PID_FILE" ]; then
        echo "dirty_usb_x11_exit restarts=$RESTARTS status=stopped" >>"$LOG_FILE"
        exit 0
    fi

    if [ "$CH347_RESTART_ON_FAIL" != "1" ] || [ "$MAX_FRAMES" != "0" ]; then
        echo "dirty_usb_x11_exit restarts=$RESTARTS status=$rc" >>"$LOG_FILE"
        exit "$rc"
    fi

    if [ "$CH347_RESTART_MAX" -gt 0 ] &&
            [ "$RESTARTS" -ge "$CH347_RESTART_MAX" ]; then
        echo "dirty_usb_x11_exit restarts=$RESTARTS status=max_restarts" >>"$LOG_FILE"
        exit "$rc"
    fi

    RESTARTS=$((RESTARTS + 1))
    restart_sleep || {
        echo "dirty_usb_x11_exit restarts=$RESTARTS status=stopped" >>"$LOG_FILE"
        exit 0
    }
done
