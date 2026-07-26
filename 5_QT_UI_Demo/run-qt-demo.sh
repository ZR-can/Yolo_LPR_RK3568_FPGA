#!/bin/bash

set -euo pipefail

DEMO_DIR="${LPR_QT_DEMO_DIR:-/userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui}"
DEMO_BINARY="${LPR_QT_DEMO_BINARY:-yolov8_ppocr_pcie_qt_ui}"
LIB_DIR="${LPR_QT_LIB_DIR:-/userdata/rknn_yolov8_ppocr_qt_ui_demo/lib}"
LAUNCHER_NAME="${LPR_QT_LAUNCHER_NAME:-$0}"
X11_SOCKET=/tmp/.X11-unix/X0
XAUTHORITY_FILE=/var/run/lightdm/root/:0

print_usage() {
    echo "Usage: ${LAUNCHER_NAME} [--roi-config PATH] [--roi \"x1,y1;x2,y2;...\"] [--light-roi \"left,top,right,bottom\"]"
}

DEMO_ARGS=("$@")
if [ "$#" -eq 1 ] && { [ "$1" = "--help" ] || [ "$1" = "-h" ]; }; then
    print_usage
    exit 0
fi
if [ $(( $# % 2 )) -ne 0 ]; then
    print_usage >&2
    exit 2
fi

ROI_SEEN=0
LIGHT_ROI_SEEN=0
ROI_CONFIG_SEEN=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --roi-config)
            if [ "${ROI_CONFIG_SEEN}" -ne 0 ]; then
                print_usage >&2
                exit 2
            fi
            ROI_CONFIG_SEEN=1
            ;;
        --roi)
            if [ "${ROI_SEEN}" -ne 0 ]; then
                print_usage >&2
                exit 2
            fi
            ROI_SEEN=1
            ;;
        --light-roi)
            if [ "${LIGHT_ROI_SEEN}" -ne 0 ]; then
                print_usage >&2
                exit 2
            fi
            LIGHT_ROI_SEEN=1
            ;;
        *)
            print_usage >&2
            exit 2
            ;;
    esac
    shift 2
done

if [ "$(id -u)" -ne 0 ]; then
    echo "This launcher must run as root." >&2
    exit 1
fi
if [ ! -d "${DEMO_DIR}" ]; then
    echo "Demo directory is missing: ${DEMO_DIR}" >&2
    exit 1
fi
if [ ! -f "${DEMO_DIR}/${DEMO_BINARY}" ]; then
    echo "Demo executable is missing: ${DEMO_DIR}/${DEMO_BINARY}" >&2
    exit 1
fi

systemctl isolate graphical.target

desktop_process_running() {
    ps -ef | grep -Ei 'Xorg|lightdm' | grep -v grep >/dev/null
}

active_mode_for_output() {
    local display_output=$1
    xrandr --query |
        awk -v target="${display_output}" '
            $1 == target && $2 == "connected" { in_target = 1; next }
            in_target && /^[^[:space:]]/ { exit }
            in_target && /\*/ { print $1; exit }
        '
}

configure_native_1080_output() {
    if ! command -v xrandr >/dev/null 2>&1; then
        echo "xrandr is required to verify the 1920x1080 fullscreen layout." >&2
        return 1
    fi

    local display_output
    display_output=$(
        xrandr --query |
            awk '$2 == "connected" && $1 ~ /^HDMI/ { print $1; exit }'
    )
    if [ -z "${display_output}" ]; then
        echo "No connected HDMI output was found for the 1920x1080 fullscreen layout." >&2
        return 1
    fi

    local current_mode
    current_mode=$(active_mode_for_output "${display_output}")
    if [[ ! "${current_mode}" =~ ^[0-9]+x[0-9]+$ ]]; then
        echo "Failed to read the active mode for ${display_output}." >&2
        return 1
    fi

    if [ "${current_mode}" = "1920x1080" ]; then
        echo "Display output ${display_output}: keeping native ${current_mode}."
        return 0
    fi

    if ! xrandr --query |
        awk -v target="${display_output}" '
            $1 == target && $2 == "connected" { in_target = 1; next }
            in_target && /^[^[:space:]]/ { exit }
            in_target && $1 == "1920x1080" { found = 1 }
            END { exit found ? 0 : 1 }
        '; then
        echo "${display_output} does not advertise the required 1920x1080 mode." >&2
        return 1
    fi

    if ! xrandr --output "${display_output}" \
        --mode 1920x1080 \
        --rate 60 \
        --pos 0x0 \
        --primary; then
        echo "${display_output} rejected the required 1920x1080 mode." >&2
        return 1
    fi

    local active_mode
    active_mode=$(active_mode_for_output "${display_output}")
    if [ "${active_mode}" != "1920x1080" ]; then
        echo "${display_output} mode readback is ${active_mode:-unknown}, expected 1920x1080." >&2
        return 1
    fi

    echo "Display output ${display_output}: ${current_mode} -> ${active_mode}; native fullscreen UI enabled."
}

wait_count=0
while { [ ! -S "${X11_SOCKET}" ] ||
        [ ! -r "${XAUTHORITY_FILE}" ] ||
        ! desktop_process_running; } &&
      [ "${wait_count}" -lt 30 ]; do
    sleep 1
    wait_count=$((wait_count + 1))
done

ls -l /tmp/.X11-unix || true
ps -ef | grep -Ei 'Xorg|lightdm' | grep -v grep || true

if [ ! -S "${X11_SOCKET}" ]; then
    echo "X11 did not become ready within 30 seconds: ${X11_SOCKET}" >&2
    exit 1
fi
if [ ! -r "${XAUTHORITY_FILE}" ]; then
    echo "Xauthority file is missing or unreadable: ${XAUTHORITY_FILE}" >&2
    exit 1
fi
if ! desktop_process_running; then
    echo "Xorg/LightDM process was not found after graphical.target activation." >&2
    exit 1
fi

cd "${DEMO_DIR}"
chmod +x "./${DEMO_BINARY}"

if lsmod | grep -q '^pango_pci_driver '; then
    rmmod pango_pci_driver
fi
insmod ./pango_pci_driver.ko
ls -l /dev/pango_pci_driver

export DISPLAY=:0
export XAUTHORITY="${XAUTHORITY_FILE}"
export QT_QPA_PLATFORM=xcb
export QT_AUTO_SCREEN_SCALE_FACTOR=0
export QT_ENABLE_HIGHDPI_SCALING=0
export QT_SCALE_FACTOR=1
export RKNN_LOG_LEVEL=0
export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

configure_native_1080_output

exec "./${DEMO_BINARY}" "${DEMO_ARGS[@]}"
