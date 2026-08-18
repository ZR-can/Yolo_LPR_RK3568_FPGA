# YOLOv8n-OBB 训练记录（2026-07-31）

## 训练目标

- 任务：4 类车牌 OBB 检测。
- 类别：`blue=0`、`green=1`、`yellow_single=2`、`other=3`。
- 初始权重：`pretrained/yolov8n-obb.pt`。
- 数据配置：`configs/yolo_obb_config.yaml`。
- 训练配置：`configs/yolo_obb_train.yaml`。
- 运行环境：Conda `YOLOv8n_LPRNet`，Python 对应
  `C:/Users/ZR/.conda/envs/YOLOv8n_LPRNet/python.exe`。
- 软件与硬件：PyTorch 2.1.0+cu121、Ultralytics 8.4.33、
  NVIDIA GeForce RTX 4060 Laptop GPU（8,188 MiB）。

## 关键参数

- `imgsz=640`、`batch=16`、`epochs=100`、`patience=20`。
- `optimizer=AdamW`、`lr0=0.001`、`cos_lr=true`、`amp=true`。
- `device=0`、`workers=8`、`seed=20260731`、`deterministic=true`。
- 验证入口：`datasets/yolo_obb_640/val_balanced.txt`，共 2,918 张。
- 训练集：40,921 张；完整 val：9,021 张；test：9,231 张。

## 启动审计

- 03:52 的首次预检运行在数据扫描阶段发现 7 个伪 JPG，已在首个 epoch
  前主动停止；没有产生 checkpoint。
- 7 个文件实际为 macOS AppleDouble 元数据。构建器已改为抽样前排除，
  并以有效 `ccpd_np` 负样本补足原配额。
- 派生数据集精确替换 7 张硬链接后通过全量结构校验；52,714 个唯一源图
  的解码与 JPEG EOI 检查均通过，异常数为 0。
- 首次运行留下的目录仅含 `args.yaml` 与空 `weights` 目录，确认无
  checkpoint 或 `results.csv` 后已删除；原始失败日志保留。

## 当前运行

- 正式启动时间：2026-07-31 04:03:58（Asia/Shanghai）。
- 主进程 PID：`4032`。
- 输出目录：`scripts/runs/obb_train_results/yolov8n_plate_obb_640`。
- 标准输出：`logs/yolo_obb_train_20260731_040358.stdout.log`。
- 标准错误：`logs/yolo_obb_train_20260731_040358.stderr.log`。
- train 扫描完成：40,921 张、500 个 background、`corrupt=0`。
- 平衡 val 扫描完成：2,918 张、62 个 background、`corrupt=0`。
- 2026-07-31 15:33 状态：已完成 38 个 epoch，epoch 39 运行至
  851/2,558 batch；GPU 利用率约 81%，显存约 2.29 GiB。
- 当前最佳已更新为 epoch 37：Precision=0.97875、Recall=0.97757、
  mAP50=0.98815、mAP50-95=0.93578。
- epoch 37 的 mAP50-95 比此前 epoch 34 最佳值 0.93545 提高
  0.00033；`best.pt` 已于 15:21:33 更新。
- 最新完成的 epoch 38：Precision=0.97954、Recall=0.97289、
  mAP50=0.98815、mAP50-95=0.93555。
- epoch 37 后 1 轮未刷新最佳；当前配置的 `patience=20` 继续生效。
- 当前未发现 OOM、Traceback、RuntimeError、NaN/Inf 或数据异常。
- epoch 17 到 18 的累计墙钟时间异常增加 20,732.92 秒；恢复后的指标和
  损失连续，具体外部暂停或阻塞原因无法仅由训练日志确定。
- 数据集标准、当前训练分析及训练完成后的分组验收要求已统一写入
  `YOLO_OBB_DATASET_RECORD_20260731.md`。
- 首次预检标准输出：`logs/yolo_obb_train_20260731_035211.stdout.log`。
- 首次预检标准错误：`logs/yolo_obb_train_20260731_035211.stderr.log`。

