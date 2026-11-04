#!/bin/sh
# Upsilon launcher for Kindle.
#
#   upsilon.sh              from KUAL: detaches, pauses the Kindle UI, then
#                           brings back the home screen when Upsilon exits
#   upsilon.sh --scriptlet  from the library (Scriptlets): same, in the
#                           foreground so the Scriptlet app ends with Upsilon
#   upsilon.sh --koreader   from the KOReader plugin: KOReader already owns
#                           the screen, so the Kindle UI is left alone

UPSILON_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="${UPSILON_DIR}/upsilon.log"

MODE="kual"
case "$1" in
    --scriptlet) MODE="scriptlet" ;;
    --koreader) MODE="koreader" ;;
    --detached) MODE="kual-detached" ;;
esac

# KUAL waits for its actions; run detached so the Kindle UI can be paused.
if [ "${MODE}" = "kual" ]; then
    if command -v nohup >/dev/null 2>&1; then
        nohup sh "$0" --detached >/dev/null 2>&1 &
    else
        sh "$0" --detached >/dev/null 2>&1 </dev/null &
    fi
    exit 0
fi

UPSILON_ARGS=""
KEEP_FRAMEWORK=""
[ -f "${UPSILON_DIR}/upsilon.conf" ] && . "${UPSILON_DIR}/upsilon.conf"
[ "${MODE}" = "koreader" ] && KEEP_FRAMEWORK=1

restore_kindle_ui() {
    killall -CONT cvm 2>/dev/null
    lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
    lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
    if [ "${MODE}" = "kual-detached" ]; then
        # Make the home screen repaint over our leftovers. (Scriptlets
        # return to the library on their own once this script ends.)
        lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home 2>/dev/null
    fi
}

if [ -z "${KEEP_FRAMEWORK}" ]; then
    # Give the menu / library a moment to settle
    sleep 1
    lipc-set-prop com.lab126.pillow disableEnablePillow disable 2>/dev/null
    lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null
    # Pause the Kindle UI so it neither draws on the screen nor reacts to taps
    killall -STOP cvm 2>/dev/null
    trap restore_kindle_ui EXIT
    trap 'exit 1' INT TERM HUP
fi

{
    echo "--- $(date) (${MODE}) ---"
    cat /proc/version 2>/dev/null
} >"${LOG}"

# shellcheck disable=SC2086
"${UPSILON_DIR}/bin/upsilon" --state "${UPSILON_DIR}/upsilon.state" ${UPSILON_ARGS} >>"${LOG}" 2>&1
STATUS=$?
echo "exit status ${STATUS}" >>"${LOG}"
exit ${STATUS}
