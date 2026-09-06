# FPGA 快速图形识别测试平台（Cam_to_YUV）使用说明

> 本文件是工程 `Cam_to_YUV` 的使用说明（README），与同目录下的两张参考资料配套：
>
> - `sysframework.png` —— 系统框架图（图 1），本平台现状与规划模块的总体框图；
> - `2_【正点原子】领航者ZYNQ之嵌入式Vitis开发指南V1.2.pdf` —— 正点原子《领航者 ZYNQ 之嵌入式 Vitis 开发指南 V1.2》节选（书中第 30~35 章：OV7725 LCD 显示、OV7725 HDMI 显示、OV5640 灰度显示、OV5640 二值化、OV5640 中值滤波、OV5640 边缘检测），本文第 7 章“新模块接入指南”即是在通读该 PDF 后整理得出的。
>
> 注：该 PDF 约 4.5 MB，属本地配套参考资料（`.gitignore` 已排除，**不入库**）；它与其完整版可从正点原子官方资料渠道获取。本 README 第 7 章内容不受影响。

---

## 1. 项目简介

本工程是一个**基于 ZYNQ（领航者开发板，XC7Z020）的 FPGA 快速图形识别测试平台**：

- **当前已实现的能力（测试基线）**：OV5640 摄像头（DVP 接口，RGB565 输出）实时采集图像，在 PL 内转为 24 bit RGB888 并做 RGB→YCbCr 灰度转换，经 `v_vid_in_axi4s` → VDMA 写入 DDR3 帧缓存；VDMA 再循环读出，经 `v_axi4s_vid_out`（由 VTC 产生时序）→ `rgb2lcd` 驱动 RGB LCD，把摄像头画面（当前为灰度图）实时回显，同时 DDR 中保存**处理后的完整帧**，可供 PS 侧随时读取/验证。全流程由 PS（ARM Cortex-A9）通过 AXI 配置 VDMA、VTC、时钟 IP 和摄像头寄存器。
- **最终目标（规划中）**：以 PL 算法流水线实现“快速图形识别”——

  > 原图像 → 滤波（去噪）→ RGB 转 HSV → 阈值分割 → 连通域标记与检测（边界周长/边长、面积）→ 形状识别（依据周长-面积关系等特征判别圆形/矩形/三角形…）→ 每帧把识别到的**每个连通区域的颜色、周长（边长）、面积**（以及形状类别）上报给 PS 端。

  平台的作用是：先用现有“采集 → 处理 → DDR → 回显”通路把摄像头、存储、显示、PS 通信这些**平台能力**跑通并固定下来，之后把上表中的算法逐个做成教材式的“VIP 图像处理模块”（见第 7 章）串入视频流水线验证；复杂算法可在 PS 侧读取 DDR 中的原始帧先行仿真定标，再下移 PL 实现。

工程名 `Cam_to_YUV` 取自已实现的 RGB→YCbCr（YUV）转换环节；注意当前仓库中的 `rgb2ycbcr` 模块只输出亮度 Y（灰度）通道，Cb/Cr 的获取与上送方式见 7.3 节。

---

## 2. 系统框架与数据流

![系统框架图](sysframework.png)

**图 1 系统框架图**（图内 PL/PS 分区：PL 实现视频采集、处理与显示链路；PS 实现摄像头配置、寄存器控制和结果读取；虚线框为规划的识别算法模块）

### 2.1 视频数据通路（现状，已在板上跑通）

```
OV5640(DVP 8bit)
   │  cam_pclk / cam_vsync / cam_href / cam_data[7:0]
   ▼
ov5640_capture_data_0         8bit→RGB565→RGB888；输出视频总线：
   │  vid_clk, vid_vsync, vid_ce, vid_active_video, vid_data[23:0]
   ▼  （像素时钟域 = cam_pclk）
rgb2ycbcr_0                   灰度处理（仅取 Y，输出 {Y,Y,Y} 24bit）
   │  ycbcb_vsync / ycbcbr_clken / ycbcr_valid / gray_data[23:0]
   ▼
v_vid_in_axi4s_0              “Video In to AXI4-Stream”，异步时钟，native 视频 → AXI4-Stream
   ▼  S_AXIS_S2MM
axi_vdma_0（写通道 S2MM）     经 AXI Interconnect → S_AXI_HP0 → DDR3
   ▼
DDR3 帧缓存区                 0x0110_0000 起，RGB888 24bit，3 帧循环
   ▼
axi_vdma_0（读通道 MM2S）     AXI4-Stream (24bit) ← DDR
   ▼  M_AXIS_MM2S
v_axi4s_vid_out_0             “AXI-Stream to Video Out”，VTC 供给 vtiming，输出 RGB888 视频总线
   ▼  vid_io_out（像素时钟域 = clk_wiz 输出 clk_out1）
rgb2lcd_0                     RGB → LCD 时序与三态数据引脚
   ▼
RGB LCD（4.3″/7″/10.1″，由 LCD ID 自动适配分辨率）
```

