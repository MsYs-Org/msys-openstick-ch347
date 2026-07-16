#!/bin/bash
# Strict parser/writer helpers for the small CH347 display tuning document.
# This file is sourced by both the package provider and the standalone start
# script.  It deliberately parses data instead of sourcing shell code.

ch347_config_uint()
{
    local value="$1"
    local minimum="$2"
    local maximum="$3"
    local decimal

    case "$value" in
        ''|*[!0-9]*) return 1 ;;
    esac
    # Bound the input before arithmetic so an attacker cannot feed Bash an
    # arbitrarily large integer expression.  All current limits fit 3 digits.
    [ "${#value}" -le 3 ] || return 1
    decimal=$((10#$value))
    [ "$decimal" -ge "$minimum" ] && [ "$decimal" -le "$maximum" ]
}

ch347_read_display_config()
{
    local config="$1"
    local line
    local value
    local seen_debug=0
    local seen_fps=0
    local seen_max=0
    local seen_idle=0

    CH347_CONFIG_DEBUG=0
    CH347_CONFIG_FPS=""
    CH347_CONFIG_MAX_FPS=""
    CH347_CONFIG_IDLE_FPS=""

    [ ! -L "$config" ] && [ -f "$config" ] || {
        echo "CH347 display config is missing: $config" >&2
        return 1
    }
    value=$(wc -c < "$config") || return 1
    case "$value" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ "$value" -le 16384 ] || {
        echo "CH347 display config exceeds 16384 bytes" >&2
        return 1
    }
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            ''|'#'*) continue ;;
            DEBUG=*)
                [ "$seen_debug" = 0 ] || {
                    echo "duplicate DEBUG in CH347 display config" >&2
                    return 1
                }
                value="${line#DEBUG=}"
                [ "$value" = 0 ] || [ "$value" = 1 ] || {
                    echo "DEBUG must be 0 or 1 in CH347 display config" >&2
                    return 1
                }
                CH347_CONFIG_DEBUG="$value"
                seen_debug=1
                ;;
            FPS=*)
                [ "$seen_fps" = 0 ] || {
                    echo "duplicate FPS in CH347 display config" >&2
                    return 1
                }
                value="${line#FPS=}"
                ch347_config_uint "$value" 1 240 || {
                    echo "FPS must be an integer from 1 to 240" >&2
                    return 1
                }
                CH347_CONFIG_FPS="$((10#$value))"
                seen_fps=1
                ;;
            XCAP_MAX_FPS=*)
                [ "$seen_max" = 0 ] || {
                    echo "duplicate XCAP_MAX_FPS in CH347 display config" >&2
                    return 1
                }
                value="${line#XCAP_MAX_FPS=}"
                ch347_config_uint "$value" 1 240 || {
                    echo "XCAP_MAX_FPS must be an integer from 1 to 240" >&2
                    return 1
                }
                CH347_CONFIG_MAX_FPS="$((10#$value))"
                seen_max=1
                ;;
            XCAP_IDLE_FPS=*)
                [ "$seen_idle" = 0 ] || {
                    echo "duplicate XCAP_IDLE_FPS in CH347 display config" >&2
                    return 1
                }
                value="${line#XCAP_IDLE_FPS=}"
                ch347_config_uint "$value" 0 60 || {
                    echo "XCAP_IDLE_FPS must be an integer from 0 to 60" >&2
                    return 1
                }
                CH347_CONFIG_IDLE_FPS="$((10#$value))"
                seen_idle=1
                ;;
            *)
                echo "unsupported assignment in CH347 display config" >&2
                return 1
                ;;
        esac
    done < "$config"

    [ "$seen_fps" = 1 ] && [ "$seen_max" = 1 ] && [ "$seen_idle" = 1 ] || {
        echo "CH347 display config is incomplete" >&2
        return 1
    }
    [ "$CH347_CONFIG_FPS" = "$CH347_CONFIG_MAX_FPS" ] || {
        echo "FPS and XCAP_MAX_FPS must match" >&2
        return 1
    }
    [ "$CH347_CONFIG_IDLE_FPS" -le "$CH347_CONFIG_FPS" ] || {
        echo "XCAP_IDLE_FPS must not exceed FPS" >&2
        return 1
    }
    export CH347_CONFIG_DEBUG CH347_CONFIG_FPS CH347_CONFIG_MAX_FPS \
        CH347_CONFIG_IDLE_FPS
}

