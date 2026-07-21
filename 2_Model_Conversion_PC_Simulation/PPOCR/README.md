# PP-OCRv4 ONNX 转 RKNN（RK3568）

## 当前模型约束

本目录基于 RKNN Model Zoo 2.3.0 的 `PPOCR/PPOCR-Rec/python/convert.py` 建立项目专用转换流程，官方参考目录保持不变。

当前待转换模型已核对为：

- ONNX opset 14；
- 输入 `x`：FP32、`[1, 3, 48, 160]`；
- 输出：FP32、`[1, 20, 74]`；
- 字典：73 个字符，CTC blank 为类别 0，字符类别为字典索引加 1；
- 输入颜色为 BGR，按高度 48 等比例缩放，右侧补零到宽度 160，再执行 `(x / 255 - 0.5) / 0.5`。

`python/convert.py` 会在调用 RKNN Toolkit2 前严格检查上述输入、输出和字典关系，避免生成分类头或输入尺寸不匹配的 RKNN 文件。

## Ubuntu 环境准备

ONNX 到 RKNN 仍在 Ubuntu VM 中执行，并使用 RKNN Toolkit2 2.3.0。生成的 `.onnx`、`.rknn`、校准图片和分析快照都已由 `model/.gitignore` 排除，不应提交。

以下命令均从本目录执行：

```bash
cd 2_Model_Conversion_PC_Simulation/PPOCR
```

可先只验证模型和字典；此步骤不导入 RKNN Toolkit2：

```bash
python python/convert.py \
  ../../1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14.onnx \
  rk3568 fp \
  --character-dict model/cblprd_plate_dict.txt \
  --validate-only
```

## 第一步：生成 FP16 基线

先生成非量化模型，用它区分“ONNX 转 FP16”误差和后续 INT8 量化误差：

```bash
python python/convert.py \
  ../../1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14.onnx \
  rk3568 fp \
  model/ppocrv4_rec14_rk3568_fp16.rknn \
  --character-dict model/cblprd_plate_dict.txt \
  --verbose
```

RK3568 不执行 FP32；`fp` 构建得到的是 FP16 RKNN 基线。

## 第二步：构造 PTQ 校准集

默认从训练清单而不是验证清单取样，避免用验证集参与量化参数校准。脚本从 basic、hard、使、学、港、澳、警、领八个子集均衡抽取 80 张，并生成无损 `48x160` PNG：

```bash
python python/build_quant_dataset.py \
  --manifest-dir ../../1_PC_Training/datasets/CBLPRD-330k \
  --data-dir ../../1_PC_Training/datasets/CBLPRD-330k \
  --split train \
  --samples 80 \
  --seed 20260720
```

先检查清单和抽样数量而不写文件时追加 `--dry-run`。重新生成已有校准目录时必须显式追加 `--overwrite`；脚本只会删除自身生成的 `quant_*.png`，发现其他文件会停止。

校准图不直接保存归一化浮点张量。它保持 BGR，按高度 48 等比例缩放，右侧用原始像素值 128 填充。转换脚本通过：

```python
mean_values=[[127.5, 127.5, 127.5]]
std_values=[[127.5, 127.5, 127.5]]
quant_img_RGB2BGR=True
```

把有效图像恢复为训练时的 `[-1, 1]` 输入，并使填充区接近训练张量中的 0。不能把未预处理原图直接写入 `dataset.txt`，否则 Toolkit 的固定尺寸缩放会把车牌横向拉伸，校准分布与真实推理不一致。

`quant_dataset.txt` 使用生成时所在 Ubuntu 环境的绝对路径，因此应在实际执行 RKNN 转换的 Ubuntu 环境中生成，不要在 Windows 生成后直接复制使用。

## 第三步：生成 W8A8 INT8 模型

RKNN 2.3.0 在 RK3568 上的 PTQ 基线使用 W8A8、Normal 和 Per-Channel：

```bash
python python/convert.py \
  ../../1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14.onnx \
  rk3568 i8 \
  model/ppocrv4_rec14_rk3568_i8.rknn \
  --character-dict model/cblprd_plate_dict.txt \
  --dataset model/quant_dataset.txt \
  --quantized-algorithm normal \
  --quantized-method channel \
  --accuracy-analysis-input model/quant_images/quant_000.png \
  --verbose
```

量化优先级如下：

1. 先保留 `normal + channel + 80 张` 作为可复现基线；
2. 若完整验证集准确率相对 FP16 明显下降，再比较 `kl_divergence + channel`；
3. 对异常值敏感时尝试 `mmse + channel`，建议另生成约 40 张校准集以控制转换时间和内存；
4. 不优先使用 `layer`，因为 Per-Channel 权重量化通常精度更高；
5. 混合量化只在普通 W8A8 经算法和校准集调整后仍不能满足精度时引入，避免过早增加部署复杂度。

`accuracy_analysis` 只用于定位逐层量化误差，不能替代端到端车牌准确率。最终必须在相同 17,357 张验证集上依次比较 ONNX、FP16 RKNN 和 INT8 RKNN 的原始 CTC 准确率及规则后准确率。

