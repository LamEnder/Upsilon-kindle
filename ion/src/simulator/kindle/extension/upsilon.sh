#!/bin/sh
# Upsilon launcher for KUAL (Kindle Unified Application Launcher)

EXT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="${EXT_DIR}/upsilon.log"

# KUAL waits for its actions; run detached so the Kindle UI can be paused.
if [ "$1" != "--detached" ]; then
    if command -v nohup >/dev/null 2>&1; then
        nohup "$0" --detached >/dev/null 2>&1 &
    else
        "$0" --detached >/dev/null 2>&1 </dev/null &
    fi
    exit 0
fi

UPSILON_ARGS=""
KEEP_FRAMEWORK=""
[ -f "${EXT_DIR}/upsilon.conf" ] && . "${EXT_DIR}/upsilon.conf"

restore_kindle_ui() {
    if [ -z "${KEEP_FRAMEWORK}" ]; then
        killall -CONT cvm 2>/dev/null
    fi
    lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
    lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
    # Make the home screen repaint over our leftovers
    lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home 2>/dev/null
}

# Give KUAL a moment to close its menu
sleep 1

lipc-set-prop com.lab126.pillow disableEnablePillow disable 2>/dev/null
lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null
if [ -z "${KEEP_FRAMEWORK}" ]; then
    # Pause the Kindle UI so it neither draws on the screen nor reacts to taps
    killall -STOP cvm 2>/dev/null
fi
trap restore_kindle_ui EXIT
trap 'exit 1' INT TERM HUP

{
    echo "--- $(date) ---"
    cat /proc/version 2>/dev/null
} >"${LOG}"

# shellcheck disable=SC2086
"${EXT_DIR}/bin/upsilon" --state "${EXT_DIR}/upsilon.state" ${UPSILON_ARGS} >>"${LOG}" 2>&1
echo "exit status $?" >>"${LOG}"