## 逐类别验证快照

- 验证时间：2026-07-31 15:39。
- checkpoint：epoch 37 `best.pt`，SHA-256
  `EA542AC7F16D4BAEC82E0E975DA58FE73A7F7E34A05B69811803FAA0ADD9C7A9`。
- 数据：`val_balanced.txt`，2,918 张图片、3,107 个实例。
- 参数：OBB、`imgsz=640`、`batch=16`。

| 类别 | 图片 | 实例 | Precision | Recall | mAP50 | mAP50-95 |
|---|---:|---:|---:|---:|---:|---:|
| `blue` | 817 | 1,000 | 0.938 | 0.936 | 0.977 | 0.909 |
| `green` | 1,000 | 1,000 | 0.999 | 0.998 | 0.994 | 0.943 |
| `yellow_single` | 997 | 1,002 | 0.988 | 0.980 | 0.987 | 0.931 |
| `other` | 105 | 105 | 0.991 | 0.996 | 0.994 | 0.962 |
| 总体 | 2,918 | 3,107 | 0.979 | 0.978 | 0.988 | 0.936 |

- 当前主要短板是 `blue`，其 mAP50-95 比总体低约 0.027。
- `other` 指标最高，但验证实例仅 105 个，稳定性仍需通过完整 test 和真实视频
  复核。
- 验证标准输出：
  `logs/best_per_class_val_20260731_153826.stdout.log`。

## 2026-07-31 15:43 监控更新

- 主训练进程 PID 仍为 `4032`，存活且命令行保持为
  `1_PC_Training/scripts/train_yolo.py`。
- 已完成 epoch 39；epoch 40 正在运行，检查时推进至约
  `985/2558` batch（39%），不存在停止推进迹象。
- epoch 39：Precision=0.98012、Recall=0.97277、mAP50=0.98807、
  mAP50-95=0.93552。
- epoch 39 训练损失：box=0.41133、cls=0.29875、dfl=0.87621、
  angle=0.00327；验证损失：box=0.35483、cls=0.33851、
  dfl=0.82311、angle=0.00114。
- 当前最佳仍为 epoch 37，mAP50-95=0.93578；连续 2 个已完成 epoch
  未刷新最佳，但尚未接近 `patience=20` 的自动早停条件。
- `results.csv` 与 `last.pt` 均于 15:39:49 更新；`best.pt` 最近更新时间
  为 15:21:33，与最佳 epoch 37 一致。
- GPU 利用率 98%，显存 2,345/8,188 MiB，温度 56°C。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf 或 corrupt 数据；
  stdout 中的 `0 corrupt` 为正常数据扫描结果，stderr 无训练异常。

## 2026-07-31 15:53 监控更新

- 已完成 epoch 40；epoch 41 正在运行，检查时推进至约
  `1200/2558` batch（47%），训练进程 PID `4032` 正常存活。
- epoch 40：Precision=0.97582、Recall=0.97922、mAP50=0.98814、
  mAP50-95=0.93461。
- epoch 40 训练损失：box=0.40971、cls=0.29940、dfl=0.87769、
  angle=0.00319；验证损失：box=0.35480、cls=0.33710、
  dfl=0.82296、angle=0.00113。
- 当前最佳仍为 epoch 37（mAP50-95=0.93578），连续 3 个已完成 epoch
  未刷新最佳；距离 `patience=20` 的自动早停阈值仍有 17 轮。
- `results.csv` 与 `last.pt` 均于 15:49:04 更新；`best.pt` 未更新，
  最近更新时间仍为 15:21:33。
- GPU 利用率 64%，显存 2,342/8,188 MiB，温度 57°C；训练日志持续产生
  batch 进度。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 17:33 监控更新

- 已完成 epoch 51；epoch 52 正在运行，检查时推进至约
  `1506/2558` batch（59%），训练进程 PID `4032` 正常存活。
