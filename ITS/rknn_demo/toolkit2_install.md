# RKNN Toolkit2 安装与模型转换

## 1. 安装位置

RKNN Toolkit2 安装在 Ubuntu 虚拟机 / x86_64 PC 上，用于把 ONNX 转成 RKNN。

不要安装到 RK3568 板端。板端只运行 `.rknn`，使用 C/C++ runtime。

## 2. 推荐安装方式

以下命令在 Ubuntu 虚拟机执行。

```bash
sudo apt update
sudo apt install -y git git-lfs python3 python3-venv python3-pip
git lfs install
```

建议创建 Python 虚拟环境：

```bash
python3 -m venv ~/venvs/rknn-toolkit2
source ~/venvs/rknn-toolkit2/bin/activate
python -m pip install --upgrade pip setuptools wheel
```

克隆官方仓库：

```bash
cd ~
git clone https://github.com/rockchip-linux/rknn-toolkit2.git
cd rknn-toolkit2
```

查找适合当前 Python 版本的 wheel：

```bash
python -V
find packages -name "rknn_toolkit2*.whl" | sort
```

安装对应 wheel。示例：

```bash
pip install packages/*/rknn_toolkit2-*-cp38-*-linux_x86_64.whl
```

如果你的 Python 是 3.10，则选择 `cp310`：

```bash
pip install packages/*/rknn_toolkit2-*-cp310-*-linux_x86_64.whl
```

验证：

```bash
python -c "from rknn.api import RKNN; print('rknn toolkit2 ok')"
```

## 3. 如果 wheel 和 Python 版本不匹配

错误特征：

```text
not a supported wheel on this platform
```

处理方式：

1. 查看当前 Python 版本：

```bash
python -V
```

2. 查看 wheel 支持版本：

```bash
find ~/rknn-toolkit2/packages -name "rknn_toolkit2*.whl" | sort
```

3. 使用匹配的 Python 环境，例如 `cp38` 对应 Python 3.8，`cp310` 对应 Python 3.10。

## 4. 转换 YOLOv8n COCO ONNX

准备目录：

```bash
mkdir -p /mnt/hgfs/ITS/rknn_demo/model_convert
```

将 `yolov8n_coco.onnx` 和 `convert.py` 放入：

```text
/mnt/hgfs/ITS/rknn_demo/model_convert/
```

进入转换环境：

```bash
source ~/venvs/rknn-toolkit2/bin/activate
cd /mnt/hgfs/ITS/rknn_demo/model_convert
```

先转 FP 版本：

```bash
python3 convert.py ./yolov8n_coco.onnx rk3568 fp ./yolov8n_coco_fp.rknn
```

确认输出：

```bash
ls -lh ./yolov8n_coco_fp.rknn
```

复制到 benchmark 安装目录：

```bash
cp ./yolov8n_coco_fp.rknn \
  /mnt/hgfs/ITS/rknn_demo/install/rk356x_linux_aarch64/its_traffic_benchmark/yolov8_traffic_benchmark/model/
```

## 5. 后续 i8 量化

FP 版本跑通后，再做 i8 量化。i8 量化需要准备 `dataset.txt` 和量化图片。

```bash
python3 convert.py ./yolov8n_coco.onnx rk3568 i8 ./yolov8n_coco_i8.rknn
```

如果量化数据集路径不对，会在 build 阶段报错。第一轮建议先跑 FP。

