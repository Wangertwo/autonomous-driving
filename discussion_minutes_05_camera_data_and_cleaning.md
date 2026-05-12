# Camera 数据处理与智能驾驶数据清洗

本文整理关于 Camera 数据格式、Camera 数据处理流程，以及智能驾驶数据清洗工作的讨论。

---

## 1. Camera 数据不是普通图片

在智能驾驶数据处理中，Camera 数据不是单一图片文件，而是一组带时空语义的传感器数据。

可以理解为：

```text
图像 / 视频字节
+ 时间戳
+ 相机 ID
+ 坐标系 frame_id
+ 编码格式
+ 图像宽高
+ 内参
+ 外参
+ 畸变参数
+ 标注和场景元数据
```

最重要的原则：

```text
图像像素、时间戳、标定参数、坐标系、标注必须保持一致。
```

---

## 2. Camera 数据常见格式

Camera 数据通常包括两层。

### 2.1 原始图像 / 视频数据

常见形式：

```text
单帧图像
连续视频流
压缩视频片段
日志包中的图像 topic
```

常见格式：

```text
JPEG
PNG
RAW / Bayer
YUV
RGB / BGR
H.264 / H.265
MP4
sensor_msgs/Image
sensor_msgs/CompressedImage
Apollo Image message
MCAP Image topic
```

### 2.2 图像元数据

每帧通常包含：

```text
timestamp：时间戳
frame_id：坐标系 ID
camera_id：相机编号
width：图像宽度
height：图像高度
encoding：编码格式
step / stride：每行字节数
exposure：曝光时间
gain：增益
intrinsic：相机内参
extrinsic：相机外参
distortion：畸变参数
sequence_id：帧序号
topic / channel 名称
```

自动驾驶中，单张图片价值有限，真正有价值的是：

```text
图像像素 + 时间戳 + 相机标定 + 车辆状态 + 其他传感器同步关系。
```

---

## 3. 常见 Camera Topic / Channel

ROS2 中常见：

```text
/camera/front/image_raw
/camera/front/image_compressed
/camera/front/camera_info
/camera/rear/image_raw
/camera/left/image_raw
/camera/right/image_raw
```

Apollo / Cyber RT 中常见：

```text
/apollo/sensor/camera/front_6mm/image
/apollo/sensor/camera/front_12mm/image
/apollo/sensor/camera/left_fisheye/image
/apollo/sensor/camera/right_fisheye/image
/apollo/sensor/camera/rear/image
```

多相机系统通常包括：

```text
前视窄角相机
前视广角相机
后视相机
左前相机
右前相机
左后相机
右后相机
环视鱼眼相机
```

---

## 4. ROS Camera 消息

### 4.1 sensor_msgs/Image

非压缩图像消息，核心字段：

```text
header.stamp：时间戳
header.frame_id：相机坐标系
height：图像高度
width：图像宽度
encoding：图像编码
is_bigendian：字节序
step：每行字节数
data：图像字节数组
```

常见 encoding：

```text
rgb8
bgr8
mono8
mono16
rgba8
bgra8
yuv422
bayer_rggb8
bayer_bggr8
```

特点：

```text
数据量大
解码简单
适合直接算法处理
不适合长期大规模存储
```

### 4.2 sensor_msgs/CompressedImage

压缩图像消息，核心字段：

```text
header.stamp：时间戳
header.frame_id：相机坐标系
format：jpeg / png
data：压缩图像字节数组
```

特点：

```text
数据量小
需要解码
有压缩损失
适合日志存储和上传
```

### 4.3 sensor_msgs/CameraInfo

相机标定信息，包含：

```text
height / width
distortion_model
D：畸变参数
K：内参矩阵
R：校正矩阵
P：投影矩阵
binning_x / binning_y
roi
```

K 内参矩阵：

```text
fx  0  cx
0   fy cy
0   0  1
```

用于：

```text
去畸变
2D / 3D 投影
图像和点云对齐
BEV 转换
多相机拼接
```

---

## 5. Camera 数据标准处理流程

一个标准 Camera 数据处理流程可以分为以下步骤。