要点：

- VDMA `tdata` 位宽为 **24 bit**（与 RGB888 一致），`c_num_fstores=3`（3 帧缓存），MM2S/S2MM 突发 64，Gen-Lock 模式，循环传输。
- 像素时钟域与 AXI 时钟域异步：采集/处理链跑在 `cam_pclk`（OV5640 PCLK）；`v_vid_in_axi4s` 的 `aclk`、VDMA、DDR 侧为 PS `FCLK_CLK0`（100 MHz）；显示侧像素时钟由 `clk_wiz_0` 动态重配输出（800×480 时约 33.3 MHz，其它分辨率见 `lcd_modes.h`）。
- `clk_wiz_0.locked` 同时作为采集/灰度模块的复位释放信号（`rst_n`）。

### 2.2 控制通路（PS → PL / 摄像头）

- **摄像头寄存器配置**：PS 的 EMIO GPIO（EMIO 54=SCL、55=SDA）软件模拟 SCCB/IIC 时序（`emio_sccb_cfg`），对 OV5640 写初始化序列（`ov5640_init`）。
- **LCD ID 识别**：AXI GPIO（0x4120_0000）通道 1 读出 LCD 屏 ID（M2:M1:M0），据此选择视频模式（480×272 / 800×480 / 1024×600 / 1280×800）。
- **VDMA / VTC / clk_wiz 配置**：PS 经 `M_AXI_GP0 → ps7_0_axi_periph` 访问各 IP 的 AXI-Lite 寄存器。
- **人机输出**：UART0（MIO14/15）打印启动信息；LCD 实时显示画面。

### 2.3 规划的识别算法链（PL 内逐级接入，见图 1 规划框）

```
原图RGB → 滤波(中值，去椒盐/孤立点)
        → RGB转HSV（颜色更稳定地描述物体，为“颜色”属性服务）
        → 颜色阈值分割（或灰度二值化）
        → 连通域标记 → 统计每区域: 面积、边界周长(边长)、平均颜色(HSV/RGB)
        → 形状识别（周长/面积、圆度、矩形度等判据）
        → 结果上报 PS（见 7.3 结果流格式）
```

该链上各 PL 模块的接入方法统一遵循第 7 章规范；建议按“滤波 → HSV → 分割/连通域 → 结果上送”的顺序逐步插入、逐步在 LCD/串口上验证。

---

## 3. 目录结构

```
Cam_to_YUV/
├─ README.md                仓库首页/入口（GitHub 主页展示）
├─ doc/                     本文档与参考资料
│  ├─ README.md             本使用说明
│  ├─ sysframework.png      系统框架图（图 1）
│  └─ 2_【正点原子】…Vitis开发指南V1.2.pdf   参考教材节选（本地资料·不入库）
├─ rtl/                     RTL 与自定义 IP 仓库
│  └─ ip_repo/              自定义 IP 源（Vivado IP 仓库）
│     ├─ ov5640_capture_data/   摄像头采集：DVP 8bit→RGB565→RGB888+视频总线
│     ├─ rgb2ycbcr/             颜色空间转换：RGB888→YCbCr（现输出 Y 灰度）
│     └─ rgb2lcd/               RGB 视频总线→RGB LCD 引脚/三态
├─ prj/                     Vivado / Vitis 工程目录（Vitis 工作空间即此目录）
│  ├─ cam_sys/              Vivado 2020.2 工程（Block Design 名 cam_sys）
│  │  ├─ cam_sys.xpr        工程文件（打开入口）
│  │  ├─ cam_sys.srcs/…/cam_sys.bd   块设计（连线/参数/地址）
│  │  ├─ cam_sys.srcs/constrs_1/new/cam_constr.xdc  引脚约束
│  │  ├─ cam_sys.runs/impl_1/cam_sys_wrapper.bit    比特流（工具生成·不入库）
│  │  └─ cam_sys_wrapper.xsa        硬件规格 XSA（工具生成·不入库）
│  ├─ cam_sys_platform/     Vitis 平台工程（工具生成·不入库，见附录 A 重建）
│  ├─ cam_screen/           Vitis 应用工程（仅 src/ 源码入库，入口 src/main.c）
│  └─ cam_screen_system/    系统工程（工具生成·不入库）
└─ sim/                     仿真目录（预留：各算法模块的 testbench 放这里）
```

> 版本管理说明：本仓库按“仅源码”策略提交（见根目录 `.gitignore`）——入库的是 `rtl/`、`doc/`、`prj/cam_sys` 的工程文件与 BD 源码、`prj/cam_screen/src`；综合/导出/平台等生成物只在本机生成、不入库，全新克隆后请按**附录 A** 重建。

