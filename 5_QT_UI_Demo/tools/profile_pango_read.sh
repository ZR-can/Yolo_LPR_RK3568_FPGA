#!/bin/sh

set -eu

TRACE_BUFFER_KB_DEFAULT=16384
PANGO_READ_FUNCTION=pango_cdev_read
COPY_FUNCTION=__arch_copy_to_user

usage() {
    cat <<'EOF'
Usage:
  sudo ./profile_pango_read.sh start [buffer_kb]
  sudo ./profile_pango_read.sh stop [raw_trace_output]
  sudo ./profile_pango_read.sh report

Run "start", exercise the Qt demo for at least 30 seconds, then run "stop".
The profiler does not modify the driver or PCIe protocol. It uses the kernel
function-graph tracer to split pango_cdev_read() into copy_to_user and residual
driver time.
EOF
}

find_trace_dir() {
    for candidate in /sys/kernel/tracing /sys/kernel/debug/tracing; do
        if [ -d "$candidate" ] && [ -e "$candidate/current_tracer" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "error: root is required to configure ftrace" >&2
        exit 1
    fi
}

set_trace_option_if_available() {
    option_path="$TRACE_DIR/options/$1"
    if [ -e "$option_path" ]; then
        printf '%s\n' "$2" > "$option_path"
    fi
}

start_profile() {
    buffer_kb="${1:-$TRACE_BUFFER_KB_DEFAULT}"
    case "$buffer_kb" in
        ''|*[!0-9]*)
            echo "error: buffer_kb must be a positive integer" >&2
            exit 1
            ;;
    esac
    if [ "$buffer_kb" -le 0 ]; then
        echo "error: buffer_kb must be greater than zero" >&2
        exit 1
    fi

    if ! grep -qw "$PANGO_READ_FUNCTION" "$TRACE_DIR/available_filter_functions"; then
        echo "error: $PANGO_READ_FUNCTION is unavailable; load pango_pci_driver.ko first" >&2
        exit 1
    fi

    printf '0\n' > "$TRACE_DIR/tracing_on"
    printf 'nop\n' > "$TRACE_DIR/current_tracer"
    : > "$TRACE_DIR/trace"
    : > "$TRACE_DIR/set_graph_function"
    printf '%s\n' "$PANGO_READ_FUNCTION" > "$TRACE_DIR/set_graph_function"
    printf '%s\n' "$buffer_kb" > "$TRACE_DIR/buffer_size_kb"
    printf 'function_graph\n' > "$TRACE_DIR/current_tracer"
    set_trace_option_if_available funcgraph-tail 1
    set_trace_option_if_available funcgraph-duration 1
    set_trace_option_if_available funcgraph-proc 1
    set_trace_option_if_available funcgraph-retval 1
    printf '1\n' > "$TRACE_DIR/tracing_on"

    echo "Pango read profiling started: trace=$TRACE_DIR buffer=${buffer_kb} KiB"
    if grep -qw "$COPY_FUNCTION" "$TRACE_DIR/available_filter_functions"; then
        echo "copy function is traceable: $COPY_FUNCTION"
    else
        echo "warning: $COPY_FUNCTION is not listed as traceable; residual split may be unavailable" >&2
    fi
}

report_profile() {
    awk -v read_name="$PANGO_READ_FUNCTION" -v copy_name="$COPY_FUNCTION" '
        function to_ms(line, cleaned, factor, count, index, value, fields) {
            factor = 0.0
            if (line ~ / ns /) {
                factor = 0.000001
                sub(/ ns .*/, "", line)
            } else if (line ~ / us /) {
                factor = 0.001
                sub(/ us .*/, "", line)
            } else if (line ~ / ms /) {
                factor = 1.0
                sub(/ ms .*/, "", line)
            } else if (line ~ / s /) {
                factor = 1000.0
                sub(/ s .*/, "", line)
            } else {
                return -1.0
            }
            cleaned = line
            gsub(/[^0-9.]+/, " ", cleaned)
            count = split(cleaned, fields, / +/)
            value = ""
            for (index = 1; index <= count; ++index) {
                if (fields[index] ~ /^[0-9]+([.][0-9]+)?$/) {
                    value = fields[index]
                }
            }
            return value == "" ? -1.0 : value * factor
        }
        index($0, read_name "() {") > 0 {
            active = 1
            current_copy_ms = -1.0
            next
        }
        active && index($0, copy_name) > 0 {
            duration = to_ms($0)
            if (duration >= 0.0) {
                current_copy_ms = duration
                ++copy_samples
                copy_total += duration
                if (duration > copy_max) copy_max = duration
            }
            next
        }
        active && index($0, read_name) > 0 && index($0, "}") > 0 {
            duration = to_ms($0)
            if (duration >= 0.0) {
                if (current_copy_ms >= 0.0) {
                    residual = duration - current_copy_ms
                    if (residual < 0.0) residual = 0.0
                    ++success_samples
                    success_total += duration
                    residual_total += residual
                    if (duration > success_max) success_max = duration
                    if (residual > residual_max) residual_max = residual
                } else {
                    ++retry_samples
                    retry_total += duration
                    if (duration > retry_max) retry_max = duration
                }
            }
            active = 0
            current_copy_ms = -1.0
        }
        END {
            print "========== Pango Driver Read Profile =========="
            if (success_samples > 0) {
                printf "Successful pango_cdev_read average/max: %.3f / %.3f ms (samples=%d)\n", success_total / success_samples, success_max, success_samples
            } else {
                print "Successful pango_cdev_read: no traceable samples"
            }
            if (copy_samples > 0) {
                printf "copy_to_user average/max: %.3f / %.3f ms (samples=%d)\n", copy_total / copy_samples, copy_max, copy_samples
            } else {
                print "copy_to_user: unavailable in function-graph trace"
            }
            if (success_samples > 0 && copy_samples == success_samples) {
                printf "Driver residual average/max: %.3f / %.3f ms (samples=%d)\n", residual_total / success_samples, residual_max, success_samples
            } else {
                print "Driver residual: unavailable because successful read/copy sample counts differ"
            }
            if (retry_samples > 0) {
                printf "No-frame read average/max: %.3f / %.3f ms (samples=%d)\n", retry_total / retry_samples, retry_max, retry_samples
            }
            print "================================================="
        }
    ' "$TRACE_DIR/trace"
}

stop_profile() {
    raw_output="${1:-}"
    printf '0\n' > "$TRACE_DIR/tracing_on"
    report_profile
    if [ -n "$raw_output" ]; then
        cp "$TRACE_DIR/trace" "$raw_output"
        echo "Raw trace saved: $raw_output"
    fi
}

require_root
TRACE_DIR="$(find_trace_dir || true)"
if [ -z "$TRACE_DIR" ]; then
    echo "error: tracefs is unavailable; mount tracefs or debugfs first" >&2
    echo "example: mount -t tracefs nodev /sys/kernel/tracing" >&2
    exit 1
fi

action="${1:-}"
case "$action" in
    start)
        start_profile "${2:-$TRACE_BUFFER_KB_DEFAULT}"
        ;;
    stop)
        stop_profile "${2:-}"
        ;;
    report)
        report_profile
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