## FP16 RKNN 板端完整验证结果（2026-07-21）

验证环境为 RK3568、RKNN Runtime 2.3.0、NPU 驱动 0.9.8，`RKNN_LOG_LEVEL=0`，预热 20 次。模型输入和输出均为 FP16，输入仍由运行时执行 `(x-127.5)/127.5`。验证集共 17,357 张，以下结果统一采用未经车牌规则修正的原始 CTC 整牌严格匹配口径。FP16 原始记录见 [evaluation.log](../../3_NPU_Yolov8_LPR_Demo/result/ppocr_fp16_board/evaluation.log) 和 [details.tsv](../../3_NPU_Yolov8_LPR_Demo/result/ppocr_fp16_board/details.tsv)，ONNX 基线见 [README_CBLPRD_RK3568.md](../../1_PC_Training/README_CBLPRD_RK3568.md)。

| 子集 | 样本数 | ONNX 正确数 | ONNX 原始准确率 | FP16 正确数 | FP16 原始准确率 | FP16 相对 ONNX 变化 | RKNN 官方推理 | 推理端到端 |
|------|-------:|------------:|-----------------:|------------:|-----------------:|--------------------:|----------------:|-----------:|
| basic | 9,897 | 9,083 | 91.7753% | 9,057 | 91.5126% | -0.2627 pp | 19.941518 ms | 21.538323 ms |
| hard | 2,260 | 1,507 | 66.6814% | 1,503 | 66.5044% | -0.1770 pp | 19.975523 ms | 21.575379 ms |
| 使 | 1,132 | 1,120 | 98.9399% | 1,122 | 99.1166% | +0.1767 pp | 19.980211 ms | 21.577976 ms |
| 学 | 1,521 | 1,263 | 83.0375% | 1,256 | 82.5773% | -0.4602 pp | 19.915312 ms | 21.512933 ms |
| 港 | 656 | 592 | 90.2439% | 590 | 89.9390% | -0.3049 pp | 19.960959 ms | 21.562829 ms |
| 澳 | 654 | 581 | 88.8379% | 581 | 88.8379% | 0.0000 pp | 19.995541 ms | 21.593036 ms |
| 警 | 44 | 26 | 59.0909% | 26 | 59.0909% | 0.0000 pp | 20.000591 ms | 21.591089 ms |
| 领 | 1,193 | 1,171 | 98.1559% | 1,170 | 98.0721% | -0.0838 pp | 19.932381 ms | 21.528970 ms |
| **TOTAL** | **17,357** | **15,343** | **88.3966%** | **15,305** | **88.1777%** | **-0.2189 pp** | **19.948465 ms** | **21.545988 ms** |

FP16 完整数据集耗时 384,706.129 ms，实际处理吞吐为 45.118 张/秒；按平均 RKNN 官方推理时间折算约为 50.13 FPS，按单张推理端到端时间折算约为 46.41 FPS。`rknn_run` 墙钟时间只比 RKNN 官方时间平均多 0.0072 ms，计时逻辑与官方查询结果一致；推理端到端比 `rknn_run` 平均多 1.5903 ms，主要包含单张输入设置、输出读取和 CTC 解码等开销。完整数据集吞吐还包含 JPEG 解码、结果记录和循环调度。

FP16 共错误 2,052 张，字符错误率（CER）为 1.8190%，对应字符级准确率 98.1810%。其中 1,824 张错误样本（88.89%）的编辑距离仅为 1，1,293 张（63.01%）存在长度不一致，主要仍是漏字符；常见字符混淆包括 `0/O`、`1/I`、`D/0` 和 `G/6`。7 字符与 8 字符车牌的整牌准确率分别为 88.9911% 和 86.6981%。错误样本中仍有 1,195 张的置信度不低于 0.95，说明 FP16 的平均置信度同样不适合直接作为错误拒识阈值。

从汇总结果看，FP16 仅比同验证集 ONNX 少正确识别 38 张，原始整牌准确率下降 0.2189 个百分点，各子集最大变化为学车牌的 -0.4602 个百分点，说明 ONNX 转 RKNN FP16 基本保持了原模型精度。当前未保存 ONNX 的逐样本预测明细，因此“少 38 张”是正确总数净差，不能解释为只有 38 个样本的预测发生变化。

## W8A8 Normal + Per-Channel 板端完整验证结果（2026-07-21）

本次模型实际使用 100 张训练集校准图、`normal` 算法和 `channel` 方法生成；当前 `quant_manifest.tsv` 与 `quant_dataset.txt` 均为 100 条，basic、hard、使、学各 13 张，港、澳、警、领各 12 张。验证环境为 RK3568、RKNN Runtime 2.3.0、NPU 驱动 0.9.8，`RKNN_LOG_LEVEL=0`，预热 20 次。验证集共 17,357 张，以下准确率均为未经车牌规则纠错的原始 CTC 整牌严格匹配结果。原始记录见 [evaluation.log](../../3_NPU_Yolov8_LPR_Demo/result/ppocr_i8_board/evaluation.log) 和 [details.tsv](../../3_NPU_Yolov8_LPR_Demo/result/ppocr_i8_board/details.tsv)。