ch347_write_display_config()
{
    local target="$1"
    local debug="$2"
    local fps="$3"
    local max_fps="$4"
    local idle_fps="$5"
    local generation="${6:-}"
    local directory
    local tmp

    [ "$debug" = 0 ] || [ "$debug" = 1 ] || return 1
    ch347_config_uint "$fps" 1 240 || return 1
    ch347_config_uint "$max_fps" 1 240 || return 1
    ch347_config_uint "$idle_fps" 0 60 || return 1
    [ "$fps" = "$max_fps" ] && [ "$idle_fps" -le "$fps" ] || return 1
    if [ -n "$generation" ]; then
        case "$generation" in
            ''|*[!0-9]*) return 1 ;;
        esac
        [ "${#generation}" -le 10 ] || return 1
        generation=$((10#$generation))
        [ "$generation" -le 2147483647 ] || return 1
    fi

    if { [ -e "$target" ] || [ -L "$target" ]; } &&
            { [ -L "$target" ] || [ ! -f "$target" ]; }; then
        echo "CH347 display config target must be a regular non-symlink file" >&2
        return 1
    fi

    directory="$(dirname -- "$target")"
    mkdir -p "$directory"
    [ -d "$directory" ] && [ ! -L "$directory" ] || {
        echo "CH347 display config directory must not be a symlink" >&2
        return 1
    }
    tmp=$(mktemp "$directory/.ch347-display-config.XXXXXX") || return 1
    (
        trap 'rm -f "$tmp"' EXIT HUP INT TERM
        {
            [ -z "$generation" ] || printf 'MSYS_GENERATION=%s\n' "$generation"
            printf 'DEBUG=%s\n' "$debug"
            printf 'FPS=%s\n' "$fps"
            printf 'XCAP_MAX_FPS=%s\n' "$max_fps"
            printf 'XCAP_IDLE_FPS=%s\n' "$idle_fps"
        } > "$tmp"
        chmod 600 "$tmp"
        mv -f "$tmp" "$target"
        trap - EXIT HUP INT TERM
    )
}

ch347_read_debug_overlay_config()
{
    local config="$1"
    local line value
    local seen_enabled=0 seen_alpha=0 seen_scale=0 seen_items=0 seen_interval=0

    CH347_CONFIG_OVERLAY_ENABLED=0
    CH347_CONFIG_OVERLAY_ALPHA=176
    CH347_CONFIG_OVERLAY_SCALE=1
    CH347_CONFIG_OVERLAY_ITEMS=39
    CH347_CONFIG_OVERLAY_INTERVAL_MS=1000
    [ ! -L "$config" ] && [ -f "$config" ] || return 1
    value=$(wc -c < "$config") || return 1
    case "$value" in ''|*[!0-9]*) return 1 ;; esac
    [ "$value" -le 4096 ] || return 1
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            ''|'#'*) continue ;;
            CH347_DEBUG_OVERLAY=*)
                [ "$seen_enabled" = 0 ] || return 1
                value="${line#CH347_DEBUG_OVERLAY=}"
                [ "$value" = 0 ] || [ "$value" = 1 ] || return 1
                CH347_CONFIG_OVERLAY_ENABLED="$value"; seen_enabled=1 ;;
            CH347_DEBUG_OVERLAY_ALPHA=*)
                [ "$seen_alpha" = 0 ] || return 1
                value="${line#CH347_DEBUG_OVERLAY_ALPHA=}"
                ch347_config_uint "$value" 0 255 || return 1
                CH347_CONFIG_OVERLAY_ALPHA="$((10#$value))"; seen_alpha=1 ;;
            CH347_DEBUG_OVERLAY_SCALE=*)
                [ "$seen_scale" = 0 ] || return 1
                value="${line#CH347_DEBUG_OVERLAY_SCALE=}"
                ch347_config_uint "$value" 1 2 || return 1
                CH347_CONFIG_OVERLAY_SCALE="$((10#$value))"; seen_scale=1 ;;
            CH347_DEBUG_OVERLAY_ITEMS=*)
                [ "$seen_items" = 0 ] || return 1
                value="${line#CH347_DEBUG_OVERLAY_ITEMS=}"
                ch347_config_uint "$value" 1 63 || return 1
                CH347_CONFIG_OVERLAY_ITEMS="$((10#$value))"; seen_items=1 ;;
            CH347_DEBUG_OVERLAY_INTERVAL_MS=*)
                [ "$seen_interval" = 0 ] || return 1
                value="${line#CH347_DEBUG_OVERLAY_INTERVAL_MS=}"
                case "$value" in ''|*[!0-9]*) return 1 ;; esac
                [ "${#value}" -le 4 ] || return 1
                value=$((10#$value))
                [ "$value" -ge 250 ] && [ "$value" -le 5000 ] || return 1
                CH347_CONFIG_OVERLAY_INTERVAL_MS="$value"; seen_interval=1 ;;
            *) return 1 ;;
        esac
    done < "$config"
    [ "$seen_enabled$seen_alpha$seen_scale$seen_items$seen_interval" = 11111 ]
    export CH347_CONFIG_OVERLAY_ENABLED CH347_CONFIG_OVERLAY_ALPHA \
        CH347_CONFIG_OVERLAY_SCALE CH347_CONFIG_OVERLAY_ITEMS \
        CH347_CONFIG_OVERLAY_INTERVAL_MS
}