---

## 4. 快速开始（复现“摄像头灰度 → LCD 实时显示”）

### 4.1 硬件准备

| 项目 | 说明 |
|---|---|
| 开发板 | 正点原子 领航者 ZYNQ（XC7Z020），启动模式按 JTAG 调试设置 |
| 摄像头 | OV5640（DVP 8bit），插入板上摄像头插座，**镜头朝外** |
| 显示屏 | RGB LCD（4.3″480×272 / 4.3″800×480 / 7″800×480 / 7″1024×600 / 10.1″1280×800 均可），FPC 排线接 RGB TFT-LCD 接口 |
| 调试 | Xilinx 下载器（JTAG）、USB-UART 线（串口 115200-8-N-1） |
| 软件 | Vivado / Vitis 2020.2（工程由 2020.2 建立） |

### 4.2 比特流与硬件规格（XSA）

Vivado 2020.2 打开 `prj/cam_sys/cam_sys.xpr` → 左侧 Flow Navigator → `Generate Bitstream`（工程已设置好 IP 仓库与约束）→ 完成后 `File → Export Hardware`，**勾选 Include bitstream**，输出 `cam_sys_wrapper.xsa`（覆盖 `prj/cam_sys/cam_sys_wrapper.xsa`）。

> 注：`.bit`/`.xsa` 为工具生成物，已被仓库 `.gitignore` 排除、不随仓库分发；克隆到新机器后必须先执行本步（见附录 A）。本机已生成过且 RTL 未改动时，可直接复用本地现有文件、无需重新综合。

### 4.3 Vitis 运行

> 前提：`cam_sys_platform`、`cam_screen`、`cam_screen_system` 为工具生成/编译工程、不入库；本机原工作空间已有这些工程时按下述步骤直接运行，全新克隆的机器请先按**附录 A** 重建一次。

1. 打开 Vitis 2020.2，工作空间指向 **`prj`**（该目录即平台/应用工程所在工作空间）。
2. 已有：平台工程 `cam_sys_platform`（若导出了新 XSA，双击平台工程 → Update Hardware Specification 指向新 XSA 后重新 Build）、应用工程 `cam_screen`。
3. 连接 JTAG/串口/摄像头/LCD 并上电 → 对 `cam_screen` 执行 Run（或 Debug），在域 `standalone_ps7_cortexa9_0` 上运行。
4. 若改动过 PL 且重新生成了平台，先在系统工程 `cam_screen_system` 上 Build，再 Run。

### 4.4 预期现象

- 串口依次打印 `LCD ID: xxx`（如 `7084`、`7016`…）与 `OV5640 detected successful!`；
- LCD 显示摄像头实时灰度画面，无撕裂（3 帧缓存）；
- 分辨率自适应：LCD ID 决定摄像头输出尺寸与显示模式（表见 5.3 节）。

---

## 5. PS 端软件说明（`cam_screen`）

### 5.1 代码结构

| 文件/目录 | 功能 |
|---|---|
| `src/main.c` | 主流程：读 LCD ID → 初始化 EMIO/SCCB → 配置 OV5640 → 配置 VDMA → 动态配置像素时钟 → 启动 VTC 显示 |
| `src/display_ctrl/` | 显示控制：`XVtc` 驱动封装 + `lcd_modes.h`（各分辨率时序参数/像素时钟频率）+ `lcd_id_read()` 读屏 ID |
| `src/vdma_api/` | `run_vdma_frame_buffer()`：VDMA S2MM/MM2S 循环帧缓存配置（Gen-Lock、3 帧、突发等） |
| `src/clk_wiz/` | `clk_wiz_cfg()`：动态重配 MMCM 分频，输出与 `VideoMode.freq` 一致的像素时钟 |
| `src/emio_sccb_cfg/` | EMIO GPIO 位号与软件 SCCB/IIC 时序（Start/Stop/读写字节/ACK） |
| `src/ov5640/` | `ov5640_init()`：OV5640 寄存器初始化表（含尺寸/总像素参数）与 ID 校验 |

### 5.2 软件启动流程

1. `XGpio` 初始化 AXI GPIO → 通道 1 置输入 → `lcd_id_read()` 得屏 ID → 恢复输出方向；
2. `emio_init()`（EMIO 54/55 置输出高电平）→ `ov5640_init(h_pixel, v_pixel, total_h, total_v)`，按屏 ID 选择摄像头输出尺寸（如 800×480 配 1800×1000 总像素、1024×600 配 2200×1000、1280×800 配 2570×980）；
3. `run_vdma_frame_buffer(...BOTH)`：S2MM+MM2S 同时开启，帧缓存基址 `0x0110_0000`（= DDR 基址 `0x0010_0000` + `0x100_0000`），3 帧 × `宽×高×3B` 循环；
4. `clk_wiz_cfg()` 把像素时钟配到 `VideoMode.freq`（如 800×480 → 33 MHz）；
5. `DisplayInitialize → DisplaySetMode → DisplayStart` 启动 VTC 发生显示时序。