| 子集 | 样本数 | ONNX 原始准确率 | INT8 正确数 | INT8 原始准确率 | 相对 ONNX 下降 | RKNN 官方推理 | 推理端到端 |
|------|-------:|----------------:|-------------:|----------------:|----------------:|----------------:|-----------:|
| basic | 9,897 | 91.7753% | 7,478 | 75.5582% | 16.2171 pp | 9.846830 ms | 10.182169 ms |
| hard | 2,260 | 66.6814% | 950 | 42.0354% | 24.6460 pp | 9.821172 ms | 10.155359 ms |
| 使 | 1,132 | 98.9399% | 1,022 | 90.2827% | 8.6572 pp | 9.807950 ms | 10.144011 ms |
| 学 | 1,521 | 83.0375% | 918 | 60.3550% | 22.6825 pp | 9.868524 ms | 10.203825 ms |
| 港 | 656 | 90.2439% | 527 | 80.3354% | 9.9085 pp | 9.918776 ms | 10.254598 ms |
| 澳 | 654 | 88.8379% | 507 | 77.5229% | 11.3150 pp | 9.927613 ms | 10.263124 ms |
| 警 | 44 | 59.0909% | 15 | 34.0909% | 25.0000 pp | 9.982477 ms | 10.322859 ms |
| 领 | 1,193 | 98.1559% | 1,029 | 86.2531% | 11.9028 pp | 9.921610 ms | 10.257824 ms |
| **TOTAL** | **17,357** | **88.3966%** | **12,446** | **71.7059%** | **16.6907 pp** | **9.854101 ms** | **10.189432 ms** |

完整数据集耗时 187,860.532 ms，实际处理吞吐为 92.393 张/秒；按平均 RKNN 官方推理时间折算约为 101.48 FPS，按单张推理端到端时间折算约为 98.14 FPS。前者只反映 NPU 推理，完整数据集吞吐还包含 JPEG 解码、结果记录和循环调度。

明细分析表明字符错误率（CER）为 4.9203%，对应字符级准确率 95.0797%，但整牌严格匹配只有 71.7059%。4,911 个错误样本中，3,837 个（78.13%）编辑距离仅为 1；3,604 个（73.39%）存在长度不一致，且主要表现为漏字符。高频混淆包括 `0/O`、`D/0`、`1/I`、`琼/京` 和 `藏/蒙`。8 字符车牌准确率为 65.2266%，低于 7 字符车牌的 75.2679%。此外，2,568 个错误样本的置信度仍不低于 0.95，因此当前分数不能直接作为可靠的错误拒识阈值。

结论：这组 `normal + channel + 100 张均衡校准图` 的速度稳定，但相对同环境 FP16 少正确识别 2,859 张，原始整牌准确率下降 16.4718 个百分点；相对 ONNX 则少正确识别 2,897 张、下降 16.6907 个百分点。由于 ONNX 转 FP16 仅下降 0.2189 个百分点，可以确认主要精度损失发生在 FP16 转 INT8 的量化阶段。INT8 的 RKNN 官方推理约为 FP16 的 2.02 倍速，完整数据集吞吐约为 2.05 倍，但当前精度损失过大，不适合作为最终部署量化方案。下一步应保持 Toolkit2 建议的最多 100 张规模，调整校准构成以提高 hard、学、警及 8 字符样本的视觉多样性，并对比 `kl_divergence + channel`、`mmse + channel`。若常规 W8A8 仍无法恢复精度，再定位敏感层并评估混合量化。

## 第四步：折叠 1×1 Conv 前的 LearnableAffine

当前普通 W8A8 相对 FP16 的精度损失过大，因此先对 12 组单消费者 `Mul -> Add -> 1×1 Conv` 执行数学等价折叠。脚本只匹配当前模型中已核对的 12 个 Conv，并强制检查标量 affine、`group=1`、1×1 kernel、独占权重/bias 和单消费者关系；原始 ONNX 不会被覆盖。

```bash
python python/fold_affine_into_1x1_conv.py \
  ../../1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14.onnx \
  ../../1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14_fold_affine_1x1.onnx \
  --overwrite
```

折叠关系为 `W' = scale * W`、`B' = B + affine_bias * sum(W)`。当前模型实测完成 12 组折叠并移除 12 个 Mul、12 个 Add 和 24 个专用 Constant；10 组随机输入最大输出绝对误差为 `3.58e-6`，`test_ppocr.jpg` 为 `1.97e-6`，CTC argmax 均与原 ONNX 一致。正式转换前仍应在完整验证集确认原始 ONNX 与折叠 ONNX 的预测一致。

先生成折叠 FP16 并验证等价性，再使用当前完全相同的 100 张校准集和 `normal + channel` 生成折叠 INT8，以便单独判断折叠收益：

