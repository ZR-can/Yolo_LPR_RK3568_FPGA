# CBLPRD PP-OCRv4 车牌识别训练说明

本文记录当前 Linux + RTX 4090 环境下，从官方 `ch_PP-OCRv4_rec` 权重重新开始训练车牌专用识别模型的配置与命令。

## 当前训练规格

- 系统：Ubuntu 22.04，Python 3.10，CUDA 11.8
- GPU：RTX 4090 24GB × 1
- CPU / 内存：20 vCPU / 90GB
- 基础模型：`ch_PP-OCRv4_rec`
- 网络：PPLCNetV3 0.95 + SVTR + CTC/NRTR MultiHead
- 训练输入：`[N, 3, 48, 160]`
- 部署分支：CTC
- CTC 输出：`[N, 20, 74]`
- 字典：73 个实际字符，加 1 个 CTC blank
- 标签长度：7～8；NRTR `max_text_length` 为 10
- 部署目标：RK3568 RKNN

配置文件：

```text
configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160.yml
```

训练输出：

```text
/root/autodl-tmp/paddleocr_output/cblprd_ppocrv4_rec_48x160_linux4090
```

## 数据集清单

当前数据集共 268,549 张：训练 251,192 张，验证 17,357 张。

| 子数据集 |   train |   val |
| -------- | ------: | ----: |
| basic    | 188,861 | 9,897 |
| hard     |  41,532 | 2,260 |
| 使       |   4,527 | 1,132 |
| 学       |   6,082 | 1,521 |
| 港       |   2,626 |   656 |
| 澳       |   2,618 |   654 |
| 警       |     174 |    44 |
| 领       |   4,772 | 1,193 |

`build_PPOCRv4_cblprd_manifests.py` 针对当前“外层清单、内层数据”的结构，在数据集外层根目录生成：

- `train_all.txt`、`val_all.txt`：全量合并清单，便于统一审计。
- `train_basic.txt`、`train_hard.txt`、`train_special_*.txt`：训练配置使用的 8 份清单。
- 对应的 8 份 `val_*.txt`：完整验证集清单。

当前目录契约为：

```text
外层数据集根目录/
├── train_*.txt、val_*.txt、train_all.txt、val_all.txt
└── CBLPRD/
    ├── basic/{train,val,train.txt,val.txt}
    ├── hard/{train,val,train.txt,val.txt}
    └── special/{使,学,港,澳,警,领}/{train,val,train.txt,val.txt}
```

脚本会双向检查每个子集：标签引用的图片必须存在，图片目录中的每张 JPG 也必须有且只有一条标签。外层的 `CBLPRD.7z` 等文件不参与清单生成。

每行都是严格的 PP-OCR TSV：

```text
CBLPRD/basic/train/000000001.jpg<TAB>粤A12345
```

需要重新生成时，先预演，再写入：

```bash
python 1_PC_Training/scripts/build_PPOCRv4_cblprd_manifests.py
python 1_PC_Training/scripts/build_PPOCRv4_cblprd_manifests.py --apply
```

Linux 上若需要对上传后的同结构目录重建，可显式指定：

```bash
python 1_PC_Training/scripts/build_PPOCRv4_cblprd_manifests.py \
  --dataset-root /root/autodl-tmp/CBLPRD \
  --apply
```

脚本会检查图片存在性、重复路径、7～8 位标签和 73 字符字典。

## 多数据集比例

PaddleOCR release/2.7 的 `SimpleDataSet` 支持多个 `label_file_list`，并由等长的 `ratio_list` 控制每份标签文件的保留比例。

第一阶段基线训练使用：

```yaml
ratio_list: [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
```

顺序依次是 `basic、hard、使、学、港、澳、警、领`。全部使用 1.0 时，每个 epoch 使用完整的 251,192 张训练图片，与将所有标签合并到单个 `train_all.txt` 的数据分布完全相同。保留多清单结构是为了以后能独立调整比例和统计子集，不会造成重复读取。

