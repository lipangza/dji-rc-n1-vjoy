# 常见故障与排查

本文档里每一条都是在本机真实踩到并修掉的问题，附了判断方法和结论。

---

## 串口相关

### `CreateFile(COMx) 失败, GetLastError=5`（拒绝访问）

**不是权限问题，是串口被别的进程独占打开了。**

排查：

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'dji|vjoy' } |
    Select-Object ProcessId, Name, CommandLine
```

本机曾出现过**两个 `DJI2vJoy.exe` 实例同时占着 COM9**，结束它们后立刻恢复：

```powershell
Stop-Process -Id <PID> -Force
```

注意：`Stop-Process` 没有 `-ErrorPassThru` 参数（写错会报参数不存在）。

### 枚举串口时有端口漏掉

注册表 `HARDWARE\DEVICEMAP\SERIALCOMM` 有时会少列设备（本机 COM9 在注册表里，
但只枚举出 COM4 / COM5）。

改用 `QueryDosDeviceA(NULL)` 遍历 `\Device\*` 再和注册表互相补充，不要靠逐个
`CreateFile` 试探。

### 探测过一次的串口，立刻重开会失败

这个驱动（`DJI USB Virtual COM`）在 `CloseHandle` 之后不是马上释放，
紧接着重开必然拿到 `ACCESS_DENIED`。

**做法：探测成功后保留句柄，不关闭、不重开**，只做 `PurgeComm(PURGE_RXCLEAR)`
清掉残留数据。代码里就是这么实现的。

### 只能收到 19 字节心跳帧

说明**没有发 PING**。遥控器必须收到握手包才开轴数据流。
见 `docs/PROTOCOL.md` 第 1 节。

---

## vJoy 相关

### 设备管理器里 vJoy 显示「代码 39：损坏的映像」

装成了精简驱动包或旧驱动。改用**官方 vJoy 2.1.9.1**，并清理残留设备实例。

### `AcquireVJD(1)` 失败

vJoy 设备 1 被别的程序占着（比如手机方案的 `VJoy.exe` 还开着，或者同时跑了两个桥接程序）。
先关掉再启动。

### 轴写进去了但游戏/系统读不到

本机 `vJoy Device 1` 只有 5 个有效槽：`X(0x30)`、`Y(0x31)`、`Z(0x32)`、
`Rz(0x35)`、`Sl0(0x36)`。

`Rx(0x33)` / `Ry(0x34)` / `Sl1(0x37)` 写入返回 `True`，但 Windows 里完全读不到变化。
详见 `docs/PROTOCOL.md` 第 5 节。

### 调 API 时崩溃（0xC0000005 空指针）

`UpdateVJD` 必须传完整状态结构：

```cpp
UpdateVJD(rID, &state);        // 正确
UpdateVJD(rID, NULL);          // 崩溃
UpdateVJD(rID);                // 参数不足，崩溃
```

轴范围是 `0..32767`，中点是 `16383`——不是 65535。

---

## 游戏相关

### 游戏启动弹「需要 DirectX Runtime」但系统 DirectX 齐全

本机实测真因是游戏目录里被 **x360ce 塞进去了 `xinput1_3.dll`**，
它在 UE4 初始化图形之前就拦住了。删掉那个 DLL 即可。

### 游戏完全识别不到遥控器

先确认游戏认的是不是标准 HID 手柄。有些模拟器（如 DJI 自家的 `DJI Virtual Flight`）
在二进制里硬编码了 `USB\VID_2CA3&PID_xxxx&MI_xx` 白名单，只走 libusb 直连，
**不读 HID**——这种情况改 vJoy 的 VID/PID 毫无作用（vJoy 是 `ROOT\HIDCLASS` 设备，
和 USB 白名单不是一个体系）。

**另一个常见原因是装错了软件**：官方 Windows 工程是给「大疆飞行模拟 DjiArena」用的，
而很多人装的是「DJI Virtual Flight」，后者只支持 FPV 遥控器和穿越摇杆。

---

## 编译相关

### 链接失败，找不到 `RegOpenKeyExA`

少了 `-ladvapi32`。完整的编译命令：

```powershell
g++ -std=c++17 -O2 -static -o VJoy.exe VJoyPC.cpp -lws2_32 -luser32 -ladvapi32
```
