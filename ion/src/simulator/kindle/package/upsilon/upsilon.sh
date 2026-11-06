#!/bin/sh
# Upsilon launcher for Kindle.
#
#   upsilon.sh              from KUAL: detaches, pauses the Kindle UI, then
#                           brings back the home screen when Upsilon exits
#   upsilon.sh --scriptlet  from the library (Scriptlets): same, in the
#                           foreground so the Scriptlet app ends with Upsilon
#   upsilon.sh --koreader   from the KOReader plugin: KOReader already owns
#                           the screen, so the Kindle UI is left alone
#   ... --debug             verbose log (same as UPSILON_DEBUG=1 in upsilon.conf)
#
# Logs: upsilon.log is the latest run, upsilon.1.log the one before, and so
# on up to upsilon.5.log.

UPSILON_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="${UPSILON_DIR}/upsilon.log"

MODE="kual"
DEBUG_FLAG=""
for arg in "$@"; do
    case "${arg}" in
        --scriptlet) MODE="scriptlet" ;;
        --koreader) MODE="koreader" ;;
        --detached) MODE="kual-detached" ;;
        --debug) DEBUG_FLAG="--debug" ;;
    esac
done

# KUAL waits for its actions; run detached so the Kindle UI can be paused.
if [ "${MODE}" = "kual" ]; then
    if command -v nohup >/dev/null 2>&1; then
        nohup sh "$0" --detached ${DEBUG_FLAG} >/dev/null 2>&1 &
    else
        sh "$0" --detached ${DEBUG_FLAG} >/dev/null 2>&1 </dev/null &
    fi
    exit 0
fi

log() {
    echo "[uptime $(cut -d' ' -f1 /proc/uptime 2>/dev/null)] launcher: $*" >>"${LOG}"
}

# Runs a command, killing it if it takes longer than $1 seconds. Kindle
# system services (lipc) can block when the service is busy or stopped.
run_timeout() {
    secs=$1
    shift
    "$@" >/dev/null 2>&1 &
    cmd_pid=$!
    ( sleep "${secs}"; kill -9 "${cmd_pid}" 2>/dev/null ) &
    watcher_pid=$!
    wait "${cmd_pid}"
    status=$?
    kill "${watcher_pid}" 2>/dev/null
    if [ "${status}" -eq 137 ]; then
        log "TIMEOUT: no answer after ${secs}s, gave up on: $*"
    elif [ "${status}" -ne 0 ]; then
        log "exit status ${status}: $*"
    fi
    return "${status}"
}

process_state() {
    sed -n 's/^State:[[:space:]]*//p' "/proc/$1/status" 2>/dev/null
}

# ---- Only one Upsilon at a time: a second tap on the launcher would start
# another one, and both would fight over the screen and the touch panel ----
LOCK="${UPSILON_DIR}/.running"
if ! mkdir "${LOCK}" 2>/dev/null; then
    OWNER="$(cat "${LOCK}/launcher.pid" 2>/dev/null)"
    if [ -n "${OWNER}" ] && kill -0 "${OWNER}" 2>/dev/null; then
        echo "[uptime $(cut -d' ' -f1 /proc/uptime 2>/dev/null)] launcher: launch (${MODE}) ignored, Upsilon is already starting or running (launcher pid ${OWNER})" >>"${LOG}"
        exit 0
    fi
    # Left over by a launcher that was killed
    rm -rf "${LOCK}"
    mkdir "${LOCK}" 2>/dev/null || exit 1
fi
echo $$ >"${LOCK}/launcher.pid"

# ---- Keep the logs of the previous runs ----
i=4
while [ ${i} -ge 1 ]; do
    [ -f "${UPSILON_DIR}/upsilon.${i}.log" ] && mv -f "${UPSILON_DIR}/upsilon.${i}.log" "${UPSILON_DIR}/upsilon.$((i + 1)).log"
    i=$((i - 1))
done
PREVIOUS_UNFINISHED=""
if [ -f "${LOG}" ]; then
    grep -q "launcher: upsilon exited" "${LOG}" || PREVIOUS_UNFINISHED=1
    mv -f "${LOG}" "${UPSILON_DIR}/upsilon.1.log"
fi