`ratio_list` 是“各文件保留比例”，不是 batch 混合权重。训练阶段不要填写大于 1 的值，否则 `random.sample` 会报错。上一轮收敛异常尚未建立稳定基线，因此当前不下采样 basic。只有完整基线正常而 special 指标明显较差时，才进行降低 basic/hard 比例的第二阶段对照实验。验证集始终全部使用 1.0。

## Linux 数据放置

代码位于 `/root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR`，数据集位于 AutoDL 数据盘 `/root/autodl-tmp/CBLPRD`。训练配置直接使用数据集绝对路径，不需要建立 `train_data` 软链接。

数据集根目录应同时包含 `train_basic.txt` 和 `CBLPRD/`。检查：

```bash
test -f /root/autodl-tmp/CBLPRD/train_basic.txt
test -f /root/autodl-tmp/CBLPRD/CBLPRD/basic/train.txt
wc -l /root/autodl-tmp/CBLPRD/train_*.txt
wc -l /root/autodl-tmp/CBLPRD/val_*.txt
```

注意：`train_*.txt` 会同时匹配 `train_all.txt`，因此 `wc` 展示的总数会包含一份额外的全量清单；训练配置只列出 8 份子集清单，不会重复训练。

## Windows 本地数据放置

从 `1_PC_Training/PaddleOCR` 目录启动训练或评估时，使用 `configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160_local.yml`。该配置将训练和验证的 `data_dir` 设为相对路径 `../datasets/CBLPRD-330k`，并从同一目录读取 `train_basic.txt`、`train_hard.txt`、六份 `train_special_*.txt` 及对应验证清单。标注中的 `CBLPRD/...` 图片相对路径由该 `data_dir` 正确解析。

## 预训练权重

配置要求以下文件存在：

```text
pretrain_models/ch_PP-OCRv4_rec_train/student.pdparams
```

`Global.pretrained_model` 填写不带 `.pdparams` 的前缀：

```yaml
pretrained_model: ./pretrain_models/ch_PP-OCRv4_rec_train/student
checkpoints:
```

`checkpoints` 必须为空，才是从官方权重进行新的微调实验。不要从旧实验的 `best_accuracy` 恢复优化器状态。

## 环境检查

进入用于 PaddleOCR 的 Conda 环境后执行：

```bash
cd /path/to/PaddleOCR
python -c "import paddle; print(paddle.__version__); print(paddle.device.cuda.device_count()); paddle.utils.run_check()"
nvidia-smi
df -h /dev/shm
```

应检测到 1 张 CUDA GPU。如果 `/dev/shm` 很小并出现 DataLoader bus error，训练时追加：

```text
-o Train.loader.use_shared_memory=false Eval.loader.use_shared_memory=false
```

## 首次小规模试跑

先用 1% 数据检查路径、显存、前向、反向和验证是否完整：

```bash
export CUDA_VISIBLE_DEVICES=0
python tools/train.py \
  -c configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160.yml \
  -o Global.epoch_num=1 \
     Global.save_model_dir=/root/autodl-tmp/paddleocr_output/cblprd_ppocrv4_rec_48x160_smoke \
     'Train.dataset.ratio_list=[0.01,0.01,0.01,0.01,0.01,0.01,0.01,0.01]'
```

日志应明确显示加载官方 `student` 权重、训练 loss 为有限值，并完成一次完整验证。当前配置默认 batch size 为 256；若显存不足，可先仅将训练 batch 降到 192：

```text
-o Train.loader.batch_size_per_card=192
```

## 开始全量训练

```bash
cd /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR
export CUDA_VISIBLE_DEVICES=0
python tools/train.py -c configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160.yml
```

当前关键参数：

- batch size：256
- DataLoader workers：训练 12，验证 8
- AMP：O1 + FP16 动态 loss scaling，初始 scale 为 1024
- 优化器：Adam，L2 factor 为 `3e-5`
- 初始学习率：`3e-4`
- warmup：3 epochs
- 调度：Cosine
- 梯度裁剪：global norm 5.0
- epochs：50
- 随机种子：`20260717`
- 每个 epoch 完整验证并更新 `latest`，每 5 个 epochs 额外保存一次编号 checkpoint
- 车牌裁剪图采用轻量增强，不使用通用 OCR 的 0.4 全概率强增强