```bash
python python/convert.py \
  ../../1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14_fold_affine_1x1.onnx \
  rk3568 fp \
  model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn \
  --character-dict model/cblprd_plate_dict.txt \
  --verbose

python python/convert.py \
  ../../1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14_fold_affine_1x1.onnx \
  rk3568 i8 \
  model/ppocrv4_rec14_fold_affine_1x1_rk3568_i8.rknn \
  --character-dict model/cblprd_plate_dict.txt \
  --dataset model/quant_dataset.txt \
  --quantized-algorithm normal \
  --quantized-method channel \
  --verbose
```

### 折叠版 FP16 转换结果（2026-07-21）

折叠版 FP16 已使用 RKNN-Toolkit2 2.3.0 成功转换，模型检查结果为输入 `x: [1,3,48,160]`、输出 `save_infer_model/scale_0.tmp_0: [1,20,74]`、字典 73 个字符，均与当前 CTC 识别模型一致。生成文件为 [ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn](model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn)，文件大小 4,770,245 字节；Toolkit 日志给出的内部内存为 1,920 KB、权重内存为 3,801.38 KB。

本次构建最终输出 `done`，无转换错误。日志中的 `quant_img_RGB2BGR` 是当前图像输入配置提示；另有 2 条 `TRANSPOSE` 维度不匹配提示和 4 条 Split 中间张量默认采用 FP16 的提示，均未阻止模型生成，但仍需通过板端加载、单图输出和完整验证集确认运行正确性。详细日志中仍出现的 `learnable_affine_block_*` 张量名不表示 12 组目标折叠失败：脚本仅折叠紧邻、单消费者且满足约束的 `Mul -> Add -> 1×1 Conv`，其他不满足该匹配条件的 LearnableAffine 会保留；同时，已折叠权重或中间张量也可能继续沿用原始命名。

### 折叠版 FP16 板端完整验证结果（2026-07-21）

折叠版 FP16 在与基线相同的 RK3568、Runtime 2.3.0、驱动 0.9.8、日志等级 0、warmup 20 和 17,357 张验证集条件下完成原始 CTC 严格匹配评估。原始记录见 [evaluation.log](../../3_NPU_Yolov8_LPR_Demo/result/ppocr_fold_affine_fp16_board/evaluation.log) 和 [details.tsv](../../3_NPU_Yolov8_LPR_Demo/result/ppocr_fold_affine_fp16_board/details.tsv)。

| 子集 | 样本数 | 基线 FP16 正确数/准确率 | 折叠 FP16 正确数/准确率 | 准确率变化 | 折叠官方推理 | 折叠推理端到端 |
|------|-------:|-----------------------:|-----------------------:|-----------:|-------------:|---------------:|
| basic | 9,897 | 9,057 / 91.5126% | 9,054 / 91.4823% | -0.0303 pp | 17.355915 ms | 18.695037 ms |
| hard | 2,260 | 1,503 / 66.5044% | 1,506 / 66.6372% | +0.1327 pp | 17.393873 ms | 18.732895 ms |
| 使 | 1,132 | 1,122 / 99.1166% | 1,120 / 98.9399% | -0.1767 pp | 17.371791 ms | 18.710347 ms |
| 学 | 1,521 | 1,256 / 82.5773% | 1,253 / 82.3800% | -0.1972 pp | 17.381725 ms | 18.722728 ms |
| 港 | 656 | 590 / 89.9390% | 590 / 89.9390% | 0.0000 pp | 17.399436 ms | 18.739052 ms |
| 澳 | 654 | 581 / 88.8379% | 581 / 88.8379% | 0.0000 pp | 17.399075 ms | 18.737048 ms |
| 警 | 44 | 26 / 59.0909% | 26 / 59.0909% | 0.0000 pp | 17.404045 ms | 18.754591 ms |
| 领 | 1,193 | 1,170 / 98.0721% | 1,170 / 98.0721% | 0.0000 pp | 17.405248 ms | 18.744333 ms |
| **TOTAL** | **17,357** | **15,305 / 88.1777%** | **15,300 / 88.1489%** | **-0.0288 pp** | **17.370938 ms** | **18.710177 ms** |

两版逐样本预测有 17,301 张完全一致，占 99.6774%；56 张发生变化，其中基线正确、折叠错误 24 张，基线错误、折叠正确 19 张，净少正确 5 张。折叠使官方推理延迟从 19.948465 ms 降至 17.370938 ms（降低 12.92%），端到端从 21.545988 ms 降至 18.710177 ms（降低 13.16%），完整数据集吞吐从 45.118 提升至 50.806 张/秒（提升 12.61%）。该折叠版在几乎保持精度的同时获得稳定速度收益，优先作为当前 FP16 部署候选。

## 第五步：折叠模型混合量化

