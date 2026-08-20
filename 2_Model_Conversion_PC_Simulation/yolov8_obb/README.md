# yolov8_obb

## Table of contents

- [1. Description](#1-description)
- [2. Current Support Platform](#2-current-support-platform)
- [3. Pretrained Model](#3-pretrained-model)
- [4. Convert to RKNN](#4-convert-to-rknn)
- [5. Python Demo](#5-python-demo)
- [6. Android Demo](#6-android-demo)
  - [6.1 Compile and Build](#61-compile-and-build)
  - [6.2 Push demo files to device](#62-push-demo-files-to-device)
  - [6.3 Run demo](#63-run-demo)
- [7. Linux Demo](#7-linux-demo)
  - [7.1 Compile \&\& Build](#71-compile-and-build)
  - [7.2 Push demo files to device](#72-push-demo-files-to-device)
  - [7.3 Run demo](#73-run-demo)
- [8. Expected Results](#8-expected-results)
- [9. Project ONNX validation record (2026-08-18)](#9-project-onnx-validation-record-2026-08-18)
- [10. Project conversion adaptation record (2026-08-18)](#10-project-conversion-adaptation-record-2026-08-18)



## 1. Description

The model used in this example comes from the following open source projects:  

https://github.com/airockchip/ultralytics_yolov8


yolov8n-obb pt model download link: 

[yolov8n-obb.pt](https://github.com/ultralytics/assets/releases/download/v8.2.0/yolov8n-obb.pt)


## 2. Current Support Platform

RK3566, RK3568, RK3588, RK3562, RV1109, RK3576, RV1126, RK1808, RK3399PRO



## 3. Pretrained Model

This project uses the epoch 54 four-class license-plate checkpoint in
`model/best.pt` and its RKNN-friendly raw-head export in `model/best.onnx`.
The classes are `blue`, `green`, `yellow_single` and `other`; the upstream DOTA
15-class model is not used by the project conversion path.



## 4. Convert to RKNN

Build the deterministic calibration set on Windows. The command selects 200
unique train images per class and creates NTFS hard links, so image pixel data is
not copied:

```powershell
cd python
python build_quant_dataset.py --per-class 200
```

Generated reproducibility files are kept under `model/`:

- `obb_quant_dataset_800.txt`: Toolkit2 input list.
- `obb_quant_manifest_800.tsv`: source, class, size and angle audit.
- `obb_quant_summary_800.json`: distribution and hard-link summary.
- `obb_quant_dataset_800/`: 800 ignored hard-link directory entries.

Run the conversion preflight on either Windows or Ubuntu without importing
RKNN Toolkit2:

```shell
python convert.py ../model/best.onnx rk3568 i8 \
  ../model/best_rk3568_i8.rknn \
  --dataset ../model/obb_quant_dataset_800.txt \
  --check-only
```

Build the FP16 baseline and INT8 model in the Ubuntu RKNN Toolkit2 2.3.0
environment:

```shell
python convert.py ../model/best.onnx rk3568 fp \
  ../model/best_rk3568_fp16.rknn

python convert.py ../model/best.onnx rk3568 i8 \
  ../model/best_rk3568_i8.rknn \
  --dataset ../model/obb_quant_dataset_800.txt
```

`convert.py` rejects dynamic or non-project ONNX signatures. The expected fixed
signature is `[1,3,640,640]` with three 68-channel branches and one 8400-element
angle branch. The calibration list is validated only for quantized builds.



## 5. Python Demo

The demo supports the Rockchip raw-head `.pt`, `.onnx` and `.rknn` models and
uses four-class DFL decoding plus rotated-IoU NMS:

```shell
cd python
python yolov8_obb.py \
  --model-path ../model/best.onnx \
  --image-path ../model/obb_quant_dataset_800/quant_0000.jpg \
  --output-path result/onnx_result.jpg

python yolov8_obb.py \
  --model-path ../model/best_rk3568_i8.rknn \
  --image-path ../model/obb_quant_dataset_800/quant_0000.jpg \
  --output-path result/rknn_result.jpg \
  --target rk3568
```

For `.pt` raw-head inference, place the local `ultralytics_yolov8` fork on
`PYTHONPATH`. ONNX Runtime inference does not require RKNN Toolkit2. RKNN
inference requires Toolkit2 and a connected target when `--target` is used.



## 6. Android Demo

#### 6.1 Compile and Build

*Usage:*

```sh
# go back to the rknn_model_zoo root directory
cd ../../
export ANDROID_NDK_PATH=<android_ndk_path>

./build-android.sh -t <TARGET_PLATFORM> -a <ARCH> -d yolov8_obb

# such as 
./build-android.sh -t rk3588 -a arm64-v8a -d yolov8_obb
```

*Description:*
- `<android_ndk_path>`: Specify Android NDK path.
- `<TARGET_PLATFORM>`: Specify NPU platform name. Support Platform refer [here](#2-current-support-platform).
- `<ARCH>`: Specify device system architecture. To query device architecture, refer to the following command:
	```shell
	# Query architecture. For Android, ['arm64-v8a' or 'armeabi-v7a'] should shown in log.
	adb shell cat /proc/version
	```

#### 6.2 Push demo files to device

With device connected via USB port, push demo files to devices:

```shell
adb root
adb remount
adb push install/<TARGET_PLATFORM>_android_<ARCH>/rknn_yolov8_obb_demo/ /data/
```

#### 6.3 Run demo

```sh
adb shell
cd /data/rknn_yolov8_obb_demo

export LD_LIBRARY_PATH=./lib
./rknn_yolov8_obb_demo model/yolov8n-obb.rknn model/test.jpg
```

- After running, the result was saved as `out.png`. To check the result on host PC, pull back result referring to the following command: 

  ```sh
  adb pull /data/rknn_yolov8_obb_demo/out.png
  ```



## 7. Linux Demo

#### 7.1 Compile and Build

*usage*

```shell
# go back to the rknn_model_zoo root directory
cd ../../

# if GCC_COMPILER not found while building, please set GCC_COMPILER path
(optional)export GCC_COMPILER=<GCC_COMPILER_PATH>

./build-linux.sh -t <TARGET_PLATFORM> -a <ARCH> -d yolov8_obb

# such as 
./build-linux.sh -t rk3588 -a aarch64 -d yolov8_obb
```

*Description:*

- `<GCC_COMPILER_PATH>`: Specified as GCC_COMPILER path.
    ```sh
    export GCC_COMPILER=~/opt/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf
    ```
- `<TARGET_PLATFORM>` : Specify NPU platform name. Support Platform refer [here](#2-current-support-platform).
- `<ARCH>`: Specify device system architecture. To query device architecture, refer to the following command: 
  
  ```shell
  # Query architecture. For Linux, ['aarch64' or 'armhf'] should shown in log.
  adb shell cat /proc/version
  ```

#### 7.2 Push demo files to device

- If device connected via USB port, push demo files to devices:

```shell
adb push install/<TARGET_PLATFORM>_linux_<ARCH>/rknn_yolov8_obb_demo/ /userdata/
```

- For other boards, use `scp` or other approaches to push all files under `install/<TARGET_PLATFORM>_linux_<ARCH>/rknn_yolov8_obb_demo/` to `userdata`.

#### 7.3 Run demo

```sh
adb shell
cd /userdata/rknn_yolov8_obb_demo

export LD_LIBRARY_PATH=./lib
./rknn_yolov8_obb_demo model/yolov8n-obb.rknn model/test.jpg
```

- After running, the result was saved as `out.png`. To check the result on host PC, pull back result referring to the following command: 

  ```
  adb pull /userdata/rknn_yolov8_obb_demo/out.png
  ```



## 8. Expected Results

This example will print the labels and corresponding scores of the test image detect results, as follows:
```
ship @ (172 757 42 43 angle=0.001677) 0.858
ship @ (582 361 64 62 angle=0.207816) 0.858
ship @ (521 489 37 40 angle=0.067267) 0.858
ship @ (187 627 89 84 angle=0.348365) 0.858
...
ship @ (631 224 15 13 angle=0.404585) 0.599
ship @ (459 96 16 13 angle=0.601354) 0.589
ship @ (482 121 13 11 angle=0.666943) 0.589
ship @ (494 136 18 13 angle=0.741903) 0.500

```
<img src="python/result.jpg">

<br>
- Note: Different platforms, different versions of tools and drivers may have slightly different results.

## 9. Project ONNX validation record (2026-08-18)

The project-specific four-class license-plate OBB checkpoint and its RKNN-optimized
ONNX export are stored under `model/`:

- `best.pt`: epoch 54 final/best checkpoint, SHA-256
  `559EEFAD166B618D7C032F05E5C92942DDEC0268D0F5ED15D829DC1050084A0A`.
- `best.onnx`: Rockchip raw-head export, 12,359,048 bytes, SHA-256
  `03F658613654594BE9C9779AE52D4F32A9D090EE82118E466CAF199BAAA28846`.

Validation was run with the local Rockchip Ultralytics fork 8.2.82, PyTorch 2.1.0,
ONNX and ONNX Runtime. `onnx.checker.check_model()` passed. The graph uses ONNX
opset 12, IR version 7, 243 nodes and 144 initializers. Its input and outputs are
fixed-shape FP32 tensors:

| Tensor | Shape | Meaning |
|---|---|---|
| `images` | `[1,3,640,640]` | normalized RGB input |
| output 0 | `[1,68,80,80]` | stride-8, 64 DFL + 4 classes |
| output 1 | `[1,68,40,40]` | stride-16, 64 DFL + 4 classes |
| output 2 | `[1,68,20,20]` | stride-32, 64 DFL + 4 classes |
| output 3 | `[1,1,8400]` | concatenated sigmoid angle feature |

The ONNX graph contains only `Add`, `Concat`, `Constant`, `Conv`, `MaxPool`,
`Mul`, `Reshape`, `Resize`, `Sigmoid` and `Split`; DFL decoding, trigonometry,
rotated NMS and perspective correction remain outside the model as required by
the Rockchip deployment path.

A real test image was letterboxed to 640 with fill value 114 and evaluated through
both the fused PyTorch RKNN export head and ONNX Runtime. All four output shapes
matched. Per-output mean absolute errors were `4.03e-6`, `4.35e-6`, `5.74e-6`
and `2.78e-7`; every output passed `numpy.allclose(rtol=1e-4, atol=1e-4)`.
The angle feature remained in `[0,1]` and is decoded outside the model as
`(angle_feature - 0.25) * pi`.

Conclusion: `best.onnx` is a valid fixed-shape RKNN-friendly OBB raw-head model
and is ready for RKNN Toolkit2 2.3.0 loading. The project calibration and
four-class postprocessing adaptations are recorded in the next section.

## 10. Project conversion adaptation record (2026-08-18)

The sample conversion and inference scripts were replaced with project-specific
implementations:

- `python/convert.py` validates opset 12 and the exact four-output OBB signature,
  validates quantization lists only for INT8/UINT8 builds, supports `--check-only`,
  uses RK3568-compatible mean/std normalization and no longer references COCO.
- `python/yolov8_obb.py` supports PT raw-head, ONNX Runtime and RKNN backends,
  four project classes, 68-channel branches, stable sigmoid/DFL decoding,
  rotated-IoU NMS, letterbox restoration and polygon rendering.
- `python/build_quant_dataset.py` selects unique `train` split source images with
  deterministic source, plate-size, tilt-angle and multi-plate stratification.

The generated calibration set uses 800 NTFS hard links, 200 primary selections
per class. No image pixel data was copied. All 800 link/source pairs passed
`os.path.samefile()` and all 800 list entries resolve to unique files. The first
sample has an NTFS link count of 3 because the source image, frozen YOLO train
dataset entry and calibration entry share the same underlying file.

| Primary class | Selected | Images containing class |
|---|---:|---:|
| `blue` | 200 | 337 |
| `green` | 200 | 200 |
| `yellow_single` | 200 | 202 |
| `other` | 200 | 200 |

Sources comprise 163 CCPD2019, 200 CCPD2020, 351 CRPD and 86 special-plate
images; 162 selections contain multiple plates. The detailed selection and
geometry audit is stored in `model/obb_quant_manifest_800.tsv`, and the compact
distribution record is stored in `model/obb_quant_summary_800.json`.

Both FP16 and INT8 `convert.py --check-only` preflights passed. ONNX inference
was tested on one primary sample from every class. The decoded class, confidence,
angle and polygon matched the PyTorch OBB result; on the blue sample, ONNX
produced confidence 0.933 and corners within one pixel of Ultralytics prediction.

### RK3568 INT8 build result

The formal INT8 conversion completed successfully in the Ubuntu `toolkit230`
environment with RKNN Toolkit2 2.3.0. All 800 calibration hard links were
validated, graph preparation completed, all 10 quantization stages completed,
and `rknn.build()` plus `rknn.export_rknn()` returned successfully.

- Output: `model/yolov8_obb_i8.rknn` (renamed from the original conversion output)
- Size: 4,361,505 bytes
- SHA-256: `669737431961C93DD12E27CDECABB1EB680966E4F9EB6E9A2DE28E4C5E532950`
- Build time stamp: 2026-08-18 21:33:35 Asia/Shanghai

Toolkit2 changed the model-native input and all four model-native outputs from
FP32 to INT8 for performance. This is expected for the W8A8 build. Runtime code
must query tensor attributes instead of assuming FP32. For the first accuracy
test, request floating outputs (`want_float=1`) or dequantize every output with
its queried zero point and scale. In particular, the angle tensor must be
dequantized before applying `(angle_feature - 0.25) * pi`. Raw RGB image input
must not be divided by 255 a second time when Runtime performs the embedded
mean/std preprocessing with `pass_through=0`.

A separate native RK3568 timing harness is provided in
`../../6_Temporary_OBB_Perf_Demo`. It compares `RKNN_LOG_LEVEL=0` and `4` with
identical input, warmup and repeat counts. The harness now uses zero-copy native
input/output memory, unpacks NC1HWC2 tensors, runs four-class OBB decode plus
rotated NMS, and produces the BGR `48x160` image buffers immediately before
PP-OCR inference.