按当前 251,192 张训练集、17,357 张验证集和 batch size 256 计算，每个 epoch 为 981 个训练 step、68 个验证 step。首个 epoch 的准确率可能受新分类头初始化影响；应重点观察前 3～5 个 epoch 的 `acc`、`norm_edit_dis` 和 loss 趋势。

`best_accuracy` 会自动保留全程最优权重。若验证指标连续多个 epoch 明显下降，不要反复从 `best_accuracy` 继续训练；先检查学习率、标签与输入，再从官方权重开始新的对照实验。

## 正确续训

只有同一次实验中断后才从 `latest` 续训：

```bash
python tools/train.py \
  -c configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160.yml \
  -o Global.checkpoints=/root/autodl-tmp/paddleocr_output/cblprd_ppocrv4_rec_48x160_linux4090/latest
```

这里同样不带 `.pdparams` 后缀。`latest` 用于恢复模型、优化器和学习率状态；`best_accuracy` 用于最终选择和导出，不作为常规续训点。

## CTC SVTR + FC 微调（2026-07-18）

配置文件：

```text
configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160_ctc_svtr_fc.yml
```

该配置从原始 `student_cblprd73` 重新开始，只解冻 `head.ctc_encoder.*` 和 `head.ctc_head.fc.*`。CTC SVTR 内部的 BN affine 参数仍保持冻结，全部 BN 使用已加载的全局均值和方差；Backbone 与 NRTR 参数不进入优化器。`skip_gtc_train: true` 会跳过 NRTR 训练前向，零权重 NRTR loss 也不会计算。训练启动日志会逐项打印可训练参数，若配置前缀没有匹配任何参数则直接报错。

首次实验使用 FP32、Adam、常量学习率 `3e-6`、3 epochs，并保持 `use_guide: true`。此阶段只训练 CTC SVTR 与 FC，不需要把 CTC 梯度传回 Backbone。

```bash
cd /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR
export CUDA_VISIBLE_DEVICES=0
python tools/train.py \
  -c configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160_ctc_svtr_fc.yml
```

需要做 `1e-6`、`3e-6`、`1e-5` 三档独立对照时，每次都从 `student_cblprd73` 开始，并使用不同输出目录：

```bash
cd /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR
export CUDA_VISIBLE_DEVICES=0
for lr in 1e-6 3e-6 1e-5; do
  tag=${lr/-/m}
  python tools/train.py \
    -c configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160_ctc_svtr_fc.yml \
    -o Optimizer.lr.learning_rate=${lr} \
       Global.save_model_dir=/root/autodl-tmp/paddleocr_output/ctc_svtr_fc_lr${tag}_20260718
done
```

使用集中评估脚本分别测试 8 个验证子集；checkpoint 参数是不带 `.pdparams` 的前缀：

```bash
bash scripts/eval_ppocr_cblprd_subsets.sh \
  configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160_ctc_svtr_fc.yml \
  /root/autodl-tmp/paddleocr_output/ctc_svtr_fc_lr3em6_20260718/best_accuracy
```

脚本集中显示 `subset / samples / correct / accuracy` 四列。代码和配置已在 Windows 工作区更新；Linux 服务器训练、checkpoint 生成及结果判断仍需在服务器上执行。

### 首轮服务器实测结果

2026-07-18 在 RTX 4090 服务器上完成 `3e-6`、FP32 的 CTC SVTR + FC 实验。训练日志确认只开放 33 个 tensor、573,434 个参数，151 个 BN 使用冻结的全局统计，NRTR 前向和 loss 均已关闭。

最佳 checkpoint 为：

```text
/root/autodl-tmp/paddleocr_output/ctc_svtr_fc_lr3em6_20260718/best_accuracy
```

该 checkpoint 出现在第 1 个 epoch 的 `global_step=300`，联合验证集指标为 `acc=88.4081%`、`norm_edit_dis=0.982215`。分子集结果为：