ch347_write_debug_overlay_config()
{
    local target="$1" enabled="$2" alpha="$3" scale="$4" items="$5"
    local interval="$6" generation="${7:-}" directory tmp

    [ "$enabled" = 0 ] || [ "$enabled" = 1 ] || return 1
    ch347_config_uint "$alpha" 0 255 || return 1
    ch347_config_uint "$scale" 1 2 || return 1
    ch347_config_uint "$items" 1 63 || return 1
    case "$interval" in ''|*[!0-9]*) return 1 ;; esac
    [ "${#interval}" -le 4 ] || return 1
    interval=$((10#$interval))
    [ "$interval" -ge 250 ] && [ "$interval" -le 5000 ] || return 1
    if [ -n "$generation" ]; then
        case "$generation" in ''|*[!0-9]*) return 1 ;; esac
        [ "${#generation}" -le 10 ] || return 1
    fi
    if { [ -e "$target" ] || [ -L "$target" ]; } &&
            { [ -L "$target" ] || [ ! -f "$target" ]; }; then return 1; fi
    directory="$(dirname -- "$target")"
    mkdir -p "$directory"
    [ -d "$directory" ] && [ ! -L "$directory" ] || return 1
    tmp=$(mktemp "$directory/.ch347-debug-overlay.XXXXXX") || return 1
    (
        trap 'rm -f "$tmp"' EXIT HUP INT TERM
        {
            [ -z "$generation" ] || printf 'MSYS_GENERATION=%s\n' "$generation"
            printf 'CH347_DEBUG_OVERLAY=%s\n' "$enabled"
            printf 'CH347_DEBUG_OVERLAY_ALPHA=%s\n' "$alpha"
            printf 'CH347_DEBUG_OVERLAY_SCALE=%s\n' "$scale"
            printf 'CH347_DEBUG_OVERLAY_ITEMS=%s\n' "$items"
            printf 'CH347_DEBUG_OVERLAY_INTERVAL_MS=%s\n' "$interval"
        } > "$tmp"
        chmod 600 "$tmp"
        mv -f "$tmp" "$target"
        trap - EXIT HUP INT TERM
    )
}

ch347_debug_overlay_items_text()
{
    local mask="$1" output="" item bit
    for item in fps dirty bytes bbox memory cpu; do
        case "$item" in fps) bit=1;; dirty) bit=2;; bytes) bit=4;; bbox) bit=8;; memory) bit=16;; cpu) bit=32;; esac
        if [ $((mask & bit)) -ne 0 ]; then
            [ -z "$output" ] || output="$output,"
            output="$output$item"
        fi
    done
    printf '%s\n' "$output"
}

