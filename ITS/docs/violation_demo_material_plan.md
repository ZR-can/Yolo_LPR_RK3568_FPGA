# 行人闯红灯展示素材与方案

Date: 2026-07-20

## 1. 目标

比赛创新项要求：

```text
能完成行人的识别，并对行人违法行为进行标记：
行人闯红灯、行人横穿马路、车辆逆行等的一种或几种常见交通违法行为。
```

当前主线改为最保守、最容易解释的方案：

```text
YOLOv8n COCO 检测 person / traffic light / vehicle
+ 固定路口场景标定
+ 红灯状态输入或交通灯颜色粗判断
+ 行人进入过街区域
= 疑似行人闯红灯标记
```

本方案只面向比赛展示素材，不宣称对任意街景图泛化。

## 2. 推荐素材类型

最适合的素材不是普通街景图，而是固定路口视角图片或视频帧：

- 画面中有行人。
- 画面中能看到红灯，或者可以明确设定当前为红灯。
- 行人处于斑马线、路口中央或过街区域内。
- 摄像头固定，最好略俯视。
- 不使用横向远景、超深纵深、远近目标尺度差异特别大的图片作为主展示。

本项目主展示素材必须同时满足：

- 同一个固定路口视角，不能是手持随拍、行车记录仪、航拍或剪辑混合视角。
- 画面中能看到红绿灯或行人信号灯。
- 画面中有行人正在过马路，且行人不是远处极小目标。
- 能在同一画面中标定过街区域，例如斑马线、路口中央过街通道或人行横道。
- 最好能看到红灯状态；如果红灯状态不稳定可见，至少要能通过素材标题/片段语义说明这是红灯等待或闯红灯场景。

## 3. 推荐在线素材来源

### 3.0 本轮纠偏结论

前两轮候选里有几类素材不适合作为主展示：

- 普通 Pexels 街拍短视频：有红绿灯和行人氛围，但行人不一定真正过街，不能支撑闯红灯演示。
- 俯拍/航拍素材：行人和车辆容易识别，但交通灯通常不可见，不适合作为“红灯 + 行人过街”的主素材。
- 单独的行人信号灯图片数据集：适合测试灯色识别，不适合作为完整违法行为演示。
- Mixkit / 普通免费 stock video：通常只满足“人、车、灯同框”，但不一定满足“红灯期间行人正在过街”，不能再列为主素材。

本项目后续素材按下面优先级处理：

1. 第一优先级：直接使用国内素材站中标题明确包含“行人闯红灯 / 斑马线 / 红绿灯 / 过马路”的视频小样或授权素材。
2. 第二优先级：自己用手机在固定路口远距离拍摄 10 到 20 秒，要求能同时拍到信号灯和过街行人。
3. 第三优先级：使用固定路口普通素材，仅作为“行人过街区域标记”测试，不作为闯红灯主展示。

### 3.1 主候选：光厂/VJshi - 行人闯红灯素材

链接：

- 搜索页：https://www.vjshi.com/so/2098713.html
- 闯红灯搜索页：https://www.vjshi.com/so/2354053.html

目前最贴合的候选标题：

- `行人闯红灯过马路斑马线4K`
- `4k 红绿灯、斑马线、行人过马路、闯红灯`
- `红绿灯人行道 过马路 闯红灯 车流`
- `等红绿灯 闯红灯行人`
- `人行监控 闯红灯`
- `都市交通，行人闯红灯抓拍`
- `礼让行人人行道违法闯红灯（原创）`

为什么它们更符合要求：

- 标题已经同时包含了本项目所需的关键语义：红绿灯、斑马线/人行道、行人过马路、闯红灯。
- 多数为 4K 或固定机位实拍素材，比公开视频更适合抽帧测试。
- 可以先下载预览小样验证 YOLO 是否能识别 `person`，再决定是否购买授权。

限制：