| 子集  | samples | correct | accuracy |
| ----- | ------: | ------: | -------: |
| basic |   9,897 |   9,078 | 91.7248% |
| hard  |   2,260 |   1,513 | 66.9469% |
| 使    |   1,132 |   1,122 | 99.1166% |
| 学    |   1,521 |   1,263 | 83.0375% |
| 港    |     656 |     591 | 90.0915% |
| 澳    |     654 |     581 | 88.8379% |
| 警    |      44 |      26 | 59.0909% |
| 领    |   1,193 |   1,171 | 98.1559% |
| 合计  |  17,357 |  15,345 | 88.4081% |

### GA 36 规则修正版验证

`PlateRuleRecMetric` 复用数据审计脚本的 `ga36_plate_type_io_v2` 规则：先删除预测中的 `·` 和全部空白；仅对 7/8 位且首位为省级行政区简称的预测执行位置修正。`省份简称 + 数字区 + 领`的整个中间区域均按数字位处理，执行 `O/I -> 0/1`；以`警`结尾时保留第二位原字符，只修正第三位及以后的 `O/I`；其余号牌仍将第二位的 `0/1` 修正为 `O/I`，并将第三位及以后的 `O/I` 修正为 `0/1`。标签本身不做规则改写。

2026-07-23 当前源码已升级为 `ga36_plate_type_v3`：使馆牌按无省份前缀的数字结构执行 `O/I -> 0/1`；领馆牌继续按数字机构编号处理；普通、警、学、港、澳牌第二位统一作为 `A-Z` 发牌机关代号，`0/1 -> O/I`，不再保留警牌数字第二位。下面已经记录的准确率均来自 v2，不能标记为 v3 结果，需重新执行服务器评估。

该 Metric 的 `acc` 与 `rule_acc` 都表示规则修正后的准确率，同时输出：

- `raw_acc`：标准验证准确率，不执行规则修正。
- `rule_changed_num`：规则改变过的预测数。
- `rule_fixed_num`：由错误修正为正确的预测数。
- `rule_harmed_num`：由正确改成错误的预测数，用于发现规则误伤。

2026-07-18 首次服务器回放旧 `ga36_position_io_v1` 时，联合准确率由 `raw_acc=88.4081%` 提升到 `89.6699%`，但领牌从 `98.1559%` 降到 `80.7209%`、警牌下降2条。核对本地标签后确认：1,193条领牌全部为`省份简称 + 5位数字 + 领`，其中218条第二位为`0/1`；旧规则误伤了其中原本识别正确的208条。警牌另有4条GT第二位为`0`，其中2条被旧规则误伤。升级到 `ga36_plate_type_io_v2` 后，本地全部训练及验证清单共268,549条GT经过规则处理均保持不变，因此原始正确预测不会再被该规则改错；按旧日志计算，新规则准确率的理论下限为 `90.8798%`，实际值需在服务器重新评估。

### `student_cblprd73` 原始基线

2026-07-21 使用未经本轮 CTC SVTR + FC 微调的 `student_cblprd73`，在同一份 17,357 张 CBLPRD 验证集上执行 `PlateRuleRecMetric` 分子集评估。checkpoint 参数使用不带 `.pdparams` 的前缀：

```bash
cd /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR
bash scripts/eval_ppocr_cblprd_subsets.sh \
  configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160_ctc_svtr_fc.yml \
  ./pretrain_models/ch_PP-OCRv4_rec_train/student_cblprd73 \
  /root/autodl-tmp/CBLPRD \
  PlateRuleRecMetric
```

| subset | samples | raw_accuracy | rule_accuracy | fixed | harmed |
|--------|--------:|-------------:|--------------:|------:|-------:|
| basic | 9897 | 90.7548% | 96.0089% | 520 | 0 |
| hard | 2260 | 51.9912% | 57.7434% | 130 | 0 |
| 使 | 1132 | 99.7350% | 99.7350% | 0 | 0 |
| 学 | 1521 | 77.0546% | 86.5220% | 144 | 0 |
| 港 | 656 | 87.1951% | 97.4085% | 67 | 0 |
| 澳 | 654 | 85.0153% | 98.0122% | 85 | 0 |
| 警 | 44 | 52.2727% | 54.5454% | 1 | 0 |
| 领 | 1193 | 98.6588% | 98.6588% | 0 | 0 |
| **TOTAL** | **17357** | **85.1875%** | **90.6435%** | **947** | **0** |

