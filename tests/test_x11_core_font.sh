#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 FONT_DIR XORG_CONFIG PYTHON_WITH_TK" >&2
    exit 2
fi

FONT_DIR="$1"
XORG_CONFIG="$2"
PYTHON="$3"
DISPLAY_ID="${TEST_DISPLAY:-:77}"
RUN_DIR="${TEST_RUN_DIR:-/tmp/msys-x11-core-font-test}"
XORG_PID=""

cleanup()
{
    if [ -n "$XORG_PID" ]; then
        kill "$XORG_PID" 2>/dev/null || true
        wait "$XORG_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

test -r "$FONT_DIR/fonts.dir"
test -r "$FONT_DIR/msys-cjk-14-regular.bdf"
test -r "$FONT_DIR/msys-cjk-14-bold.bdf"
test -x "$PYTHON"
mkdir -p "$RUN_DIR"
rm -f "$RUN_DIR/Xorg.log"

Xorg "$DISPLAY_ID" -noreset -nolisten tcp -novtswitch -sharevts \
    -config "$XORG_CONFIG" -logfile "$RUN_DIR/Xorg.log" \
    -fp "$FONT_DIR,built-ins" >"$RUN_DIR/stdout.log" 2>&1 &
XORG_PID="$!"

for _ in $(seq 1 50); do
    if DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null
awk '/^VmRSS:/ { print "Xorg RSS before Tk:", $2, $3 }' "/proc/$XORG_PID/status"
DISPLAY="$DISPLAY_ID" xlsfonts -ll -fn \
    '-msys-Noto Sans CJK SC-medium-r-normal--14-140-75-75-p-138-iso10646-1' |
    grep -F -- '-msys-noto sans cjk sc-medium-r-normal--14-140-75-75-p-138-iso10646-1'
DISPLAY="$DISPLAY_ID" xlsfonts -ll -fn \
    '-msys-Noto Sans CJK SC-bold-r-normal--14-140-75-75-p-138-iso10646-1' |
    grep -F -- '-msys-noto sans cjk sc-bold-r-normal--14-140-75-75-p-138-iso10646-1'

DISPLAY="$DISPLAY_ID" "$PYTHON" - <<'PY'
import os
import subprocess
import tkinter as tk
from pathlib import Path
from tkinter import font as tkfont

root = tk.Tk()
root.geometry("320x480+0+0")
root.configure(bg="#111827")
families = tuple(str(item) for item in root.tk.call("font", "families"))
print("Tk font families:", families)
if not any(item.casefold() == "noto sans cjk sc" for item in families):
    raise SystemExit("Noto Sans CJK SC is missing from Tk font families")

sample = "\u8bbe\u7f6e\u5e94\u7528\u4e2d\u6587"
for requested in ("Noto Sans CJK SC", "msyscjk"):
    for size in (8, 9, 10, 11, 12, 14, 20):
        font = tkfont.Font(root=root, family=requested, size=size)
        actual = str(font.actual("family"))
        measured = int(font.measure(sample))
        print(
            f"Tk font requested={requested!r} size={size} "
            f"actual={actual!r} measure={measured}"
        )
        if actual.casefold() != requested.casefold():
            raise SystemExit(
                f"Tk selected {actual!r} instead of requested family {requested!r}"
            )
        if measured <= 0:
            raise SystemExit(f"Tk cannot measure Chinese with {requested!r}")

    bold = tkfont.Font(root=root, family=requested, size=11, weight="bold")
    bold_actual = str(bold.actual("family"))
    bold_weight = str(bold.actual("weight"))
    bold_measure = int(bold.measure(sample))
    print(
        f"Tk bold requested={requested!r} actual={bold_actual!r} "
        f"weight={bold_weight!r} measure={bold_measure}"
    )
    if bold_actual.casefold() != requested.casefold() or bold_weight != "bold":
        raise SystemExit(f"Tk did not select the real bold {requested!r} strike")
    if bold_measure <= 0:
        raise SystemExit(f"Tk cannot measure bold Chinese with {requested!r}")

title_font = tkfont.Font(
    root=root, family="Noto Sans CJK SC", size=20, weight="bold"
)
body_font = tkfont.Font(root=root, family="Noto Sans CJK SC", size=16)
tk.Label(
    root,
    text="\u8bbe\u7f6e \u00b7 \u5e94\u7528",
    font=title_font,
    bg="#111827",
    fg="#F8FAFC",
    pady=48,
).pack(fill="x")
tk.Label(
    root,
    text=(
        "\u4e2d\u6587\u5b57\u4f53\u771f\u5b9e\u6e32\u67d3\n"
        "Wi-Fi \u00b7 \u84dd\u7259 \u00b7 \u663e\u793a"
    ),
    font=body_font,
    bg="#1F2937",
    fg="#93C5FD",
    padx=12,
    pady=24,
).pack(fill="x", padx=16)
root.update_idletasks()
root.update()

screenshot = os.environ.get("TEST_SCREENSHOT", "").strip()
if screenshot:
    output = Path(screenshot)
    if output.exists():
        raise SystemExit(f"refusing to overwrite screenshot: {output}")
    subprocess.run(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
            "-f", "x11grab", "-video_size", "320x480",
            "-i", f"{os.environ['DISPLAY']}.0+0,0",
            "-frames:v", "1", str(output),
        ],
        check=True,
        timeout=15,
    )
    print(f"Tk screenshot: {output}")
root.destroy()
PY
awk '/^VmRSS:/ { print "Xorg RSS after Tk:", $2, $3 }' "/proc/$XORG_PID/status"