> 提示：后续把识别结果送给 PS 时，识别任务应放在 `DisplayStart` 之后的循环/中断里执行（见 7.3），不要在初始化序列中阻塞。

### 5.3 分辨率与地址速查

| LCD ID | 屏 | 视频模式 | 摄像头输出 |
|---|---|---|---|
| 0x4342 | 4.3″ 480×272 | VMODE_480x272（像素时钟 9 MHz） | 480×272 |
| 0x4384 / 0x7084 | 4.3″/7″ 800×480 | VMODE_800x480（33 MHz） | 800×480 |
| 0x7016 | 7″ 1024×600 | VMODE_1024x600（50 MHz） | 1024×600 |
| 0x1018 | 10.1″ 1280×800 | VMODE_1280x800 | 1280×800 |

PS 外设地址（`xparameters.h` / BD 地址编辑器）：AXI GPIO `0x4120_0000`；AXI VDMA `0x4300_0000`；clk_wiz `0x43C0_0000`；VTC `0x43C1_0000`；DDR 空间至 `0x3FFF_FFFF`；帧缓存 `0x0110_0000`。

---

## 6. PL 端硬件说明（Block Design `cam_sys`）

### 6.1 BD 模块一览

| 模块 | 类型 | 作用/关键参数 |
|---|---|---|
| `processing_system7_0` | PS7 | 双核 A9（666 MHz），DDR3（533 MHz），UART0(MIO14/15)，EMIO GPIO×2，FCLK0=100 MHz，HP0 使能 |
| `axi_vdma_0` | Xilinx | 24 bit 流，S2MM+MM2S，3 帧缓存，突发 64，Gen-Lock；S2MM/MM2S 经 `axi_mem_intercon` 走 HP0 |
| `v_vid_in_axi4s_0` | Xilinx | native 视频(RGB888，cam_pclk 域) → AXI4-Stream(100 MHz 域)，异步 |
| `v_axi4s_vid_out_0` | Xilinx | AXI4-Stream → RGB888 视频输出（像素时钟域），VTC 供时序，异步 |
| `v_tc_0` | Xilinx | 视频时序发生器（只生成，不检测） |
| `clk_wiz_0` | Xilinx | 100 MHz→像素时钟，动态重配使能；locked 兼作采集/灰度模块复位释放 |
| `axi_gpio_0` | Xilinx | 3 bit GPIO（LCD ID） |
| `ov5640_capture_data_0` | 自定义 | OV5640 DVP 采集与 RGB565→RGB888 |
| `rgb2ycbcr_0` | 自定义 | RGB888→灰度（Y），3 拍流水 |
| `rgb2lcd_0` | 自定义 | 视频总线→LCD 引脚（含屏 ID 读回三态） |
| `rst_ps7_0_100M` | Xilinx | 100 MHz 域复位 |
| `ps7_0_axi_periph` / `axi_mem_intercon` | Xilinx | AXI 互联/地址译码 |

顶层端口（`cam_sys_wrapper`）：`cam_data[7:0]/cam_pclk/cam_vsync/cam_href/cam_rst_n/cam_pwdn`、`emio_sccb_tri_io[1:0]`、`lcd_rgb_tri_io[23:0]`、`lcd_clk/hs/vs/de/bl/rst`、DDR/FIXED_IO。引脚位置见 `cam_constr.xdc`（LVCMOS33）。

### 6.2 自定义 IP 端口（新模块必须与其对齐，详见 7.2）

`rgb2ycbcr_0` 端口（处理模块端口范本）：

```
输入: clk, rst_n(低有效),
     rgb_vsync,          // 帧同步
     rgb_clken,          // 像素时钟使能（行有效期内逐像素）
     rgb_valid,          // 数据有效
     rgb_data[23:0]      // RGB888
输出: ycbcb_vsync, ycbcbr_clken, ycbcr_valid,      // 与输入同名信号延迟 N 拍
     gray_data[23:0]     // = {Y,Y,Y}
```

灰度公式（模块内注释，BT.601 定点）：`Y = (77R + 150G + 29B) >> 8`；Cb/Cr 公式同在模块注释中（当前仅取 Y）。模块为三级乘法/加法/移位流水，因此 vsync/clken/valid 各打 3 拍与数据对齐。

---

## 7. 新模块接入指南（通读《正点原子 Vitis 开发指南》后的结论）