TOTAL 根据各子集的样本数、原始正确数和 `fixed` 汇总：原始正确 14,786 张，规则修正后正确 15,733 张。规则没有误伤，但原始模型在 hard 子集仅为 51.9912%，是该基线最明显的短板。

### CTC SVTR + FC 微调 checkpoint

对单个 checkpoint 执行全量规则版验证：

```bash
cd /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR
python tools/eval.py \
  -c configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160_ctc_svtr_fc.yml \
  -o Global.pretrained_model=null \
     Global.checkpoints=/root/autodl-tmp/paddleocr_output/ctc_svtr_fc_lr3em6_20260718/best_accuracy \
     Metric.name=PlateRuleRecMetric
```

分 8 个子集集中显示原始准确率、规则准确率和修正数量：

```bash
bash scripts/eval_ppocr_cblprd_subsets.sh \
  configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160_ctc_svtr_fc.yml \
  /root/autodl-tmp/paddleocr_output/ctc_svtr_fc_lr3em6_20260718/best_accuracy \
  /root/autodl-tmp/CBLPRD \
  PlateRuleRecMetric
```

| subset | samples | raw_accuracy | rule_accuracy | fixed | harmed |
|--------|---------|--------------|---------------|-------|--------|
| basic | 9897 | 91.7248% | 93.7456% | 200 | 0 |
| hard | 2260 | 66.9469% | 70.9735% | 91 | 0 |
| 使 | 1132 | 99.1166% | 99.1166% | 0 | 0 |
| 学 | 1521 | 83.0375% | 87.1137% | 62 | 0 |
| 港 | 656 | 90.0915% | 95.1219% | 33 | 0 |
| 澳 | 654 | 88.8379% | 95.4128% | 43 | 0 |
| 警 | 44 | 59.0909% | 59.0909% | 0 | 0 |
| 领 | 1193 | 98.1559% | 98.1559% | 0 | 0 |

若训练期间也要按规则修正后的 `acc` 选择 `best_accuracy`，在训练命令末尾增加：

```text
-o Metric.name=PlateRuleRecMetric
```

标准训练配置仍保留 `Metric.name=RecMetric`，因此不会静默改变此前实验的 checkpoint 选择口径。规则版属于单独的部署后处理评估口径，必须同时保留 `raw_acc` 作为模型本体能力基线。

## 单独评估与导出最佳模型

使用训练期间保存的 `best_accuracy` 做完整验证：

```bash
python tools/eval.py \
  -c configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160.yml \
  -o Global.checkpoints=/root/autodl-tmp/paddleocr_output/cblprd_ppocrv4_rec_48x160_linux4090/best_accuracy
```

导出只保留 CTC 分支的 Paddle 推理模型：

```bash
python tools/export_model.py \
  -c configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160.yml \
  -o Global.pretrained_model=/root/autodl-tmp/paddleocr_output/cblprd_ppocrv4_rec_48x160_linux4090/best_accuracy \
     Global.save_inference_dir=/root/autodl-tmp/paddleocr_output/cblprd_ppocrv4_rec_48x160_linux4090/inference
```

### CTC SVTR + FC 三份 ONNX 规则版分子集对照评估

本节对照同一 CTC SVTR + FC 模型的三份 ONNX：

| 对照模型 | 来源与关系 |
| -------- | ---------- |
| `ppocrv4_rec.onnx` | 基础导出模型；历史文件名 `ppocrv4_rec_fixed.onnx` 与其表示同一份模型。 |
| `ppocrv4_rec14.onnx` | 原始 CTC SVTR + FC 使用 opset 14 导出的 ONNX。 |
| `ppocrv4_rec14_fold_affine_1x1.onnx` | 在 opset 14 模型上折叠 LearnableAffine：将其缩放和平移吸收到后续 1×1 Conv 的权重和 bias 中，再删除对应的 Mul/Add 节点。 |