ch347_read_cursor_config()
{
    local config="$1" line value seen=0 size

    CH347_CONFIG_CURSOR_ENABLED=0
    [ ! -L "$config" ] && [ -f "$config" ] || return 1
    size=$(wc -c < "$config") || return 1
    case "$size" in ''|*[!0-9]*) return 1 ;; esac
    [ "$size" -le 256 ] || return 1
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            ''|'#'*) continue ;;
            CH347_CURSOR=*)
                [ "$seen" = 0 ] || return 1
                value="${line#CH347_CURSOR=}"
                [ "$value" = 0 ] || [ "$value" = 1 ] || return 1
                CH347_CONFIG_CURSOR_ENABLED="$value"
                seen=1
                ;;
            *) return 1 ;;
        esac
    done < "$config"
    [ "$seen" = 1 ] || return 1
    export CH347_CONFIG_CURSOR_ENABLED
}

ch347_write_cursor_config()
{
    local target="$1" enabled="$2" generation="${3:-}" directory tmp

    [ "$enabled" = 0 ] || [ "$enabled" = 1 ] || return 1
    if [ -n "$generation" ]; then
        case "$generation" in ''|*[!0-9]*) return 1 ;; esac
        [ "${#generation}" -le 10 ] || return 1
        generation=$((10#$generation))
        [ "$generation" -le 2147483647 ] || return 1
    fi
    if { [ -e "$target" ] || [ -L "$target" ]; } &&
            { [ -L "$target" ] || [ ! -f "$target" ]; }; then return 1; fi
    directory="$(dirname -- "$target")"
    mkdir -p "$directory"
    [ -d "$directory" ] && [ ! -L "$directory" ] || return 1
    tmp=$(mktemp "$directory/.ch347-cursor.XXXXXX") || return 1
    (
        trap 'rm -f "$tmp"' EXIT HUP INT TERM
        {
            [ -z "$generation" ] || printf 'MSYS_GENERATION=%s\n' "$generation"
            printf 'CH347_CURSOR=%s\n' "$enabled"
        } > "$tmp"
        chmod 600 "$tmp"
        mv -f "$tmp" "$target"
        trap - EXIT HUP INT TERM
    )
}

ch347_read_rotation_config()
{
    local config="$1" line value seen=0 size

    CH347_CONFIG_ROTATION=normal
    [ ! -L "$config" ] && [ -f "$config" ] || return 1
    size=$(wc -c < "$config") || return 1
    case "$size" in ''|*[!0-9]*) return 1 ;; esac
    [ "$size" -le 256 ] || return 1
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            ''|'#'*) continue ;;
            CH347_DISPLAY_ROTATION=*)
                [ "$seen" = 0 ] || return 1
                value="${line#CH347_DISPLAY_ROTATION=}"
                case "$value" in
                    normal|right|inverted|left) ;;
                    *) return 1 ;;
                esac
                CH347_CONFIG_ROTATION="$value"
                seen=1
                ;;
            *) return 1 ;;
        esac
    done < "$config"
    [ "$seen" = 1 ] || return 1
    export CH347_CONFIG_ROTATION
}

ch347_write_rotation_config()
{
    local target="$1" rotation="$2" generation="${3:-}" directory tmp

    case "$rotation" in normal|right|inverted|left) ;; *) return 1 ;; esac
    if [ -n "$generation" ]; then
        case "$generation" in ''|*[!0-9]*) return 1 ;; esac
        [ "${#generation}" -le 10 ] || return 1
        generation=$((10#$generation))
        [ "$generation" -le 2147483647 ] || return 1
    fi
    if { [ -e "$target" ] || [ -L "$target" ]; } &&
            { [ -L "$target" ] || [ ! -f "$target" ]; }; then return 1; fi
    directory="$(dirname -- "$target")"
    mkdir -p "$directory"
    [ -d "$directory" ] && [ ! -L "$directory" ] || return 1
    tmp=$(mktemp "$directory/.ch347-rotation.XXXXXX") || return 1
    (
        trap 'rm -f "$tmp"' EXIT HUP INT TERM
        {
            [ -z "$generation" ] || printf 'MSYS_GENERATION=%s\n' "$generation"
            printf 'CH347_DISPLAY_ROTATION=%s\n' "$rotation"
        } > "$tmp"
        chmod 600 "$tmp"
        mv -f "$tmp" "$target"
        trap - EXIT HUP INT TERM
    )
}
