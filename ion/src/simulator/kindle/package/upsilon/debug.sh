#!/bin/sh
# Upsilon debugging helper, to run on the Kindle over SSH (USBNetwork):
#
#   sh /mnt/us/upsilon/debug.sh            status of the running Upsilon
#   sh /mnt/us/upsilon/debug.sh report     ask Upsilon for a full report
#                                          (backtrace...), then show it
#   sh /mnt/us/upsilon/debug.sh log        follow the log live
#   sh /mnt/us/upsilon/debug.sh stop       stop Upsilon and bring back the
#                                          Kindle UI, even if it is frozen

UPSILON_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="${UPSILON_DIR}/upsilon.log"
PID="$(cat "${UPSILON_DIR}/.running/upsilon.pid" 2>/dev/null)"
if [ -z "${PID}" ] || [ ! -d "/proc/${PID}" ]; then
    PID="$(pidof upsilon 2>/dev/null)"
fi

state() {
    sed -n 's/^State:[[:space:]]*//p' "/proc/$1/status" 2>/dev/null
}

case "$1" in
    report)
        if [ -z "${PID}" ]; then
            echo "Upsilon is not running"
            exit 1
        fi
        LINES=$(wc -l <"${LOG}")
        kill -QUIT "${PID}"
        sleep 2
        tail -n +$((LINES + 1)) "${LOG}"
        ;;
    log)
        tail -n 40 -f "${LOG}"
        ;;
    stop)
        if [ -n "${PID}" ]; then
            echo "stopping Upsilon (pid ${PID})"
            kill -TERM "${PID}"
            i=0
            while [ ${i} -lt 5 ] && kill -0 "${PID}" 2>/dev/null; do
                sleep 1
                i=$((i + 1))
            done
            if kill -0 "${PID}" 2>/dev/null; then
                echo "still running, state: $(state "${PID}"); killing it"
                kill -QUIT "${PID}" 2>/dev/null
                sleep 1
                kill -KILL "${PID}"
            fi
        else
            echo "Upsilon is not running"
        fi
        LAUNCHER="$(cat "${UPSILON_DIR}/.running/launcher.pid" 2>/dev/null)"
        [ -n "${LAUNCHER}" ] && kill -0 "${LAUNCHER}" 2>/dev/null && sleep 2
        rm -rf "${UPSILON_DIR}/.running"
        killall -CONT cvm 2>/dev/null
        lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
        lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
        lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home 2>/dev/null
        echo "Kindle UI restored"
        ;;
    *)
        CVM="$(pidof cvm 2>/dev/null)"
        echo "Kindle UI (cvm): pid ${CVM:-none}, $(state "${CVM}")"
        if [ -z "${PID}" ]; then
            echo "Upsilon is not running. Last lines of the log:"
            tail -n 20 "${LOG}"
            exit 0
        fi
        echo "Upsilon: pid ${PID}"
        for task in /proc/${PID}/task/*; do
            tid=$(basename "${task}")
            echo "  thread ${tid}: $(state "${tid}"), wait channel: $(cat "${task}/wchan" 2>/dev/null), syscall: $(cat "${task}/syscall" 2>/dev/null)"
            if [ -r "${task}/stack" ]; then
                sed 's/^/      /' "${task}/stack"
            fi
        done
        echo "Last lines of the log:"
        tail -n 20 "${LOG}"
        echo "(run '$0 report' for a backtrace)"
        ;;
esac