项目使用 `python/hybrid_quant.py` 封装官方 `hybrid_quantization_step1/step2` 两阶段流程。当前折叠模型在 Toolkit2 2.3.0 中开启 `proposal=True` 会进入自动敏感层建议路径，并在内部 `IRGraph.expand_batch/infer_shapes` 对临时 tensor `Slice.5-rs` 抛出 `KeyError`；官方 `examples/functions/hybrid_quant/step1.py` 默认使用 `proposal=False`。因此项目脚本固定关闭自动 proposal，保留人工依据 `accuracy_analysis` 选择 FP16 tensor 的流程，避免把 Toolkit 内部异常误判为 ONNX 或校准集损坏。

Step1 必须使用不存在或为空的独立工作目录，防止失败残留的 `.model/.data` 与新转换混用。脚本会重新校验固定输入输出、73 字符字典和全部校准图片，保存绝对路径校准清单，并同时保留 Toolkit 原始 cfg 和用于人工修改的 `*.quantization.manual.cfg`：

```bash
python python/hybrid_quant.py step1 \
  ../../1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14_fold_affine_1x1.onnx \
  rk3568 \
  --dataset model/quant_dataset.txt \
  --character-dict model/cblprd_plate_dict.txt \
  --work-dir model/hybrid_fold_manual \
  --quantized-algorithm mmse \
  --quantized-method channel \
  --verbose
```

### MMSE 手工混合量化 Step1 结果（2026-07-21）

已在 `model/hybrid_fold_manual` 使用 100 张现有均衡校准图和 `mmse + channel` 完成 `proposal=False` 的 Step1，成功生成 7,758,486 字节的 `.model`、1,247,708 字节的 `.data`，以及两份 69,768 字节的原始/手工 cfg；此前 `proposal=True` 的 `Slice.5-rs` 异常未再出现。Step1 刚完成时手工 cfg 为 `custom_quantize_layers: {}`；随后已保留该空配置作为全 INT8 诊断基线，并完成 `accuracy_analysis` 后选定 H1。

在编辑手工 cfg 前先复制空配置，并通过 Step2 生成与本次 MMSE Step1 参数完全相同的全量 INT8 诊断模型和逐层误差报告。`accuracy_analysis` 中 `single` 表示当前层局部误差，`entire` 表示传播至当前层的累计误差；应使用报告中的输出 tensor 名，在 `custom_quantize_layers` 下逐步加入 `tensor_name: float16`，不要修改 `quantize_parameters`：

```bash
cp model/hybrid_fold_manual/ppocrv4_rec14_fold_affine_1x1.quantization.manual.cfg \
   model/hybrid_fold_manual/ppocrv4_rec14_fold_affine_1x1.quantization.mmse_int8_diag.cfg

python python/hybrid_quant.py step2 \
  model/hybrid_fold_manual \
  model/ppocrv4_rec14_fold_affine_1x1_rk3568_mmse_i8_diag.rknn \
  --config model/hybrid_fold_manual/ppocrv4_rec14_fold_affine_1x1.quantization.mmse_int8_diag.cfg \
  --accuracy-analysis-input model/quant_images/quant_001.png \
  --accuracy-analysis-output model/hybrid_fold_manual/mmse_i8_accuracy_analysis \
  --verbose
```

### MMSE 全 INT8 逐层误差分析与 H1 配置（2026-07-21）

全 INT8 诊断模型已生成 `mmse_i8_accuracy_analysis/error_analysis.txt`。单样本模拟器报告中，融合 Conv 输出 `Add.27` 的单层余弦仅为 `0.92380`、单层欧氏误差为 `559.17`，同时其累计余弦下降到 `0.91406`，显著差于后续的 `Add.39=0.98529`、`Add.51=0.98942` 和前一层 `Add.15=0.99081`，是当前最明确的首要敏感层。第二个 attention 的 `softmax_1.tmp_0_tp_rs` 单层余弦为 `0.99718`，末端 `swish_4.tmp_0` 为 `0.99746`；最终分类头 `linear_8.tmp_0_mm_tp_sw` 单层余弦为 `0.99994`，最终输出累计余弦恢复到 `0.99947`，因此首轮不优先恢复分类头或 Softmax。

为保持变量单一并测量单层 FP16 的真实收益，H1 构建时只配置：

```yaml
custom_quantize_layers:
    Add.27: float16
```

H1 生成命令如下；输出模型和分析目录均使用新名称，不覆盖全 INT8 诊断结果：

```bash
python python/hybrid_quant.py step2 \
  model/hybrid_fold_manual \
  model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h1_add27.rknn \
  --config model/hybrid_fold_manual/ppocrv4_rec14_fold_affine_1x1.quantization.manual.cfg \
  --accuracy-analysis-input model/quant_images/quant_001.png \
  --accuracy-analysis-output model/hybrid_fold_manual/mmse_h1_add27_accuracy_analysis \
  --verbose
```

### H1 逐层误差结果与 H2 配置（2026-07-21）