### 5.1 读取数据

来源：

```text
rosbag / rosbag2
Apollo record
MCAP
视频文件
图片目录
对象存储中的原始文件
```

读取时要拿到：

```text
图像字节
时间戳
camera_id
frame_id
topic / channel
encoding
宽高
序列号
```

### 5.2 解码图像

按格式解码：

```text
JPEG / PNG → RGB / BGR 图像矩阵
H.264 / H.265 → 视频帧
RAW / Bayer → 去马赛克后转 RGB
YUV → RGB / BGR
```

常用工具：

```text
OpenCV
FFmpeg
Pillow
PyAV
TurboJPEG
```

注意：

```text
OpenCV 默认 BGR；
深度学习框架通常用 RGB；
视频编码常见 YUV；
相机原始流可能是 Bayer。
```

颜色通道错误会严重影响训练。

### 5.3 图像质量检查

检查：

```text
黑屏
花屏
过曝
欠曝
模糊
雨滴
泥点
遮挡
图像尺寸异常
帧率异常
编码损坏
```

方法：

```text
亮度均值 / 方差
边缘清晰度
过曝像素比例
黑屏像素比例
图像 hash
解码异常捕获
```

经验：

```text
不要粗暴删除所有低质量图像。
雨夜、逆光、模糊可能是重要长尾；
黑屏、花屏、编码损坏更像设备或数据异常。
```

### 5.4 时间戳处理

检查：

```text
图像时间戳是否单调递增
帧间间隔是否符合预期
是否有掉帧
是否有重复帧
相机时间和系统时间是否有偏移
不同相机之间是否同步
Camera 与 LiDAR / Radar / Localization 是否同步
```

处理：

```text
按时间戳排序
去除重复帧
标记丢帧区间
估计时间偏移
与其他传感器做最近邻匹配
必要时插值低频状态量
```

图像本身通常不应随意插值生成训练帧。

### 5.5 标定处理

Camera 数据处理离不开：

```text
内参 intrinsic：fx, fy, cx, cy
畸变 distortion：k1, k2, p1, p2, k3
外参 extrinsic：camera_frame → vehicle_frame / base_link
```

需要检查：

```text
标定版本是否匹配
相机 ID 是否匹配
图像分辨率是否匹配内参
外参是否和车辆配置一致
畸变模型是否正确
```

标定错误会影响：

```text
图像去畸变
点云投影到图像
2D 框转 3D
BEV 生成
多相机拼接
仿真回放
```

### 5.6 去畸变与图像校正

流程：

```text
原始图像
  ↓
内参 + 畸变参数
  ↓
去畸变图像
```

普通前视相机和鱼眼相机处理方式不同。

鱼眼常见模型：

```text
fisheye model
equidistant model
Kannala-Brandt model
```

关键经验：

```text
训练和推理必须保持一致；
如果训练用去畸变图，推理也应保持相同几何处理。
```

### 5.7 多相机同步与拼接

需要处理：

```text
按时间戳对齐多相机帧
检查帧率一致性
检查是否某个相机掉帧
绑定相机外参
必要时做多相机拼接或 BEV 投影
```

同步策略：

```text
硬同步：多个相机硬件同步触发
软同步：后处理按时间戳最近邻匹配
容忍窗口：例如 20ms / 50ms 内认为可对齐
```

### 5.8 与其他传感器对齐

Camera + LiDAR 用于：

```text
点云投影到图像
3D 标注辅助
深度估计
多模态感知
自动标注
```

Camera + Radar 用于：

```text
速度辅助
远距离目标辅助
恶劣天气补充
```

Camera + Localization 用于：

```text
地图元素投影
车道线对齐
路口语义分析
轨迹回放
仿真构建
```

### 5.9 格式转换

常见转换：

```text
rosbag / record / MCAP → 图片序列 + JSON 元数据
视频 → 帧图片
图片序列 → WebDataset / TFRecord / LMDB
图像元数据 → Parquet
标注 → COCO / KITTI / nuScenes / OpenLABEL
```

典型输出结构：