UPSILON_ARGS=""
KEEP_FRAMEWORK=""
[ -f "${UPSILON_DIR}/upsilon.conf" ] && . "${UPSILON_DIR}/upsilon.conf"
[ "${MODE}" = "koreader" ] && KEEP_FRAMEWORK=1
[ -n "${DEBUG_FLAG}" ] && UPSILON_DEBUG=1
[ -n "${UPSILON_DEBUG}" ] && [ "${UPSILON_DEBUG}" != "0" ] && export UPSILON_DEBUG

{
    echo "=== Upsilon launch, $(date), mode ${MODE}${UPSILON_DEBUG:+, debug} ==="
    echo "firmware: $(cat /etc/prettyversion.txt 2>/dev/null)"
    echo "kernel: $(uname -r) $(uname -v)"
    echo "memory: $(grep -E '^(MemAvailable|MemFree):' /proc/meminfo 2>/dev/null | tr -s ' ' | tr '\n' ' ')"
    echo "input devices:"
    grep -E '^(N|H):' /proc/bus/input/devices 2>/dev/null | sed 's/^/    /'
    echo "framebuffer: $(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null) $(cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null)bpp rotate $(cat /sys/class/graphics/fb0/rotate 2>/dev/null)"
} >"${LOG}"
[ -n "${PREVIOUS_UNFINISHED}" ] && log "NOTE: the previous run did not finish normally, see upsilon.1.log"

CVM_PID="$(pidof cvm 2>/dev/null)"
log "Kindle UI (cvm) pid ${CVM_PID:-none}, state: $(process_state "${CVM_PID}")"
case "$(process_state "${CVM_PID}")" in
    T*) log "NOTE: the Kindle UI was already paused before this launch" ;;
esac

UPSILON_PID=""

on_exit() {
    if [ -n "${UPSILON_PID}" ] && kill -0 "${UPSILON_PID}" 2>/dev/null; then
        log "stopping upsilon (pid ${UPSILON_PID})"
        kill -TERM "${UPSILON_PID}" 2>/dev/null
        wait "${UPSILON_PID}" 2>/dev/null
    fi
    [ -z "${KEEP_FRAMEWORK}" ] && restore_kindle_ui
    rm -rf "${LOCK}"
}
trap on_exit EXIT
trap 'log "launcher received a termination signal"; exit 1' INT TERM HUP

restore_kindle_ui() {
    log "restoring the Kindle UI"
    killall -CONT cvm 2>/dev/null
    run_timeout 5 lipc-set-prop com.lab126.powerd preventScreenSaver 0
    run_timeout 5 lipc-set-prop com.lab126.pillow disableEnablePillow enable
    if [ "${MODE}" = "kual-detached" ]; then
        # Make the home screen repaint over our leftovers. (Scriptlets
        # return to the library on their own once this script ends.)
        run_timeout 10 lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home
    fi
    log "done"
}

if [ -z "${KEEP_FRAMEWORK}" ]; then
    # Give the menu / library a moment to settle
    sleep 1
    log "pausing the Kindle UI"
    run_timeout 5 lipc-set-prop com.lab126.pillow disableEnablePillow disable
    run_timeout 5 lipc-set-prop com.lab126.powerd preventScreenSaver 1
    # Pause the Kindle UI so it neither draws on the screen nor reacts to taps
    killall -STOP cvm 2>/dev/null
    log "Kindle UI state: $(process_state "${CVM_PID}")"
fi

log "starting ${UPSILON_DIR}/bin/upsilon ${UPSILON_ARGS}"
# shellcheck disable=SC2086
"${UPSILON_DIR}/bin/upsilon" --state "${UPSILON_DIR}/upsilon.state" ${UPSILON_ARGS} >>"${LOG}" 2>&1 &
UPSILON_PID=$!
echo "${UPSILON_PID}" >"${LOCK}/upsilon.pid"
wait "${UPSILON_PID}"
STATUS=$?
UPSILON_PID=""
case ${STATUS} in
    0) log "upsilon exited normally" ;;
    70) log "upsilon exited: quit with the rescue gesture after freezing (status 70)" ;;
    *)
        if [ ${STATUS} -gt 128 ]; then
            log "upsilon exited: killed by signal $((STATUS - 128)) (status ${STATUS})"
        else
            log "upsilon exited with status ${STATUS}"
        fi
        ;;
esac
exit ${STATUS}