- 多数需要购买授权或只能下载带水印小样。
- 需要逐个打开预览确认机位是否固定、红绿灯是否在画面内、行人是否足够大。

结论：

这是目前唯一可以作为“主展示素材”继续推进的在线来源。免费素材站和公开数据集暂时都不满足主展示要求。

### 3.2 自拍素材：最稳的免费方案

如果不想购买素材，建议直接自拍：

- 找一个带人行横道和行人信号灯的路口。
- 手机固定在三脚架、栏杆或路边安全位置，不要手持移动。
- 横屏录制 1080p 或 4K，时长 10 到 20 秒。
- 画面必须同时包含：
  - 行人信号灯或红绿灯。
  - 斑马线或过街区域。
  - 等待或过街的行人。
- 后续从视频中抽取 3 到 5 帧，作为板端测试图片。

这个方案最容易满足比赛现场要求，因为画面角度、行人大小、红绿灯位置都可控，也不存在下载授权和素材不匹配问题。

### 3.3 辅助候选：Pexels - Pedestrian Traffic Light Changing Sequence

链接：

- https://www.pexels.com/video/pedestrian-traffic-light-changing-sequence-30404309/

用途：

- 免费素材，页面说明为交通灯从红变绿，同时行人行走。
- 适合测试行人信号灯红/绿状态变化，或者作为 PPT 里“灯态识别增强”的辅助素材。

限制：

- 如果画面主要是交通灯特写而不是完整路口，不能作为主闯红灯检测素材。
- 更适合作为灯态裁剪/颜色阈值的验证材料。

### 3.4 不作为主素材：LISA Traffic Light Dataset

链接：

- https://www.kaggle.com/datasets/mbornoe/lisa-traffic-light-dataset

用途：

- 适合寻找包含交通灯的真实道路图像。
- 可用于测试 `traffic light` 检测和红灯颜色裁剪判断。
- 不一定天然包含行人闯红灯动作，需要人工筛选有行人和红灯同框的帧。

### 3.5 不作为主素材：Pedestrian Traffic Light Dataset

链接：

- https://www.kaggle.com/datasets/wjybuqi/pedestrian-traffic-light-dataset

用途：

- 更贴近“行人信号灯”场景。
- 适合寻找红色行人灯、过街区域、行人相关图片。
- 可以作为行人闯红灯规则展示素材的优先候选。

### 3.6 不作为主素材：Urban Tracker Dataset

链接：

- 页面：https://www.jpjodoin.com/urbantracker/dataset.html
- St-Marc 样例图：https://www.jpjodoin.com/urbantracker/dataset/stmarc/sample.png
- St-Marc 视频：https://www.jpjodoin.com/urbantracker/dataset/stmarc/stmarc_video.avi
- Rouen 样例图：https://www.jpjodoin.com/urbantracker/dataset/rouen/sample.png
- Rouen 视频：https://www.jpjodoin.com/urbantracker/dataset/rouen/rouen_video.avi

用途：

- 固定监控视角，适合人和车检测。
- 适合做“行人进入过街区域”或“人车冲突风险”的对照展示。
- 如果画面中没有清楚红灯，可通过配置文件设置当前灯态为红灯，作为规则验证素材。

### 3.7 论文素材：Pedestrian Red-Light Violation Dataset / 论文素材

可搜索关键词：

```text
pedestrian red light violation dataset
pedestrian traffic light violation detection dataset
行人 闯红灯 数据集 路口 监控
```

用途：

- 可作为答辩 PPT 的背景引用。
- 不建议作为第一优先，因为下载门槛、格式和授权通常不如 Kaggle / Urban Tracker 清楚。

## 4. 展示素材建议

只准备 3 组素材即可：

1. `red_light_person_crossing`
   - 红灯状态。
   - 行人在斑马线或过街区域内。
   - 输出：`suspected_pedestrian_red_light_violation`。