`scripts/eval_onnx_ppocr_cblprd_subsets.py`直接评估固定输入`[1,3,48,160]`、输出`[1,20,74]`的上述 CTC ONNX。预处理与PaddleOCR的`RecResizeImg`保持一致：读取BGR图像、按高度48等比例缩放、宽度向上取整、右侧补零到160，再归一化到`[-1,1]`。解码使用73字符字典，CTC blank为索引0，先删除连续重复再删除blank；随后加载 `plate_rule.py` 当前声明的规则版本，现为 `ga36_plate_type_v3`。

在服务器PaddleOCR目录运行：

```bash
for model in \
  ppocrv4_rec \
  ppocrv4_rec14 \
  ppocrv4_rec14_fold_affine_1x1; do
  python scripts/eval_onnx_ppocr_cblprd_subsets.py \
    --model "/root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/onnx/${model}.onnx" \
    --manifest-dir /root/autodl-tmp/CBLPRD \
    --data-dir /root/autodl-tmp \
    --provider auto \
    --save-details "/root/autodl-tmp/paddleocr_output/onnx/${model}_onnx_val_details.tsv"
done
```

`ppocrv4_rec.onnx` 与 `ppocrv4_rec14.onnx` 的分子集结果均为：

| subset | samples | raw_correct | raw_accuracy | rule_correct | rule_accuracy | changed | fixed | harmed |
|--------|---------|-------------|--------------|--------------|---------------|---------|-------|--------|
| basic | 9897 | 9083 | 91.7753% | 9282 | 93.7860% | 228 | 199 | 0 |
| hard | 2260 | 1507 | 66.6814% | 1597 | 70.6637% | 113 | 90 | 0 |
| 使 | 1132 | 1120 | 98.9399% | 1120 | 98.9399% | 0 | 0 | 0 |
| 学 | 1521 | 1263 | 83.0375% | 1325 | 87.1137% | 62 | 62 | 0 |
| 港 | 656 | 592 | 90.2439% | 625 | 95.2744% | 34 | 33 | 0 |
| 澳 | 654 | 581 | 88.8379% | 624 | 95.4128% | 43 | 43 | 0 |
| 警 | 44 | 26 | 59.0909% | 26 | 59.0909% | 0 | 0 | 0 |
| 领 | 1193 | 1171 | 98.1559% | 1171 | 98.1559% | 0 | 0 | 0 |
| TOTAL | 17357 | 15343 | 88.3966% | 15770 | 90.8567% | 480 | 427 | 0 |

#### 2026-07-21 ONNX 变体全量对比

在 RTX 4090 服务器上使用 `CUDAExecutionProvider`、同一份 17,357 张验证集和当时的 `ga36_plate_type_io_v2` 规则完成三份 ONNX 的全量评估。`ppocrv4_rec14.onnx` 与 `ppocrv4_rec.onnx` 的所有分子集统计完全一致，也与上表结果一致；`ppocrv4_rec14_fold_affine_1x1.onnx` 的汇总结果如下：

| ONNX 模型 | samples | raw_correct | raw_accuracy | rule_correct | rule_accuracy | changed | fixed | harmed |
| ---------- | ------: | ----------: | -----------: | -----------: | ------------: | ------: | ----: | -----: |
| `ppocrv4_rec14.onnx` | 17,357 | 15,343 | 88.3966% | 15,770 | 90.8567% | 480 | 427 | 0 |
| `ppocrv4_rec.onnx` | 17,357 | 15,343 | 88.3966% | 15,770 | 90.8567% | 480 | 427 | 0 |
| `ppocrv4_rec14_fold_affine_1x1.onnx` | 17,357 | 15,347 | 88.4197% | 15,774 | 90.8798% | 481 | 427 | 0 |

`ppocrv4_rec14_fold_affine_1x1.onnx` 的分子集明细为：