H1 已成功生成 2,776,978 字节的 RKNN，比 2,770,834 字节的 MMSE 全 INT8 诊断模型增加 6,144 字节。报告证明配置已经生效：`Add.27` 的单层余弦从 `0.92380` 提升至 `1.00000`，累计余弦从 `0.91406` 提升至 `0.98885`。但 Toolkit 在其前后增加了 FP16/INT8 边界；紧随 `Add.27` 的 `Add.27__int8` 回量化单层余弦只有 `0.92843`、累计余弦为 `0.91776`，基本重新引入原始误差。最终输出累计余弦反而从全 INT8 的 `0.99947` 降至 `0.99473`，欧氏误差从 `0.1490` 增至 `0.4564`。全 INT8 与 H1 在该样本上的 20 步 argmax 和 CTC 文本仍相同，但相对次大类别的最小概率间隔从 `0.66394` 降到 `0.33081`。因此 H1 只证明了敏感点定位正确，但 FP16 区间过短，不作为当前优先板端候选。

H2 不再跳到不相邻的 `Add.39`，而是把 `Add.27` 的直接消费者 `hardswish_4.tmp_0` 一并保持为 FP16，使首次回量化发生在 HardSwish 输出之后。该输出在全 INT8 报告中的单层余弦为 `0.99984`，预期比直接量化 `Add.27` 更稳定。当前手工 cfg 已改为：

```yaml
custom_quantize_layers:
    Add.27: float16
    hardswish_4.tmp_0: float16
```

```bash
python python/hybrid_quant.py step2 \
  model/hybrid_fold_manual \
  model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn \
  --config model/hybrid_fold_manual/ppocrv4_rec14_fold_affine_1x1.quantization.manual.cfg \
  --accuracy-analysis-input model/quant_images/quant_001.png \
  --accuracy-analysis-output model/hybrid_fold_manual/mmse_h2_add27_hsw4_accuracy_analysis \
  --verbose
```

### H2 逐层误差结果与 H3 配置（2026-07-21）

H2 已成功生成 2,776,274 字节的 RKNN。`Add.27` 与 `hardswish_4.tmp_0` 的单层余弦均达到 `1.00000`，HardSwish 后新增的 `hardswish_4.tmp_0__int8` 回量化单层余弦为 `0.99992`，说明 H1 的边界问题已经解决，不需要继续延伸至 `Add.33`。但是最终输出累计余弦进一步从全 INT8/H1 的 `0.99947/0.99473` 降至 `0.98668`，欧氏误差增至 `0.7199`；20 个时间步中有 1 步 argmax 从重复字符变为空白，CTC 折叠文本暂时不变，但最小分类概率间隔进一步降至 `0.108887`。这表明 `Add.27` 的大局部误差会被后续网络抵消，将该分支恢复为 FP16 反而破坏最终输出，不再继续该路径。

H3 改为独立测试下一敏感 Conv `Add.39`，并从一开始同时保留其直接消费者 `hardswish_6.tmp_0` 为 FP16，避免重复 H1 的立即回量化问题。当前手工 cfg 已改为：

```yaml
custom_quantize_layers:
    Add.39: float16
    hardswish_6.tmp_0: float16
```

```bash
python python/hybrid_quant.py step2 \
  model/hybrid_fold_manual \
  model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h3_add39_hsw6.rknn \
  --config model/hybrid_fold_manual/ppocrv4_rec14_fold_affine_1x1.quantization.manual.cfg \
  --accuracy-analysis-input model/quant_images/quant_001.png \
  --accuracy-analysis-output model/hybrid_fold_manual/mmse_h3_add39_hsw6_accuracy_analysis \
  --verbose
```

### H3 逐层误差结果与当前停止条件（2026-07-21）

H3 已成功生成 2,776,274 字节的 RKNN。`Add.39` 的单层余弦从全 INT8 的 `0.98529` 提升至 `1.00000`，`hardswish_6.tmp_0` 也达到 `1.00000`，其后的 `hardswish_6.tmp_0__int8` 回量化单层余弦为 `0.99997`，因此配置及边界均正确。但是最终输出累计余弦从全 INT8/H1/H2 的 `0.99947/0.99473/0.98668` 继续降至 `0.97963`，欧氏误差增至 `0.8909`，最大绝对差增至 `0.629396`。H3 与 H2 一样有 1 个重复字符时间步变为空白；CTC 折叠文本未变，最小分类概率间隔为 `0.440186`，仍低于全 INT8 的 `0.66394`。

连续三次单图实验已经证明，当前 `quant_001.png` 报告中的最低局部余弦不能直接代表最终 CTC 输出敏感度。完成该阶段后曾将手工 cfg 恢复为空并停止依据单图构建 H4；随后通过板端完整验证重新评价各候选。不得一次性恢复整个 attention 或全部 HardSwish，否则可能丢失 INT8 性能收益且无法归因。

### MMSE 全 INT8 与 H1/H2/H3 板端完整验证（2026-07-21）