- epoch 51：Precision=0.97978、Recall=0.97534、mAP50=0.98844、
  mAP50-95=0.93662。
- epoch 51 训练损失：box=0.39568、cls=0.28466、dfl=0.87170、
  angle=0.00303；验证损失：box=0.35485、cls=0.33354、
  dfl=0.82186、angle=0.00111。
- **最佳模型已由 epoch 50 更新为 epoch 51**；mAP50-95 从 0.93642
  提升至 0.93662（+0.00020），早停等待计数重新归零。
- `results.csv`、`best.pt` 与 `last.pt` 均于 17:28:11 更新，时间与新最佳
  epoch 51 一致。
- GPU 利用率 81%，显存 2,327/8,188 MiB，温度 55°C；batch 持续推进。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 17:23 监控更新

- 已完成 epoch 50；epoch 51 正在运行，检查时推进至约
  `1182/2558` batch（46%），训练进程 PID `4032` 正常存活。
- epoch 50：Precision=0.97974、Recall=0.97539、mAP50=0.98836、
  mAP50-95=0.93642。
- epoch 50 训练损失：box=0.39804、cls=0.28724、dfl=0.87127、
  angle=0.00296；验证损失：box=0.35506、cls=0.33244、
  dfl=0.82202、angle=0.00112。
- **最佳模型已由 epoch 46 更新为 epoch 50**；mAP50-95 从 0.93621
  提升至 0.93642（+0.00021），早停等待计数重新归零。
- `results.csv`、`best.pt` 与 `last.pt` 均于 17:19:21 更新，时间与新最佳
  epoch 50 一致。
- GPU 利用率 85%，显存 2,327/8,188 MiB，温度 55°C；batch 持续推进。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 17:13 监控更新

- 已完成 epoch 49；epoch 50 正在运行，检查时推进至约
  `811/2558` batch（32%），训练进程 PID `4032` 正常存活。
- epoch 49：Precision=0.97627、Recall=0.97899、mAP50=0.98828、
  mAP50-95=0.93564。
- epoch 49 训练损失：box=0.39902、cls=0.28890、dfl=0.87410、
  angle=0.00299；验证损失：box=0.35490、cls=0.33227、
  dfl=0.82223、angle=0.00112。
- 当前最佳仍为 epoch 46（mAP50-95=0.93621）；连续 3 个已完成 epoch
  未刷新最佳，当前早停等待计数为 3/20。
- `results.csv` 与 `last.pt` 均于 17:10:28 更新；`best.pt` 最近更新时间
  为 16:43:52。
- GPU 利用率 88%，显存 2,327/8,188 MiB，温度 55°C；batch 持续推进。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 17:03 监控更新

- 已完成 epoch 48；epoch 49 正在运行，检查时推进至约
  `480/2558` batch（19%），训练进程 PID `4032` 正常存活。
- epoch 48：Precision=0.97625、Recall=0.97919、mAP50=0.98829、
  mAP50-95=0.93599。
- epoch 48 训练损失：box=0.40184、cls=0.28911、dfl=0.87421、
  angle=0.00310；验证损失：box=0.35490、cls=0.33306、
  dfl=0.82230、angle=0.00112。
- 当前最佳仍为 epoch 46（mAP50-95=0.93621）；epoch 47、48 连续两轮
  未刷新最佳，当前早停等待计数为 2/20。
- `results.csv` 与 `last.pt` 均于 17:01:35 更新；`best.pt` 最近更新时间
  为 16:43:52。
- GPU 利用率 79%，显存 2,326/8,188 MiB，温度 55°C；batch 持续推进。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 16:53 监控更新

- 已完成 epoch 46 和 epoch 47；epoch 48 正在运行，检查时推进至约
  `144/2558` batch（6%），训练进程 PID `4032` 正常存活。