| subset | samples | raw_correct | raw_accuracy | rule_correct | rule_accuracy | changed | fixed | harmed |
| ------ | ------: | ----------: | -----------: | -----------: | ------------: | ------: | ----: | -----: |
| basic | 9,897 | 9,084 | 91.7854% | 9,283 | 93.7961% | 228 | 199 | 0 |
| hard | 2,260 | 1,509 | 66.7699% | 1,599 | 70.7522% | 114 | 90 | 0 |
| 使 | 1,132 | 1,121 | 99.0283% | 1,121 | 99.0283% | 0 | 0 | 0 |
| 学 | 1,521 | 1,263 | 83.0375% | 1,325 | 87.1137% | 62 | 62 | 0 |
| 港 | 656 | 592 | 90.2439% | 625 | 95.2744% | 34 | 33 | 0 |
| 澳 | 654 | 581 | 88.8379% | 624 | 95.4128% | 43 | 43 | 0 |
| 警 | 44 | 26 | 59.0909% | 26 | 59.0909% | 0 | 0 | 0 |
| 领 | 1,193 | 1,171 | 98.1559% | 1,171 | 98.1559% | 0 | 0 | 0 |
| TOTAL | 17,357 | 15,347 | 88.4197% | 15,774 | 90.8798% | 481 | 427 | 0 |

相对另外两份 ONNX，折叠版本的原始正确数和规则后正确数均增加 4，准确率均提高 `0.0231` 个百分点；增量来自 basic `+1`、hard `+2`、使牌 `+1`，其余分子集正确数不变。三份模型的规则误伤数均为 0，规则净修复数均为 427。


`--provider auto`在安装`onnxruntime-gpu`且CUDA Provider可用时优先使用GPU，否则使用CPU。脚本按basic、hard、使、学、港、澳、警、领及TOTAL集中输出原始正确数/准确率、规则正确数/准确率、规则改动数、修复数和误伤数。`onnx_val_details.tsv`记录每张图片的GT、原始预测、规则预测、置信度及两种正确性，便于核对Paddle与ONNX差异。

脚本同时支持放在`1_PC_Training/scripts/`和`1_PC_Training/PaddleOCR/scripts/`：启动时会检查当前位置是否已经是PaddleOCR根目录，否则再检查同级`PaddleOCR/`，并据此定位字符字典与`plate_rule.py`，避免重复拼接`PaddleOCR/PaddleOCR`。

#### yolo_lprnet_crops 图像测试

在服务器 PaddleOCR 目录分别使用三份 ONNX 测试 YOLO 裁剪后的车牌图像。

测试 `ppocrv4_rec.onnx`（与历史 `ppocrv4_rec_fixed.onnx` 为同一模型）：

```bash
python eval_onnx_blue_green_train_test.py \
  --dataset-root /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/yolo_lprnet_crops \
  --character-dict /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/ppocr/utils/cblprd_plate_dict.txt \
  --provider cuda \
  --progress-step 100 \
  --model /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/onnx/ppocrv4_rec.onnx
```

测试原始 opset 14 导出的 `ppocrv4_rec14.onnx`：

```bash
python eval_onnx_blue_green_train_test.py \
  --dataset-root /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/yolo_lprnet_crops \
  --character-dict /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/ppocr/utils/cblprd_plate_dict.txt \
  --provider cuda \
  --progress-step 100 \
  --model /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14.onnx
```

测试折叠 LearnableAffine 的 `ppocrv4_rec14_fold_affine_1x1.onnx`：

```bash
python eval_onnx_blue_green_train_test.py \
  --dataset-root /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/yolo_lprnet_crops \
  --character-dict /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/ppocr/utils/cblprd_plate_dict.txt \
  --provider cuda \
  --progress-step 100 \
  --model /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14_fold_affine_1x1.onnx
```

三份 ONNX 在 `yolo_lprnet_crops` 上的全部分子集统计完全一致，共同结果为：

| subset | samples | raw_correct | raw_accuracy | rule_correct | rule_accuracy | changed | fixed | harmed |
|--------|---------|-------------|--------------|--------------|---------------|---------|-------|--------|
| blue_train | 13106 | 12332 | 94.0943% | 12385 | 94.4987% | 63 | 53 | 0 |
| green_train | 1913 | 1797 | 93.9362% | 1797 | 93.9362% | 0 | 0 | 0 |
| blue_test | 1404 | 1321 | 94.0883% | 1331 | 94.8006% | 11 | 10 | 0 |
| green_test | 608 | 545 | 89.6382% | 545 | 89.6382% | 0 | 0 | 0 |
| TOTAL | 17031 | 15995 | 93.9170% | 16058 | 94.2869% | 74 | 63 | 0 |

