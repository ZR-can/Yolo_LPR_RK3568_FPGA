#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CTRL_TOOL=${FPGA_BAR0_CTRL_TOOL:-"$SCRIPT_DIR/fpga_bar0_ctrl_test"}

REG_MODE=0x150
REG_THRESHOLD=0x160
REG_ROI_XY=0x170
REG_ROI_WH=0x180
REG_CAPTURE_CTRL=0x130
REG_DEBUG_TRIG=0x190
REG_FRAME_CFG=0x1a0

usage() {
    cat <<EOF
Usage: $0 <command> [arguments]

Commands:
  status
  mode <bypass|brightness|contrast|roi-zoom|0|1|2|3>
  threshold <0..255>
  roi <x> <y> <width> <height>
  apply <mode> <threshold> <x> <y> <width> <height>
  capture <on|off>
  debug-trig <value>
  frame-cfg <value>

Examples:
  $0 status
  $0 mode brightness
  $0 threshold 128
  $0 roi 100 100 512 256
  $0 capture on
EOF
}

die() {
    echo "fpga_preproc_ctrl: $*" >&2
    exit 2
}

require_tool() {
    if [ ! -f "$CTRL_TOOL" ]; then
        die "control tool is missing: $CTRL_TOOL"
    fi
    if [ ! -x "$CTRL_TOOL" ]; then
        chmod +x "$CTRL_TOOL" 2>/dev/null ||
            die "control tool is not executable and chmod failed: $CTRL_TOOL"
    fi
}

parse_u32() {
    case "$1" in
        0|[1-9][0-9]*|0x[0-9a-fA-F]*|0X[0-9a-fA-F]*) ;;
        *) die "invalid unsigned value: $1" ;;
    esac
    printf '%u' "$(( $1 ))" 2>/dev/null || die "invalid unsigned value: $1"
}

parse_u16_dec() {
    case "$1" in
        0|[1-9][0-9]*) ;;
        *) die "invalid decimal value: $1" ;;
    esac
    value=$(( $1 ))
    [ "$value" -ge 0 ] && [ "$value" -le 65535 ] ||
        die "value out of 16-bit range: $1"
    printf '%u' "$value"
}

write_and_read() {
    offset=$1
    value=$2
    attempt=1
    while [ "$attempt" -le 3 ]; do
        echo "FPGA register attempt=$attempt offset=$offset value=$value" >&2
        if "$CTRL_TOOL" --write "$offset" --value "$value" --read "$offset"; then
            return 0
        fi
        status=$?
        echo "FPGA register attempt failed status=$status offset=$offset" >&2
        sleep 0.05
        attempt=$((attempt + 1))
    done
    return 1
}

parse_mode() {
    case "$1" in
        bypass|0) printf '0' ;;
        brightness|light|lighting|gray|grayscale|1) printf '1' ;;
        contrast|enhance|threshold|binary|2) printf '2' ;;
        roi|roi-zoom|zoom|3) printf '3' ;;
        *) die "mode must be bypass, brightness, contrast, roi-zoom, or 0..3" ;;
    esac
}

require_tool

[ "$#" -gt 0 ] || { usage; exit 2; }

case "$1" in
    status)
        [ "$#" -eq 1 ] || die "status takes no arguments"
        "$CTRL_TOOL" --regs
        ;;
    mode)
        [ "$#" -eq 2 ] || die "mode requires one value"
        value=$(parse_mode "$2")
        write_and_read "$REG_MODE" "$value"
        ;;
    threshold)
        [ "$#" -eq 2 ] || die "threshold requires one value"
        value=$(parse_u16_dec "$2")
        [ "$value" -le 255 ] || die "threshold must be in range 0..255"
        write_and_read "$REG_THRESHOLD" "$value"
        ;;
    roi)
        [ "$#" -eq 5 ] || die "roi requires x y width height"
        x=$(parse_u16_dec "$2")
        y=$(parse_u16_dec "$3")
        width=$(parse_u16_dec "$4")
        height=$(parse_u16_dec "$5")
        xy=$(( (y << 16) | x ))
        wh=$(( (height << 16) | width ))
        write_and_read "$REG_ROI_XY" "$(printf '0x%08x' "$xy")"
        write_and_read "$REG_ROI_WH" "$(printf '0x%08x' "$wh")"
        ;;
    apply)
        [ "$#" -eq 7 ] || die "apply requires mode threshold x y width height"
        mode_value=$(parse_mode "$2")
        threshold_value=$(parse_u16_dec "$3")
        [ "$threshold_value" -le 255 ] || die "threshold must be in range 0..255"
        x=$(parse_u16_dec "$4")
        y=$(parse_u16_dec "$5")
        width=$(parse_u16_dec "$6")
        height=$(parse_u16_dec "$7")
        xy=$(( (y << 16) | x ))
        wh=$(( (height << 16) | width ))
        write_and_read "$REG_MODE" "$mode_value"
        write_and_read "$REG_THRESHOLD" "$threshold_value"
        write_and_read "$REG_ROI_XY" "$(printf '0x%08x' "$xy")"
        write_and_read "$REG_ROI_WH" "$(printf '0x%08x' "$wh")"
        ;;
    capture)
        [ "$#" -eq 2 ] || die "capture requires on or off"
        case "$2" in
            on|1) value=1 ;;
            off|0) value=0 ;;
            *) die "capture must be on or off" ;;
        esac
        write_and_read "$REG_CAPTURE_CTRL" "$value"
        ;;
    debug-trig)
        [ "$#" -eq 2 ] || die "debug-trig requires one value"
        value=$(parse_u32 "$2")
        write_and_read "$REG_DEBUG_TRIG" "$value"
        ;;
    frame-cfg)
        [ "$#" -eq 2 ] || die "frame-cfg requires one value"
        value=$(parse_u32 "$2")
        write_and_read "$REG_FRAME_CFG" "$value"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        die "unknown command: $1"
        ;;
esac