- epoch 46：Precision=0.97596、Recall=0.97886、mAP50=0.98822、
  mAP50-95=0.93621；训练损失 box=0.40347、cls=0.29034、
  dfl=0.87628、angle=0.00322；验证损失 box=0.35511、cls=0.33678、
  dfl=0.82239、angle=0.00113。
- **最佳模型已由 epoch 45 更新为 epoch 46**；mAP50-95 从 0.93594
  提升至 0.93621（+0.00027），`best.pt` 于 16:43:52 更新。
- 最新 epoch 47：Precision=0.97601、Recall=0.97945、mAP50=0.98819、
  mAP50-95=0.93601；训练损失 box=0.40277、cls=0.28879、
  dfl=0.87459、angle=0.00318；验证损失 box=0.35499、cls=0.33384、
  dfl=0.82239、angle=0.00113。
- epoch 47 未刷新最佳；当前早停等待计数为 1/20。
- `results.csv` 与 `last.pt` 均于 16:52:42 更新；GPU 利用率 100%，
  显存 2,331/8,188 MiB，温度 55°C。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 16:43 监控更新

- 已完成 epoch 45；epoch 46 正在运行，检查时推进至约
  `2496/2558` batch（98%），训练进程 PID `4032` 正常存活。
- epoch 45：Precision=0.97617、Recall=0.97890、mAP50=0.98815、
  mAP50-95=0.93594。
- epoch 45 训练损失：box=0.40618、cls=0.29280、dfl=0.87685、
  angle=0.00319；验证损失：box=0.35514、cls=0.34047、
  dfl=0.82249、angle=0.00113。
- **最佳模型已由 epoch 37 更新为 epoch 45**；mAP50-95 从 0.93578
  提升至 0.93594（+0.00016），早停等待计数已重新归零。
- `results.csv`、`best.pt` 与 `last.pt` 均于 16:34:57 更新，三者时间与
  新最佳 epoch 45 一致。
- GPU 利用率 71%，显存 2,326/8,188 MiB，温度 56°C；epoch 46 已接近
  训练阶段结束，日志仍持续推进。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 16:33 监控更新

- 已完成 epoch 44；epoch 45 正在运行，检查时推进至约
  `2149/2558` batch（84%），训练进程 PID `4032` 正常存活。
- epoch 44：Precision=0.97576、Recall=0.97911、mAP50=0.98813、
  mAP50-95=0.93559。
- epoch 44 训练损失：box=0.40343、cls=0.29402、dfl=0.87468、
  angle=0.00314；验证损失：box=0.35486、cls=0.34034、
  dfl=0.82249、angle=0.00113。
- epoch 44 的 mAP50-95 较 epoch 43 再回升 0.00013，距 epoch 37
  最佳值 0.93578 仅差 0.00019，但尚未刷新 `best.pt`；已连续 7 个完成
  epoch 未刷新最佳。
- `results.csv` 与 `last.pt` 均于 16:26:03 更新；`best.pt` 最近更新时间
  仍为 15:21:33。
- GPU 利用率 100%，显存 2,326/8,188 MiB，温度 55°C；batch 持续推进。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 16:23 监控更新

- 已完成 epoch 43；epoch 44 正在运行，检查时推进至约
  `1830/2558` batch（72%），训练进程 PID `4032` 正常存活。
- epoch 43：Precision=0.97559、Recall=0.97974、mAP50=0.98812、
  mAP50-95=0.93546。
- epoch 43 训练损失：box=0.40781、cls=0.29536、dfl=0.87719、
  angle=0.00325；验证损失：box=0.35506、cls=0.33852、
  dfl=0.82259、angle=0.00113。
- epoch 43 的 mAP50-95 较 epoch 42 回升 0.00063，但仍低于 epoch 37
  最佳值 0.93578；已连续 6 个完成 epoch 未刷新最佳。
- `results.csv` 与 `last.pt` 均于 16:17:09 更新；`best.pt` 最近更新时间
  仍为 15:21:33。
