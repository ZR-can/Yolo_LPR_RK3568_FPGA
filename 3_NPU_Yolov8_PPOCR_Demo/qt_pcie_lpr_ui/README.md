# Qt PCIe LPR UI

This is a Qt 5 UI entry for the RK3568 PCIe YOLOv8 LPR demo. It does not modify
`src/main_lpr.cc`; the active Qt source files live in the parent project's
`src` directory and reuse the existing PCIe frame source and RKNN pipeline.

The visual layout is described in `../src/mainwindow.ui`, the Qt entry point
is `../src/main_pcie_qt.cc`, and reusable Qt helpers live in
`../src/pcie_qt_ui_helpers.*`. CMake compiles those files directly from `src`.
The video frame itself is already annotated by the existing C/C++ overlay code;
Qt only displays that annotated frame and the side-panel text/status fields.

Chinese UI text is rendered with `model/simhei.ttf`, copied from the reference
PyQt demo. `ppocr_keys_v1.txt` is an OCR dictionary, not a Qt display font.

The UI `Stop` button pauses the capture loop only. It does not kill the process,
close the PCIe source, or unload the driver. Press `Start` again to resume.
If manual termination is needed from ADB shell, use `kill -TERM $(pidof
yolov8_lpr_pcie_qt_ui)` first; reserve `kill -9` for a stuck process.

## Build on Ubuntu

The RK3568 Qt tutorial uses:

- Ubuntu 20.04 x86_64
- `gcc-aarch64-linux-gnu` and `g++-aarch64-linux-gnu`
- Qt 5.12.9 cross-built with `-xplatform linux-aarch64-gnu-g++`
- Qt ARM64 install prefix similar to `/home/ubuntu/Qt-5.12.9-arm64`

Preferred build, from the parent demo project:

```bash
cd /mnt/hgfs/3_NPU_Yolov8_PPOCR_Demo
sed -i 's/\r$//' build-linux.sh
chmod +x build-linux.sh

GCC_COMPILER=/usr/bin/aarch64-linux-gnu \
QT_ARM64_PREFIX=/home/gyn/Qt-5.12.9-arm64 \
./build-linux.sh -t rk3568 -a aarch64 -d yolov8_lpr -q
```

The `-q` option enables the optional Qt target. Without `-q`, the original
picture/video/PCIe demos build as before.

The old `qt_pcie_lpr_ui/build-qt-linux.sh` is now only a compatibility wrapper;
it delegates to the parent `build-linux.sh -q` flow so that there is a single
install tree.

CMake option for manual builds:

```bash
cmake -S .. -B build-qt-from-parent \
  -DENABLE_QT_UI=ON \
  -DCMAKE_PREFIX_PATH=/home/ubuntu/Qt-5.12.9-arm64 \
  -DQt5_DIR=/home/ubuntu/Qt-5.12.9-arm64/lib/cmake/Qt5
```

Output:

```text
install/rk356x_linux_aarch64/rknn_yolov8_lpr_demo/yolov8_lpr_pcie_qt_ui/
```

## Run on RK3568

Copy the installed directory to the board, then run:

```bash
chmod +x yolov8_lpr_pcie_qt_ui
./yolov8_lpr_pcie_qt_ui model/yolov8.rknn model/lprnet7repair_i8.rknn model/lprnet8repair_i8.rknn
```

On the current RK3568 image, run through X11/xcb:

```bash
export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0
export QT_QPA_PLATFORM=xcb
```

The UI expects the existing PCIe driver node and FPGA video stream to be working.
If startup fails, first check that the PCIe endpoint is enumerated and that the
driver module is loaded.