> 本节依据本仓库 `doc/` 下 PDF（书中第 30~35 章）整理。教材里“灰度 / 二值化 / 中值滤波 / 边缘检测”实验与本平台是同一套架构（OV5640 → 采集 → **VIP 处理模块** → Video In to AXI4-Stream → VDMA → DDR → 回显 LCD），每个实验只是**在图像采集模块与 `v_vid_in_axi4s` 之间多串一个“VIP (video image process)”图像处理 IP**，软件则“与 OV5640 摄像头 LCD 显示实验完全相同，不再赘述”。教材第 33~35 章（二值化 / 中值 / Sobel）是自封装 VIP 的完整教程，可直接作为本平台“滤波 → HSV → 连通域检测”各模块的开发模板。

### 7.0 结论速查（三要素一览）

| 要素 | 结论 |
|---|---|
| **接入位置** | ① 视频处理模块：串入 `ov5640_capture_data_0`（或其后级处理模块）与 `v_vid_in_axi4s_0` 之间（教材 32/33/34/35 章标准做法）；② 帧级/验证：PS 直接读 DDR 帧缓存区（0x0110_0000，RGB888）；③ 结果回传：识别模块输出 → 新 AXI-Lite/GPIO 寄存器组挂 `ps7_0_axi_periph` 新开 master 口（地址编辑器分配，PS 经 GP0 读） |
| **端口形式** | 行流水模块用“native 视频 4 线 + 数据”端口：`vsync`(帧) + `clken`(像素使能) + `valid`(数据有效) + `data[23:0]`，配 `clk`(像素时钟)/`rst_n`(低有效)；同步信号必须随数据**打拍对齐 N 拍**（数据延迟 N 拍的模块，其 vsync/clken/valid 也寄存 N 拍）。控制参数可先做 Verilog `parameter`（教材做法），需 PS 可调时升级为 AXI4-Lite 从口 |
| **数据流格式要求** | 视频链为 **RGB888、24 bit/像素、1 像素/时钟**，灰度内容以 `{Y,Y,Y}` 占满 24 bit（VDMA 流位宽 24 bit、3 帧缓存、循环传输）；处理完的帧自动入 DDR 并被 LCD 回显；识别结果每帧一个“结果包”（帧头+若干区域记录）经寄存器组按帧同步节拍上送 PS（格式见 7.3） |

### 7.1 接入位置（三种，按用途选择）

**P1 视频流水线串入位置（推荐给所有逐像素/逐行算法）**

```
ov5640_capture_data_0 ─▶ 【新增 VIP：滤波→HSV→分割…】 ─▶ rgb2ycbcr_0 ─▶ v_vid_in_axi4s_0
      （删掉原自动高亮连线，把新 IP 串入，其余 BD 不变）
```

- 按教材第 34/35 章流程：在 BD 中删除 capture（或现有 `rgb2ycbcr_0`）与 `v_vid_in_axi4s_0` 之间的连线，从 IP Catalog 搜索添加新封装的 VIP，手动连接并 `Validate Design`。
- 也可以先保持灰度回显通路不变，把新模块做成“并联观察支路”（与 `rgb2ycbcr_0` 同入同出，加选通）用于对照，验证通过后再正式串入。
- 优点：处理帧自动进 DDR、自动上屏，PS 软件不改即可先看到处理效果（教材各章均如此）；后续 PS 可从 DDR 帧里核对像素级结果。

**P2 PS 端软件算法位置（验证/定标用）**

DDR 帧缓存区 `0x0110_0000` 起按 `宽×高×3B` 保存最近 3 帧处理结果（RGB888），PS 可直接按地址读取做离线算法（阈值标定、连通域参考实现等），**不需改 PL**。注意读取前 `Xil_DCacheInvalidateRange()`，并避开正在写的帧（可查 VDMA 帧计数寄存器，或简化处理为“只读最旧一帧”）。

**P3 结果上报 PS 的位置（识别模块最终输出）**

连通域/形状识别模块按帧产生少量结果（每区域：颜色、周长、面积、形状码），**不适合也无需走 DDR 帧**。推荐：

- 在 `ps7_0_axi_periph` 上新增一个 master（M04），挂自定义 AXI-Lite 从机“结果寄存器组”IP（或复用 AXI GPIO 通道）；Vivado Address Editor 自动分配地址（建议落在 `0x43C2_0000` 段，64K）；
- PS 轮询寄存器组；需要节拍通知时，识别模块在“新结果就绪”时发一帧脉冲，经 `IRQ_F2P`（PL-PS 中断，接 GIC）通知 PS（中断配置方法见教材 Vitis 基础章节，不在本 PDF 节选范围内）；
- 联调阶段也可先用 UART 打印结果（xil_printf），跑通后再接寄存器通路。

### 7.2 端口形式（新模块对外接口规范）

**（a）行级流水视频口（默认形式，与 `rgb2ycbcr` 完全一致）**