```text
dataset/
  images/
    front/
      000001.jpg
      000002.jpg
  meta/
    frames.parquet
  calibration/
    camera_front.yaml
  labels/
    annotations.json
```

每帧元数据至少包含：

```text
frame_id
camera_id
timestamp
image_path
width
height
encoding
intrinsic_id
extrinsic_id
vehicle_id
log_id
scene_id
```

---

## 6. Camera 数据常见坑

```text
RGB / BGR 搞反
时间戳用错
标定版本不匹配
图像 resize 后内参未更新
压缩损失造成训练 / 推理分布差异
图像增强破坏几何关系
把真实困难场景误删为脏数据
```

尤其要注意：

```text
裁剪、resize、旋转图像时，2D 框坐标和内参 cx / cy / fx / fy 也要同步更新。
```

---

## 7. 数据清洗的定位

数据清洗在智能驾驶数据闭环里不是简单“删脏数据”，而是把车端回流的原始日志变成：

```text
可解析
可检索
可训练
可评测
可追溯
```

的数据资产。

数据清洗要回答三个问题：

```text
这份数据可信不可信？
这份数据适合做什么？
这份数据进入训练、评测、仿真之前还需要处理什么？
```

---

## 8. 数据清洗具体工作

### 8.1 文件完整性清洗

检查：

```text
文件是否上传完整
文件大小是否异常
压缩包能否解压
rosbag / record / MCAP 能否打开
索引是否损坏
分片文件是否缺失
校验和 checksum 是否一致
文件时间范围是否符合元数据
```

处理：

```text
重传
修复索引
标记损坏
剔除不可用文件
合并分片
去重归档
```

### 8.2 Topic / Channel 完整性清洗

检查关键 topic 是否存在：

```text
camera
lidar
radar
localization
chassis
perception
prediction
planning
control
hmi
diagnosis
```

处理：

```text
补充元数据
标记缺失 topic
降低数据等级
限制用途
剔除无法分析的数据
```

例如：

```text
缺 camera，但有 planning/control → 可用于规划控制分析，不适合感知训练
缺 hmi 接管信号 → 不适合作接管数据集
缺 localization → 不适合仿真回放和坐标分析
```

### 8.3 时间戳清洗

检查：

```text
时间戳是否单调递增
是否存在时间跳变
是否存在大量重复时间戳
是否存在未来时间或异常时间
传感器时间和系统时间是否一致
topic 之间时间范围是否重叠
消息 publish 时间和 record 时间差是否异常
```

处理：

```text
时间戳修正
时间偏移估计
异常片段裁剪
标记时间不可用
剔除无法对齐的数据
```

### 8.4 频率与丢帧清洗

检查：

```text
平均频率是否正常
最大帧间隔是否异常
是否存在连续丢帧
是否存在突发消息堆积
是否有 topic 长时间无数据
```

处理：

```text
标记丢帧区间
裁剪异常片段
插值补齐低频状态量
剔除严重缺帧数据
生成数据质量报告
```

经验：

```text
车辆速度、定位等低频状态量可以谨慎插值；
图像、点云、感知目标通常不应随意伪造补帧。
```

### 8.5 传感器质量清洗

Camera 检查：

```text
黑屏
花屏
过曝
欠曝
模糊
遮挡
雨滴 / 泥点
帧尺寸异常
编码格式异常
```

LiDAR 检查：

```text
点数异常少
点云空洞
反射强度异常
ring 缺失
时间戳异常
坐标范围异常
点云畸变
```

Radar 检查：

```text
目标数量异常
速度异常
距离异常
RCS 异常
目标 ID 跳变
虚警过多
```

GNSS / IMU 检查：

```text
定位跳变
姿态角异常
速度异常
加速度异常
IMU 饱和
GPS 信号丢失
定位置信度下降
```

CAN / Chassis 检查：

```text
车速异常
方向盘角异常
档位异常
刹车 / 油门信号异常
接管信号缺失
控制模式状态异常
```

### 8.6 坐标系与标定清洗

检查：

```text
frame_id 是否正确
外参版本是否匹配
内参版本是否匹配
TF tree 是否完整
坐标变换是否可用
传感器安装位置是否和车辆配置一致
地图坐标是否匹配
```

