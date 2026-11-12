# DJI RC-N1 → vJoy 桥接

把大疆 RC-N1 遥控器接到电脑上当虚拟手柄用，供飞行模拟器（DCL / Uncrashed / Liftoff /
蔚蓝无尽 等）读取。

两条链路：

| 链路 | 采样率 | 依赖 |
|---|---|---|
| **USB 串口直连**（推荐） | 63 Hz | 一根数据线 |
| 手机 UDP 中继（备用） | 20~30 Hz | 手机 + 同一局域网 |

主程序是 **C++ 单文件实现**，一个 exe 同时编入两条链路：插着遥控器走 USB，
找不到就自动回退到手机 UDP，不需要额外装 Python。

---

## 它解决什么问题

RC-N1 插上电脑后，设备管理器里只会出现一个 **`DJI USB Virtual COM`** 串口，
游戏和模拟器都识别不到它——因为它不是标准 HID 手柄。

本项目的做法：读这个串口、解出杆位，再通过 [vJoy](https://github.com/shauleiz/vJoy)
虚拟手柄驱动，把它变成游戏能读到的游戏控制器。

---

## 快速开始（Windows）

### 前置条件

1. **vJoy 2.1.9.1**（官方版，不是精简驱动包）已安装，`vJoy Device 1` 已创建
   - 检查：`Get-Service vjoy` 应为 Running
   - 检查：`C:\Program Files\vJoy\x64\vJoyConfig.exe -t` 能看到 Device 1
2. 编译需要 MinGW-w64 g++（或改用 VS，见下）

### 编译

```powershell
cd pc
.\build.ps1
```

等价于：

```powershell
g++ -std=c++17 -O2 -static -o VJoy.exe VJoyPC.cpp -lws2_32 -luser32 -ladvapi32
```

> `advapi32` 是必需的（用到 `RegOpenKeyExA`）。漏掉会链接失败。

### 运行

```powershell
.\VJoy.exe                  # 默认：USB 直连，找不到就自动回退手机 UDP
.\VJoy.exe --usb COM9       # 指定串口
.\VJoy.exe --scan           # 只看哪些串口、哪个是遥控器
.\VJoy.exe --keys           # 按键探测模式
.\VJoy.exe --udp            # 只用手机 UDP（端口 8456）
```

窗口保持开着，然后进游戏。运行日志写在 exe 同目录的 `vjoy.log`。

运行前请确认**没有别的程序占着串口或 vJoy 设备 1**，否则会看到
`CreateFile(COMx) 失败, GetLastError=5` 或 `AcquireVJD 失败`。

---

## 手感参数（`vjoy_usb.ini`）

首次运行会在 exe 同目录自动生成一份带注释的模板。

```ini
inv=0,0,0,0            # 反向：0=不反向 1=反向
ctr=50,50,50,50        # 中位：杆在物理中位时，输出落在行程的百分之几
sens=100,100,100,100   # 灵敏度：100=最快(0.18s 走完全行程)，10=最慢(1.68s)
deadzone=3             # 中位死区（原始 SBUS 单位）
baud=115200
```

四个轴顺序固定：**左杆横（偏航）、左杆纵（油门）、右杆横（横滚）、右杆纵（俯仰）**。

### 中位 `ctr`：穿越机的油门悬停点

`ctr` 表示「物理中位 → 输出落在这个百分比」。想让杆回中时正好是悬停油门，
就把第 2 个数改成你实测的悬停值：

```ini
ctr=50,45,50,50
```

两端仍然能推到满行程，只是中位的位置被挪了。

### 灵敏度 `sens`：滑动速率，不是延迟

它控制的是「杆自己滑到目标位置需要多久」：

`tFull = 0.18 + 1.67 × (1 - sens/100)` 秒

| sens | 走完全行程 |
|---|---|
| 100 | 0.18 s |
| 80 | 0.51 s |
| 50 | 1.02 s |
| 30 | 1.35 s |
| 10 | 1.68 s |

调小之后推杆不会瞬移，但**目标位置不变**——推到底还是到底，只是滑过去慢一点。
这是「手感调慢」，不是「加延迟」。

---

## 仓库结构

```
.
├── README.md
├── LICENSE
├── docs/
│   ├── PROTOCOL.md        串口帧格式、轴位、按键位（含实测状态标注）
│   ├── CREDITS.md         代码与协议的来源、致谢
│   └── TROUBLESHOOTING.md 常见故障与排查过程
├── pc/
│   ├── VJoyPC.cpp         主程序（USB + UDP 双链路）
│   ├── build.ps1          编译脚本
│   ├── vjoy_usb.ini.example
│   └── tools/
│       ├── axis_probe.py    轴位诊断：推杆，看哪个偏移真的在动
│       └── button_probe.py  按键诊断：按一个键，看哪个位变了
└── android/               安卓端（手机 UDP 中继 + 触控摇杆）
```

---

## 协议摘要

完整版见 [`docs/PROTOCOL.md`](docs/PROTOCOL.md)。

- 连上后**必须先发 13 字节 PING**（10ms 一次），遥控器才会开数据流；
  只被动读只能收到 19 字节心跳帧
- `0x55 0x26`，38 字节，轴帧，**63 Hz**
- `0x55 0x3A`，58 字节，按键帧，32 Hz，按键在**第 29 号字节**（`0x1d`），8 位掩码
- 轴量纲 SBUS：364 ~ 1684，中位 1024，半行程 660

---

## 已知限制

这些都写在 `docs/PROTOCOL.md` 里，这里只列结果：

1. **轴帧的纵向两路（偏移 16、19）尚未确认**：实测抓到这两组恒为 1684 不变化，
   可能是采样时没推纵向，也可能偏移本身记错了。在确认之前不要当成可靠结论。
2. **按键位图只确认了 4 个位**（`0x01 / 0x20 / 0x40 / 0x80`），其余 4 位未触发过。
3. 本机 vJoy 设备只有 5 个有效轴槽（X/Y/Z/Rz/Sl0），`Rx/Ry/Sl1` 写入无效。
4. 安卓端依赖 DJI Mobile SDK，构建前需自行配置，且遵循 DJI 的条款。
5. 需要半模拟器先能安装 vJoy 驱动；本项目不分发 vJoy 二进制。

---

## 来源

**请务必阅读 [`docs/CREDITS.md`](docs/CREDITS.md)。**

简要说明：

- 协议知识来自 **Timebutt/mDjiController**（[GitHub](https://github.com/Timebutt/mDjiController)）
  等前人项目的实测结论，经本项目在本机独立复测后采用
- 虚拟手柄能力来自 **vJoy** 项目，本项目只调用其公开 API，不分发其二进制
- 用户提供的参考脚本 `dji_vjoy.py` 非本仓库原创，未收录，其两处结论已被本项目修正并实测验证

---

## 免责

本项目与 DJI 官方无关。DJI、Mavic、RC-N1 是 DJI 的商标。
把遥控器接到电脑上跑模拟器不影响飞行器本身，但请自行评估合规性。