| 方向 | 信号 | 位宽 | 说明 |
|---|---|---|---|
| 输入 | `clk` | 1 | 像素时钟，接前级 `vid_clk`（cam_pclk） |
| 输入 | `rst_n` | 1 | 低有效；与同链模块保持一致——本平台现有链接 `clk_wiz_0.locked` 作复位释放（教材 34/35 章做法为接 Processor System Reset） |
| 输入 | `pre_image_vsync` | 1 | 帧同步 |
| 输入 | `pre_image_clken` | 1 | 像素时钟使能（行有效期内逐像素） |
| 输入 | `pre_data_valid`（可选） | 1 | 数据有效 |
| 输入 | `pre_image_data` | 24 | RGB888（内部转灰度后可按 8 bit 灰度处理） |
| 输出 | `pos_image_vsync / pos_image_clken / pos_data_valid` | 1/1/1 | 相对输入延迟 N 拍的同名同步信号 |
| 输出 | `pos_image_data` | 24 | 处理结果，RGB888 或 `{Y,Y,Y}`/黑白 `{24{monoc}}` 形式 |

（接口命名沿用教材 VIP 封装的 `pre_image_* / pos_image_*`；与仓库现有模块混接时也可沿用 `rgb_vsync/rgb_clken/…` 风格，只要求一一对应。）

**同步对齐规则（教材反复强调，务必遵守）**：

- 处理数据**每延迟 1 拍**，`vsync/clken/valid` 必须同步寄存打 1 拍，否则画面错位/滚动。示例：灰度 3 级流水 → 打 3 拍（本仓库 `rgb2ycbcr.v` 即如此）；教材二值化比较耗 1 拍 → 打 1 拍；中值滤波（3×3 窗口 2 拍 + 三步排序 3 拍）→ 打 5 拍；Sobel（含 CORDIC 开方）共 16 拍 → 打 16 拍。
- 行数据连续（消隐期 `clken` 拉低），模块不应依赖行间额外空闲周期。

**（b）窗口/行缓存结构（滤波、HSV 邻域、连通域边界追踪常用）**

教材 3×3 模板标准做法（第 34/35 章）：

- 行缓存：`Block Memory Generator` → Simple Dual Port RAM，`8×1024`（8 bit 灰度，1024 深可覆盖 1024 像素行宽；分辨率更大时加深），例化 2 个分别缓存前两行，B 口勾选输出寄存器；
- 窗口：两行缓存读出 + 当前行输入经 3 级移位得到 `p11…p33` 三行三列矩阵；
- 生成窗口约 2 拍，此后每级运算 1 拍，同步信号按总延迟打拍（同（a）规则）。

**（c）配置/参数口**

- 初版：Verilog `parameter`（教材的阈值即 `parameter THRESHOLD=…`，Sobel 同理）——编译期固定，PS 不可调；
- 需 PS 在线调参（分割阈值、HSV 范围、识别使能等）时：把参数做成 **AXI4-Lite 从口寄存器**（自封装 IP 勾选 AXI-Lite，或另做寄存器小 IP），挂 GP0 总线；PS 用 `Xil_In32/Xil_Out32` 直接读写即可，不必生成 BSP 驱动；
- 多路参数打包进同一寄存器组；建议寄存器写入在“下一帧 vsync”处同步装载生效，避免帧中突变。

**（d）中断口（可选）**

结果就绪/帧完成：输出 1 bit 脉冲 → BD 中接 `processing_system7_0` 的 `IRQ_F2P[7:0]` → 使能 GIC。本阶段轮询够用也可不加。

### 7.3 新数据流格式要求

**（a）视频处理链上的数据格式（进出 VIP 必须满足）**

| 项 | 要求 |
|---|---|
| 像素格式 | RGB888；模块内部可按 8 bit 灰度处理，但对外口保持 24 bit（灰度写作 `{Y,Y,Y}`） |
| 速率/时序 | 1 像素/像素时钟，`clken` 有效处数据有效；帧/行/像素同步信号与数据逐拍对齐 |
| 时钟/复位 | 与采集链同源 `cam_pclk`；`rst_n` 低有效、与同链模块一致（本平台取 `clk_wiz_0.locked`，教材 34/35 章接视频时钟与 Processor System Reset） |
| 与 DDR/VDMA 的关系 | 处理后的整帧自动进入 `v_vid_in_axi4s → VDMA`；DDR 帧布局 24 bit/像素、行连续，3 帧循环；PS 读帧按 `宽×高×3B/帧` 寻址 |
| 分辨率 | 随 LCD ID 在 480×272 / 800×480 / 1024×600 / 1280×800 间切换；**新模块的行宽计数/行缓存深度需参数化**（或按最大 1280 设计） |

**（b）YUV/彩色相关约定（工程名 Cam_to_YUV 的由来）**