- GPU 利用率 87%，显存 2,343/8,188 MiB，温度 55°C；batch 持续推进。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 16:03 监控更新

- 已完成 epoch 41；epoch 42 正在运行，检查时推进至约
  `1460/2558` batch（57%），训练进程 PID `4032` 正常存活。
- epoch 41：Precision=0.97760、Recall=0.97814、mAP50=0.98814、
  mAP50-95=0.93455。
- epoch 41 训练损失：box=0.40741、cls=0.29600、dfl=0.87662、
  angle=0.00332；验证损失：box=0.35472、cls=0.33392、
  dfl=0.82275、angle=0.00113。
- 当前最佳仍为 epoch 37（mAP50-95=0.93578），连续 4 个已完成 epoch
  未刷新最佳；距离 `patience=20` 自动早停阈值仍有 16 轮。
- `results.csv` 与 `last.pt` 均于 15:58:11 更新；`best.pt` 未更新，
  最近更新时间仍为 15:21:33。
- GPU 利用率 100%，显存 2,342/8,188 MiB，温度 58°C。
- 日志持续产生 batch 进度；未发现 OOM、Traceback、RuntimeError、
  NaN/Inf、非零 corrupt 计数或停止推进。

## 2026-07-31 16:13 监控更新

- 已完成 epoch 42；epoch 43 正在运行，检查时推进至约
  `1682/2558` batch（66%），训练进程 PID `4032` 正常存活。
- epoch 42：Precision=0.97559、Recall=0.97955、mAP50=0.98810、
  mAP50-95=0.93483。
- epoch 42 训练损失：box=0.40754、cls=0.29558、dfl=0.87819、
  angle=0.00311；验证损失：box=0.35485、cls=0.33574、
  dfl=0.82268、angle=0.00113。
- 当前最佳仍为 epoch 37（mAP50-95=0.93578），连续 5 个已完成 epoch
  未刷新最佳；距离 `patience=20` 自动早停阈值仍有 15 轮。
- `results.csv` 于 16:07:19 更新，`last.pt` 于 16:07:19 更新；
  `best.pt` 最近更新时间仍为 15:21:33。
- GPU 利用率 98%，显存 2,345/8,188 MiB，温度 54°C；日志持续推进。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 17:43 监控更新

- 已完成 epoch 52；epoch 53 正在运行，检查时推进至约
  `1849/2558` batch（72%），训练进程 PID `4032` 正常存活。
- epoch 52：Precision=0.97931、Recall=0.97553、mAP50=0.98841、
  mAP50-95=0.93648。
- epoch 52 训练损失：box=0.39671、cls=0.28166、dfl=0.87240、
  angle=0.00299；验证损失：box=0.35486、cls=0.33412、
  dfl=0.82173、angle=0.00111。
- 当前最佳仍为 epoch 51（mAP50-95=0.93662）；epoch 52 未刷新最佳，
  当前早停等待计数为 1/20。
- `results.csv` 与 `last.pt` 均于 17:37:05 更新；`best.pt` 最近更新时间
  为 17:28:11。
- GPU 利用率 81%，显存 2,327/8,188 MiB，温度 55°C；batch 持续推进。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 2026-07-31 17:53 监控更新

- 已完成 epoch 53；epoch 54 正在运行，检查时推进至约
  `2195/2558` batch（86%），训练进程 PID `4032` 正常存活。
- epoch 53：Precision=0.97938、Recall=0.97552、mAP50=0.98848、
  mAP50-95=0.93715。
- epoch 53 训练损失：box=0.39687、cls=0.28229、dfl=0.87114、
  angle=0.00299；验证损失：box=0.35502、cls=0.33225、
  dfl=0.82157、angle=0.00111。
- **最佳模型已由 epoch 51 更新为 epoch 53**；mAP50-95 从 0.93662
  提升至 0.93715（+0.00053），早停等待计数重新归零。
- `results.csv`、`best.pt` 与 `last.pt` 均于 17:45:56 更新，时间与新最佳
  epoch 53 一致。