同一次服务器评估记录的端到端耗时和吞吐率为：

| ONNX 模型 | elapsed | end-to-end throughput |
| ---------- | ------: | --------------------: |
| `ppocrv4_rec.onnx` | 33.41 s | 509.69 images/s |
| `ppocrv4_rec14.onnx` | 34.47 s | 494.14 images/s |
| `ppocrv4_rec14_fold_affine_1x1.onnx` | 30.43 s | 559.70 images/s |

三份模型的整牌准确率和规则修正结果没有差异。本次单轮测试中，折叠版本相对 `ppocrv4_rec.onnx` 吞吐率提高约 `9.81%`，相对 `ppocrv4_rec14.onnx` 提高约 `13.27%`；性能结论仍应通过相同环境下的多轮重复测试确认。

#### yolo_lprnet_crops Letterbox 对照测试

`eval_onnx_blue_green_train_test_letterbox.py` 复用原脚本的模型加载、四组数据遍历、CTC 解码、规则修正和统计逻辑，仅替换输入预处理。图像等比例缩放到 `160×48` 内并固定左对齐：宽高比不超过 `160/48` 时在右侧补零，超过时在上下居中补零，左侧始终不补。当前 `94×24` 数据缩放为 `160×41`，上补3行、下补4行；归一化和原脚本一致，输出仍为 FP32 `[1,3,48,160]`。

```bash
cd /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/scripts

python eval_onnx_blue_green_train_test_letterbox.py \
  --dataset-root /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/yolo_lprnet_crops \
  --character-dict /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/ppocr/utils/cblprd_plate_dict.txt \
  --provider cuda \
  --progress-step 100 \
  --model /root/RK3568_FPGA_Project/1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14_fold_affine_1x1.onnx \
  --save-details onnx_blue_green_letterbox_eval_details.tsv
```

该结果应与原脚本记录的 `TOTAL raw_accuracy=93.9170%`、`rule_accuracy=94.2869%` 分别对照，并重点比较 `blue_test` 和 `green_test`，不能跨模型或跨运行环境混合比较耗时。

三份 ONNX 在 yolo_lprnet_crops 上Letterbox 对照的全部分子集使用统计完全一致，共同结果为：
| subset      | samples | raw_correct | raw_accuracy | rule_correct | rule_accuracy | changed | fixed | harmed |
|-------------|--------:|-----------:|------------:|-------------:|--------------:|--------:|------:|------:|
| blue_train  | 13106  | 12153      | 92.7285%    | 12228        | 93.3008%      | 88     | 75    | 0     |
| green_train | 1913   | 1789       | 93.5180%    | 1789         | 93.5180%      | 1      | 0     | 0     |
| blue_test   | 1404   | 1305       | 92.9487%    | 1314         | 93.5897%      | 12     | 9     | 0     |
| green_test  | 608    | 548        | 90.1316%    | 548          | 90.1316%      | 0      | 0     | 0     |
| TOTAL       | 17031  | 15795      | 92.7426%    | 15879        | 93.2359%      | 101    | 84    | 0     |

## 必须保持的部署约束

1. `Global.max_text_length` 保持 10，确保 NRTR 能编码 8 字符车牌及起止标记。
2. `Global.use_space_char` 保持 `false`。
3. 73 字符字典顺序在训练、导出、ONNX、RKNN 和 C++ 解码端必须完全一致。
4. 原图不需要预先缩放或补零；`RecResizeImg` 在加载时等比例缩放并补到 48×160。
5. 训练使用 MultiHead，推理导出只保留 CTC，预期 RKNN 输出为 `[1, 20, 74]`。
6. 官方中文预训练模型的旧分类头与 74 类不匹配属于正常现象，PPLCNetV3 与 SVTR 主体权重仍会加载。

## VisualDL

```bash
visualdl \
  --logdir /root/autodl-tmp/paddleocr_output/cblprd_ppocrv4_rec_48x160_linux4090 \
  --host 0.0.0.0 \
  --port 6006
```