处理：

```text
绑定正确标定版本
修正 frame_id
标记标定异常
限制用途
剔除严重错位数据
```

### 8.7 消息结构与版本兼容清洗

检查：

```text
消息类型是否能解析
schema 版本是否匹配
字段是否缺失
字段单位是否变化
枚举值是否兼容
默认值是否合理
topic 名称是否变更
```

处理：

```text
schema registry 管理
版本转换
字段映射
单位统一
不可兼容数据隔离
```

### 8.8 元数据清洗

清洗字段包括：

```text
车辆 ID
车型
传感器配置
城市
道路类型
天气
采集时间
软件版本
模型版本
地图版本
驾驶模式
功能状态
接管原因
上传批次
任务 ID
```

处理：

```text
标准化字段
补全缺失值
关联车辆配置库
关联版本库
关联地图服务
重复数据合并
异常元数据标记
```

### 8.9 去重与样本均衡

重复来源：

```text
同一日志重复上传
同一事件被多个触发器捕获
连续片段高度重叠
相似普通场景过多
同一路口同一时间重复采集
```

去重方式：

```text
文件 hash 去重
事件 ID 去重
时间窗口重叠去重
轨迹相似度去重
图像 / 场景 embedding 去重
元数据组合去重
```

样本均衡是为了避免数据集被大量普通场景淹没。

### 8.10 脱敏与合规清洗

处理对象：

```text
人脸
车牌
地理位置
用户 ID
车辆 VIN
语音数据
手机蓝牙信息
行程轨迹
用户行为信息
```

处理方式：

```text
人脸 / 车牌打码
敏感字段脱敏
用户 ID 哈希化
精细地理位置降采样或权限隔离
数据访问审计
按权限分级开放
```

---

## 9. 清洗后的数据分级

清洗不是只有“保留 / 删除”。

更实际的是分级：

```text
A 级：完整、时间同步好、标定正确，可用于训练和评测
B 级：轻微缺失，可用于分析或部分训练
C 级：缺关键 topic，只能用于特定问题分析
D 级：严重损坏，仅归档或删除
```

也可以按用途打标签：

```text
可用于感知训练
可用于规划分析
可用于接管复盘
可用于仿真回放
可用于指标统计
不可用于训练
不可用于评测
需要人工复核
```

经验：

```text
某些数据对一个任务不可用，但对另一个任务仍然有价值。
```

---

## 10. 清洗输出物

一次清洗后通常产出：

```text
清洗后的原始数据或引用
标准化元数据
topic / channel 摘要
时间范围
频率统计
丢帧统计
质量评分
异常标签
可用性标签
数据用途标签
清洗日志
质量报告
```

示例：

```text
log_id: xxx
vehicle_id: car_001
city: shanghai
start_time: 2026-05-11 20:10:00
end_time: 2026-05-11 20:11:00
topics:
  camera_front: 30Hz, missing_rate 0.2%
  lidar_top: 10Hz, missing_rate 0%
  planning: 10Hz, missing_rate 0%
  control: 100Hz, missing_rate 0.1%
quality_score: 92
tags:
  - night
  - intersection
  - takeover
  - usable_for_planning_analysis
  - usable_for_simulation
warnings:
  - camera_right exposure unstable
```

---

## 11. 结论

Camera 数据处理的核心是：

```text
读取 → 解码 → 质量检查 → 时间同步 → 标定匹配 → 去畸变 → 多传感器对齐 → 格式转换 → 标注处理 → 数据增强 → 存储索引
```

数据清洗的核心是：

```text
文件完整性检查
topic / channel 完整性检查
时间戳清洗
频率和丢帧检查
传感器质量检查
坐标系和标定检查
消息结构和版本兼容处理
元数据标准化
去重与样本均衡
隐私脱敏与合规处理
数据质量评分和用途分级
```

最终经验：

```text
数据清洗不是把脏数据删掉，而是判断数据可信度、适用范围和进入训练 / 评测 / 仿真前的处理要求。
```