- 现状 `rgb2ycbcr` 只输出 Y 灰度通道（Cb/Cr 公式已写在模块注释中）。若识别需要“颜色”属性，推荐改走 **RGB→HSV**（H 通道对亮度/阴影更鲁棒，适合分割与取色），模块可复用灰度模块的流水与打拍框架；
- 若确实要把 YCbCr 全通道作为数据流送 PS/DDR：建议扩展输出 Y/Cb/Cr 三路 8 bit，或打包成 YCbCr422 16 bit，并相应调整 BD 中 native 视频/VDMA 位宽与 PS 端帧解析——**改动前先冻结“视频链 24 bit RGB888”约定**，或另开一路直通分支，避免破坏显示回环；
- 需要同时保留“彩色原图给 PS、灰度/二值链给显示/识别”时：在采集输出处分叉（一路直通、一路过处理链），两路各配帧缓存区域或 VDMA，地址由 PS 统一管理。

**（c）识别结果流格式（新数据流，建议按此规范实现，最终以 PS 侧需求为准）**

PL 识别模块以帧为节拍（`vsync` 结束后的场消隐内完成本帧统计）产生结果，写入结果寄存器组。建议格式：

| 字段 | 含义 | 建议位宽/编码 |
|---|---|---|
| 帧序号 | 结果对应的帧计数 | u32（低 16 位有效） |
| 区域数 N | 本帧有效连通域个数 | u8（≤ MAX_REGIONS，超限截断并置溢出位） |
| 状态/握手 | 就绪/已读/溢出 | bit 域：ready 置 1，PS 读后写 0 清 |
| 每区域记录 ×N： | | |
| └ 颜色 | 区域主色 | RGB888 或 HSV（H u16 + S u8 + V u8） |
| └ 面积 | 区域像素总数 | u32（单位：像素） |
| └ 周长/边长 | 边界长度 | 周长（轮廓像素计数）u32；若用外接矩形边长则提供宽、高两个 u16 并注明 |
| └ 形状类别 | 识别结论 | u8 枚举：0=未知，1=圆/椭圆，2=矩形，3=三角形，4=其它 |
| └ 位置/外接矩形（可选） | 便于 PS 叠加显示 | u16×4：x, y, w, h |

PS 侧按帧节拍轮询 `ready`，命中即解析（联调先 xil_printf 打印：帧号、区域数，以及每区 周长/颜色/面积/形状）。

### 7.4 接入操作步骤（Checklist，教材 34/35 章流程适用于本工程）

1. **开发 RTL**：按 7.2(a) 端口形式编写算法模块（如 `filter.v` / `rgb2hsv.v` / `connected_domain.v`），在 `sim/` 下建 testbench 仿真验证，重点核对 vsync/clken/数据延迟对齐（教材中值滤波有“3 拍延迟输出”仿真实例可参照）；
2. **打包 IP**：Tools → Create and Package New IP；或按 `rtl/ip_repo/rgb2ycbcr` 目录格式放置源文件后 Add/Refresh 仓库；多模块组合可学教材封装成顶层 VIP（`Video_Image_Processor` 风格）；
3. **更新仓库**：Vivado → Settings → IP → Repository，加入/刷新 `rtl/ip_repo`（教材提示：先删旧路径再加新路径，避免缓存）；
4. **BD 串入**：打开 `cam_sys.bd`，删除 `ov5640_capture_data_0`（或 `rgb2ycbcr_0`）与 `v_vid_in_axi4s_0` 之间连线 → 添加新 VIP → 连接 `clk`（取前级 `vid_clk`）、`rst_n`（取 `clk_wiz_0.locked`）、数据/同步总线 → `Validate Design`；
5. **综合实现**：Generate Output Products → Create HDL Wrapper → Generate Bitstream；
6. **导出硬件**：Export Hardware（含 bitstream），覆盖 `cam_sys_wrapper.xsa`；
7. **Vitis 同步**：平台工程 `cam_sys_platform` → Update Hardware Specification 指向新 XSA 并 Build（若 PS 侧无新增读取/打印任务，应用 `cam_screen` 代码无需改动）；
8. **板级验证**：按第 4 章接线运行，LCD/串口观察处理效果；再联调 7.3(c) 的结果上报通路。

### 7.5 本 PDF 与平台对应关系速查

