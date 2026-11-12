# 安卓端（可选）

这是 USB 直连之外的**备用链路**：手机上跑这个 App，把遥控器杆位通过 WiFi 用 UDP
发给电脑，PC 端 `VJoy.exe --udp` 收下来写进 vJoy。

直连能用的场合**不需要**这个组件。它的价值在于：

- 不想插线，或者 USB 口被占用
- 手机端还能提供**触控虚拟摇杆**（没有实体遥控器时也能操作）
- 提供逐轴的 中位 / 反向 / 灵敏度 图形化调参，比改 ini 直观

---

## 构建

前置：

1. **JDK 17**
2. **Android SDK**（含 platform-tools、对应 build-tools）
3. **DJI Mobile SDK**：本仓库不含其二进制。按 DJI 官方文档配置仓库与依赖
   （本项目使用的是 `dji-sdk-provided`），并遵守 DJI 的开发者条款

设置环境变量后运行：

```powershell
$env:JAVA_HOME    = 'C:\path\to\jdk17'
$env:ANDROID_HOME = 'C:\path\to\android-sdk'
.\build_apk.ps1
```

产物：`app\build\outputs\apk\debug\app-debug.apk`

---

## 与 PC 端的接口（UDP v2）

默认端口 **8456**，每包 **16 字节**，**大端**：

| 字节 | 内容 |
|---|---|
| 0..11 | 6 个 int16 大端：左杆横、左杆纵、右杆横、右杆纵、波轮、备用 |
| 12..13 | uint16 大端：按键位掩码（低 16 位） |
| 14..15 | uint16 大端：序号，用于丢弃过期包 |

PC 端接收端做了三件事保证跟手：

1. `SO_RCVBUF` 设到 4MB，避免突发时丢包
2. 非阻塞 + `select`，**一次只处理最新的一包**，不追历史
3. 轴值没变化时跳过 `UpdateVJD`

手机端固定 **60Hz** 节拍发送（`STICK_SEND_INTERVAL_MS = 16`），不在每帧新建线程。

---

## 手感参数

App 内「设置」页可以逐轴调：

- **中位**：物理中位时输出落在行程的百分之几（油门悬停点靠这个）
- **反向**
- **灵敏度**：滑动速率，不是延迟。`tFull = 0.18 + 1.67 × (1 - sens/100)` 秒

这三个参数与 PC 端 `vjoy_usb.ini` 的语义完全一致。

---

## 注意

- 手机 App 侧**不直接调用 vJoy**（那是 Windows 独有的 DLL），只发 UDP，由 PC 端写入
- 手机与电脑需要在**同一局域网**
- 使用 DJI Mobile SDK 需要遵守 DJI 条款；本项目与 DJI 官方无关