四个 MMSE 模型均在相同 RK3568、Runtime 2.3.0、驱动 0.9.8、`RKNN_LOG_LEVEL=0`、warmup 20 和 17,357 张验证集条件下完成原始 CTC 严格匹配评估；输入均为 INT8 `[1,48,160,3]`，输出均为 FP16 `[1,20,74]`。结果同时列出折叠 FP16 和此前 `normal + channel` INT8 作为参照：

| 模型 | 正确数 / 准确率 | RKNN 官方推理 | 推理端到端 | 数据集吞吐 |
|------|----------------:|--------------:|-----------:|-----------:|
| 折叠 FP16 | 15,300 / 88.1489% | 17.370938 ms | 18.710177 ms | 50.806 张/s |
| 普通 INT8（normal） | 12,446 / 71.7059% | 9.854101 ms | 10.189432 ms | 92.393 张/s |
| MMSE 全 INT8 | 14,405 / 82.9925% | **8.488004 ms** | **8.760366 ms** | **108.032 张/s** |
| MMSE H1：`Add.27` | 14,552 / 83.8394% | 8.899664 ms | 9.174841 ms | 103.329 张/s |
| MMSE H2：`Add.27 + hardswish_4` | **14,616 / 84.2081%** | 9.022712 ms | 9.298863 ms | 101.963 张/s |
| MMSE H3：`Add.39 + hardswish_6` | 14,482 / 83.4361% | 8.873892 ms | 9.147620 ms | 103.542 张/s |

MMSE 全 INT8 相对普通 INT8 多正确 1,959 张、提升 11.2865 个百分点，同时官方推理延迟降低 13.86%，说明折叠模型配合 MMSE 是当前量化改进的主体。H2 是三个混合候选中准确率最高者：相对 MMSE 全 INT8 多正确 211 张、提升 1.2156 个百分点，代价是官方推理延迟增加 6.30%、数据集吞吐下降 5.62%。相对折叠 FP16，H2 少正确 684 张、下降 3.9408 个百分点，但官方推理快 1.925 倍，完整数据集吞吐约为 2.007 倍。

逐样本比较显示，H2 相对 MMSE 全 INT8 修正 796 张原错误，同时使 585 张原正确样本变错，净增 211 张；主要净收益来自 basic `+104`、hard `+65` 和学 `+52`，但澳 `-16`、领 `-6`。相对折叠 FP16，H2 修正 396 张 FP16 错误，却使 1,080 张 FP16 正确样本变错。H2 的 CER 为 2.5114%，优于 MMSE 全 INT8 的 2.8295%，仍差于折叠 FP16 的 1.8268%；7 字符准确率为 86.1071%，8 字符为 80.7536%，相对 FP16 分别低 2.8393 和 5.9445 个百分点，8 字符仍是主要量化短板。

板端完整结果与单张模拟器余弦排序不一致：H2 在 `quant_001.png` 的最终余弦低于全 INT8，却在完整验证集取得最高量化准确率。因此混合量化只能以完整板端 CTC 指标定型，单图 `accuracy_analysis` 仅用于解释边界和筛选候选。当前手工 cfg 已恢复为 H2 的 `Add.27 + hardswish_4.tmp_0`，H2 定为当前优先量化候选；H1 和 H3 不再继续。若目标必须接近折叠 FP16 的原始识别效果，H2 尚有 3.9408 个百分点差距，不能替代 FP16；若接受该精度差距以换取约 2 倍吞吐，则可进入 YOLO 级联实测。后续量化优化应从 H2 相对 FP16 的真实错误样本中分层选择 hard、学、澳和 8 字符样本进行多样本诊断，不再由单张校准图决定 FP16 层。

## 第六步：RKNN 无损剪枝 P0/P1（2026-07-21）

新增 `python/build_pruned_models.py`，从同一份折叠 ONNX 连续构建两个互不覆盖的模型：P0 为 `model_pruning=True + FP16`，用于隔离验证剪枝本身的体积、GFLOPs、精度和速度收益；P1 为 `model_pruning=True + MMSE/channel W8A8`，用于验证剪枝是否改善当前全 INT8 基线。脚本复用 `convert.py` 的固定输入输出、73 字符字典及校准集校验，并默认拒绝覆盖已有 RKNN。

从本目录在 RKNN-Toolkit2 2.3.0 Ubuntu 环境执行：

```bash
set -o pipefail
python python/build_pruned_models.py \
  ../../1_PC_Training/PaddleOCR/onnx/ppocrv4_rec14_fold_affine_1x1.onnx \
  rk3568 \
  --dataset model/quant_dataset.txt \
  --character-dict model/cblprd_plate_dict.txt \
  --output-dir model \
  --verbose 2>&1 | tee model/pruning_p0_p1_build.log
```

生成文件固定为：

- `model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16_pruned.rknn`（P0）；
- `model/ppocrv4_rec14_fold_affine_1x1_rk3568_mmse_i8_pruned.rknn`（P1）。