- GPU 利用率 93%，显存 2,327/8,188 MiB，温度 54°C；batch 持续推进。
- 未发现 OOM、Traceback、RuntimeError、NaN/Inf、非零 corrupt 计数或
  停止推进。

## 训练终态与测试集评估

### 训练终态

- 用户于 2026-07-31 约 17:59 确认停止训练；当时 epoch 54 已完整结束，
  epoch 55 正运行至约 `956/2558` batch（37%）。
- Windows 普通进程树终止被数据加载子进程拒绝，核对 PID `4032` 及其
  24 个直接子进程均属于本次训练后，对该训练进程树执行强制终止。
- 终止后已复核：不存在 `train_yolo.py` 训练进程，GPU 显存回落至约
  50 MiB；未完成的 epoch 55 未写入 `results.csv`，不作为有效结果。
- 最终完整轮及最佳轮均为 epoch 54：Precision=0.98237、
  Recall=0.97433、mAP50=0.98846、mAP50-95=0.93734。
- epoch 54 训练损失：box=0.39434、cls=0.28121、dfl=0.86866、
  angle=0.00299；验证损失：box=0.35534、cls=0.33232、
  dfl=0.82138、angle=0.00111。
- 最终 `best.pt` 与 `last.pt` 文件大小均为 18,995,977 字节，SHA-256
  均为 `559EEFAD166B618D7C032F05E5C92942DDEC0268D0F5ED15D829DC1050084A0A`，
  表明二者均对应完整保存的 epoch 54 checkpoint。

### 冻结测试集评估

- 评估时间：2026-07-31 18:00–18:03。
- 模型：最终 epoch 54 `best.pt`；任务为 OBB，`split=test`、
  `imgsz=640`、`batch=16`、`device=0`。
- 测试集：9,231 张图片、11,010 个车牌实例、62 张背景图，`corrupt=0`。

| 类别 | 图片 | 实例 | Precision | Recall | mAP50 | mAP50-95 |
|---|---:|---:|---:|---:|---:|---:|
| `blue` | 4,064 | 5,711 | 0.974 | 0.906 | 0.987 | 0.932 |
| `green` | 5,006 | 5,006 | 1.000 | 0.984 | 0.995 | 0.937 |
| `yellow_single` | 249 | 256 | 0.958 | 0.918 | 0.952 | 0.860 |
| `other` | 37 | 37 | 0.902 | 0.919 | 0.948 | 0.924 |
| **总体** | **9,231** | **11,010** | **0.958** | **0.932** | **0.970** | **0.913** |

- 测试集总体 mAP50-95 为 0.913，低于平衡验证集 epoch 54 的 0.93734；
  测试集按类别宏平均，且 `yellow_single` 与 `other` 样本明显偏少。
- 测试集主要短板为 `yellow_single`（mAP50-95=0.860）；`blue` 的主要
  问题表现为 Recall=0.906，说明漏检风险高于其定位精度所反映的水平。
- `other` 仅 37 个测试实例，其 0.924 指标统计不稳定；应结合真实视频
  与更多独立特殊车牌样本复核，不能仅据该数值判断泛化能力。
- 推理速度：单图预处理 0.7 ms、模型推理 3.2 ms、后处理 2.0 ms；这是
  RTX 4060 Laptop GPU 上的 PC 评估速度，不代表 RK3568 板端速度。
- 评估输出：
  `scripts/runs/obb_test_results/yolov8n_plate_obb_640_test_epoch54`。
- 标准输出：`logs/yolo_obb_test_epoch54_20260731.stdout.log`；标准错误为空：
  `logs/yolo_obb_test_epoch54_20260731.stderr.log`。
- 测试生成了 PR/F1/P/R 曲线、混淆矩阵及 3 组标签/预测可视化，评估进程
  正常退出，未发现 OOM、Traceback、RuntimeError、NaN/Inf 或非零
  corrupt 计数。