| 教材内容（章） | 与本平台的关系 | PDF 内位置（约） |
|---|---|---|
| 第 30 章 OV7725 摄像头 LCD 显示 | 基线工程的搭建/接线/串口现象范本；30.5 节结论：3 帧缓存解决画面撕裂 | PDF 第 1~16 页（书内 625 页起） |
| 第 31 章 OV7725 HDMI 显示 | 同架构的 HDMI 变体，时钟方案可参考 | PDF 第 17~25 页 |
| 第 32 章 OV5640 灰度显示（rgb2ycbcr 串入） | **本平台当前状态即 OV5640 版灰度显示**：采集后串入 RGB2YCbCr（灰度）再入 DDR/回显 | PDF 第 26~42 页 |
| 第 33 章 OV5640 二值化（VIP 封装教程） | 新模块封装、串入、同步打拍的范本 | PDF 第 43~54 页 |
| 第 34 章 OV5640 中值滤波（3×3、行缓存、三步排序） | 滤波类新模块模板（规划链第 1 步） | PDF 第 55~82 页 |
| 第 35 章 OV5640 边缘检测（Sobel、CORDIC 开方、16 拍延迟） | 卷积类/大延迟模块模板 | PDF 第 83~98 页 |

---

## 8. 常见问题与调试提示

| 现象 | 排查 |
|---|---|
| 串口无打印 | 确认串口线接 UART、波特率 115200；程序是否已 Run 成功（看 Vitis Console） |
| `OV5640 detected failed!` | 摄像头是否插牢、镜头朝外；先上电再接摄像头，或复位重跑；SCCB 为 EMIO 54/55 软件模拟，勿短接 |
| `LCD ID: 0` 或显示错乱 | FPC 排线接触不良/插反；换屏后重新上电（屏 ID 仅在启动时读一次） |
| 画面撕裂/闪烁 | 平台已用 3 帧循环缓存（教材 30.5 结论：≥3 帧无撕裂），修改 VDMA 帧数时请勿低于 3 |
| 串入新模块后画面错位/滚动/花屏 | 多为 vsync/clken/valid 未与数据同拍延迟（见 7.2(a) 对齐规则）；用仿真或 ILA 抓取核对 |
| 分辨率改动后无图像 | 像素时钟由 `clk_wiz_cfg` 按 `VideoMode.freq` 动态配置，确认屏 ID 对应表项；改 `lcd_modes.h` 需同时核对 VTC 参数 |
| 灰度正常但处理链输出全黑/全白 | 检查阈值类参数（parameter）与位宽（24 bit 外口 / 8 bit 内部）是否匹配 |

---

## 附录 A 从仓库克隆后的重建步骤（仓库仅含源码）

仓库按“仅源码”策略管理（见根目录 `.gitignore`）：`.bit/.xsa`、Vitis 平台/BSP、应用编译产物均不入库。全新克隆或换机后，按下列顺序重建一次（需完整综合，耗时取决于机器）：

1. **Vivado 2020.2** 打开 `prj/cam_sys/cam_sys.xpr`；若克隆目录与原工程路径不同，先确认 `Settings → IP → Repository` 中自定义 IP 仓库 `rtl/ip_repo` 路径正确（必要时删除旧路径、重新添加）。
2. `Generate Bitstream`（工程含 Block Design，首次会先自动生成各 IP 输出并创建 HDL Wrapper）；完成后 `File → Export Hardware`，**勾选 Include bitstream**，导出到 `prj/cam_sys/cam_sys_wrapper.xsa`。
3. **Vitis 2020.2**：工作空间指向 `prj`（新建目录亦可）→ `File → New Platform Project`：名称 `cam_sys_platform`，Hardware Specification 选择上一步的 XSA，域 `standalone_ps7_cortexa9_0` → 创建后 Build 平台。
4. `File → New Application Project`：名称 `cam_screen`，平台选 `cam_sys_platform`，模板选空应用（Empty Application，C 语言）→ Build 出空工程后，把仓库 `prj/cam_screen/src/` 下的 `main.c`、`lscript.ld` 及 `display_ctrl/`、`vdma_api/`、`clk_wiz/`、`emio_sccb_cfg/`、`ov5640/` 等全部导入工程 `src`（右键工程 → Import → File System，或直接拷贝进 src 后刷新）。
5. Build `cam_screen` → 连接 JTAG/UART/摄像头/LCD 并上电 → Run，串口（115200）应打印 `LCD ID: xxx` 与 `OV5640 detected successful!`，LCD 显示摄像头灰度实时画面。
6. （可选）需要 SD 卡启动时，再建系统工程 `cam_screen_system` 把应用挂到平台并生成启动镜像；仅 JTAG 调试可跳过。

---

## 修订记录

| 日期 | 说明 |
|---|---|
| 2026-09 | 首版：整理为测试平台使用说明；明确图形识别方案与平台数据流；通读《正点原子领航者ZYNQ之嵌入式Vitis开发指南V1.2》（第 30~35 章节选）并给出新模块接入位置、端口形式与数据流格式要求 |
| 2026-09 | 补充附录 A：仓库改为“仅源码”入库（.gitignore 排除 Vivado/Vitis 全部生成物）后，克隆重建步骤与相关章节说明 |