可先追加 `--validate-only` 只检查 ONNX、字典、校准图片和输出冲突；重复构建必须显式追加 `--overwrite`。当前校准集为 100 张，为保持与既有 MMSE 全 INT8 基线可比，P1 不自动裁剪样本数，但会提示 Toolkit2 对 MMSE 通常建议 20–50 张。构建后先从日志记录 `Weight Compress`、模型大小和 `GFLOPs` 的前后变化；若没有压缩统计或变化接近 0，说明当前折叠模型缺少足够稀疏性，不继续期待剪枝收益。

“无损”只描述 Toolkit2 的剪枝变换目标，不代表 P1 的 INT8 量化无损。P0、P1 仍需分别在相同 17,357 张验证集、`RKNN_LOG_LEVEL=0` 和相同板端频率条件下测试原始 CTC 准确率、RKNN 官方推理时间及 RKNN 文件大小。P0 应与折叠 FP16 对照，P1 应与 MMSE 全 INT8 对照；只有压缩量可观且精度、性能指标不退化时才保留候选。

### 后续量化路线决策（2026-07-21）

当前不再沿着单张逐层报告继续增加混合 FP16 层；H2 保留为 PTQ/混合量化基线。训练阶段冻结 BN 的目的是让小学习率、部分参数微调继续使用预训练全局均值和方差，避免小 batch 或车牌域数据改写统计量，并不表示 BN 是部署量化时最敏感的算子。现有逐层报告的主要局部误差出现在 Conv 输出，BN 相关输出没有形成跨样本证据；BN 在推理转换中还可能与 Conv 融合。若进行 QAT，仍保持全部 BN 的 running mean/variance 与 affine 参数冻结，但必须让 CTC 推理分支中的 Conv/Linear 权重参与训练，不能继续只开放原来的 33 个 CTC SVTR/FC tensor，否则 Backbone 无法学习补偿伪量化误差。

若 P0/P1 证实当前模型没有实质剪枝收益，且 H2 相对折叠 FP16 的 3.9408 个百分点差距不可接受，下一优先级才改为 PaddleSlim QAT，而不是 PaddleSlim PTQ 后再交给 RKNN 二次 PTQ。使用项目现有 PaddleOCR 官方入口 `deploy/slim/quantization/quant.py` 与 `export_model.py`，首轮保持 W8A8、权重 `channel_wise_abs_max`、激活 `moving_average_abs_max`、量化 `Conv2D/Linear`，并保持 CTC-only、NRTR 训练前向关闭和 BN 冻结。QAT 量化推理模型经 Paddle2ONNX 导出后，必须先确认 ONNX 中保留 `QuantizeLinear/DequantizeLinear` 或等价量化参数、ONNXRuntime 与 Paddle QAT 的完整验证集输出一致，再由 RKNN-Toolkit2 以 `do_quantization=False` 构建，禁止再次提供校准集执行 PTQ。

PaddleSlim 到 ONNX 是当前链路中风险最高的跨框架环节，正式长周期 QAT 前先做短训练烟雾测试：验证 Paddle QAT 精度、量化推理模型导出、ONNX 量化节点保留、RKNN 加载及板端单图输出。任一阶段丢失量化参数都应停止，不能把退化后的普通浮点 ONNX 当成 QAT 模型继续转换。最终仍以 17,357 张板端原始 CTC 准确率和 `RKNN_QUERY_PERF_RUN` 为准；建议 QAT 至少明显超过 H2，且优先把与折叠 FP16 的差距压到 1 个百分点以内，才值得替换现有候选。

如果需要使用其他 cfg，可显式传入 `--config <path>`。重复生成已经存在的 RKNN 时必须添加 `--overwrite`；逐层分析目录不允许覆盖，应为每个候选使用新目录。混合量化的最终判断仍以 RK3568、`RKNN_LOG_LEVEL=0`、相同 17,357 张验证集的原始 CTC 整牌准确率和 `RKNN_QUERY_PERF_RUN` 为准。

## 当前验证边界

基线 FP16、折叠版 FP16、普通 INT8、MMSE 全 INT8 以及 H1/H2/H3 均已完成 RK3568 板端加载、固定输入输出语义、17,357 张原始 CTC 严格匹配和 `RKNN_QUERY_PERF_RUN` 性能验证。折叠 FP16 以 88.1489% 保持当前最高准确率；H2 以 84.2081% 和 9.022712 ms 成为当前优先量化候选，但尚不能在精度优先场景替代折叠 FP16。P0/P1 剪枝构建脚本已就绪，但尚未生成和板端验证，不能计入当前候选结论。下一阶段先完成 P0/P1 的剪枝统计和同口径板端对照；若没有实质收益，再决定进入 QAT 或直接以折叠 FP16/H2 开展 YOLO 多车牌级联吞吐及隔帧策略对比。

参考依据：`02_Rockchip_RKNPU_User_Guide_RKNN_SDK_V2.3.0_CN.pdf` 第 6.2、7.1、10.3 节，以及 `03_Rockchip_RKNPU_API_Reference_RKNN_Toolkit2_V2.3.0_CN.pdf` 的 `config`、`build`、`accuracy_analysis` 接口说明。