2. `red_light_no_person_crossing`
   - 红灯状态。
   - 没有行人进入过街区域。
   - 输出：无闯红灯告警。

3. `green_light_person_crossing`
   - 绿灯状态或配置为绿灯。
   - 行人在过街区域内。
   - 输出：无闯红灯告警。

如果找不到真实绿灯图，第三张可以用同一类路口素材，配置 `light_state = green` 做规则对照。

主展示建议优先用图片帧而不是完整视频：从视频中抽取 3 到 5 帧，保证 YOLO 能稳定检测到 `person`，并让 `crossing_area` 标定清楚。完整视频可以作为加分演示，但不要让现场效果完全依赖视频中每一帧都识别成功。

素材验收标准：

- `red_light_person_crossing` 必须是合格主素材，不能用交通灯特写、航拍、远处小人或没有过街行人的街景替代。
- `red_light_no_person_crossing` 和 `green_light_person_crossing` 可以用同一固定路口的相邻帧或同类机位素材完成对照。
- 如果只拿到普通固定路口素材，最多只能证明“行人过街区域标记”，不能在答辩中说它是真实闯红灯素材。

## 5. 板端规则设计

### 5.1 最稳展示版

先不依赖自动识别红灯颜色，使用配置文件或命令行设置灯态：

```json
{
  "light_state": "red",
  "crossing_area": [[0.18, 0.42], [0.82, 0.42], [0.98, 0.92], [0.04, 0.94]],
  "min_person_box_height_ratio": 0.045
}
```

规则：

```text
if light_state == red
and person is detected
and person box height >= min_person_box_height_ratio * image_height
and person bottom-center is inside crossing_area:
    mark suspected_pedestrian_red_light_violation
```

优点：

- 现场最稳定。
- 不受交通灯太小、曝光、颜色偏差影响。
- 能直接满足“完成行人的识别，并对行人闯红灯进行标记”。

答辩表述：

```text
当前版本在固定路口监控场景下，通过信号灯状态输入与行人检测框位置关系，
对疑似行人闯红灯行为进行规则化标记。
```

### 5.2 半自动增强版

在 YOLO 检测到 `traffic light` 后，对交通灯框内部做红色像素比例判断：

```text
detect traffic light
crop traffic light box
convert to HSV or RGB threshold
if red pixels exceed threshold:
    light_state = red
else:
    light_state = unknown / green_candidate
```

建议只作为增强，不作为现场唯一判断依据。

原因：

- 交通灯目标常常很小。
- 公开图片曝光和颜色差异大。
- COCO 的 `traffic light` 只检测灯，不直接输出红/绿状态。

## 6. 与当前 V0.14 的关系

当前 V0.14 已经完成：

```text
YOLO 交通类别过滤
person / vehicle 检测计数
person-vehicle spatial conflict risk
```

下一版 V0.15 建议新增：

```text
suspected_pedestrian_red_light_violation
```

V0.14 的人车冲突风险保留为辅助提示，不再作为创新项主线。

## 7. 推荐实现顺序

1. 新增 `traffic_violation_config.json`。
2. 支持 `light_state = red / green / unknown`。
3. 支持一个 `crossing_area` 多边形。
4. 在 `traffic_scene.cc` 中新增行人闯红灯规则。
5. 输出图中用红色文字和红色框突出违法行人。
6. README 记录 V0.15。

## 8. 推荐展示话术

推荐说：

```text
系统使用 RK3568 NPU 上的 YOLOv8n COCO 模型识别行人和交通灯。
在固定路口监控视角下，系统结合红灯状态和过街区域标定，
判断行人是否在红灯期间进入过街区域，从而标记疑似行人闯红灯行为。
该方案不需要重新训练违法行为模型，具有部署简单、可解释性强、适合嵌入式平台的特点。
```

不要说：

```text
系统可以对任意道路图片自动识别所有闯红灯行为。
系统已经完全理解红绿灯、斑马线和所有交通规则。
```
