// dji_vjoy PC 端 —— RC-N1 USB 直连（主）+ 手机 UDP（备），适配官方 vJoy 2.1.9.1
//
// 默认无参数运行 = USB 直连：自动找「DJI USB Virtual COM」串口 -> 发 PING 打开数据流
//                    -> 解析 0x26 轴帧(63Hz) / 0x3A 按键帧 -> 写 vJoy 设备 1
// 命令行：
//   VJoy.exe                   USB 直连（日常用这个）
//   VJoy.exe --usb COM9        指定串口
//   VJoy.exe --scan            只列出串口并探测哪个是遥控器
//   VJoy.exe --keys            按键探测：按一个键，看 0x3A 帧哪个字节变了
//   VJoy.exe --udp [8456]      走老的手机 UDP 链路
//
// ============================ USB 直连协议 ============================
// 打开数据流：先发 PING（每 10ms）+ PING2（每 25ms），遥控器才会推轴数据
//   0x55 0x26  38 字节  轴帧   ~63Hz
//   0x55 0x3A  58 字节  按键帧 ~32Hz
//   0x55 0x13  19 字节  心跳帧（不带杆位）
// 轴量纲 SBUS：364 ~ 1684，中位 1024，半行程 660
// 轴帧内偏移（int16 小端）：右杆横 13、右杆纵 16、左杆纵 19、左杆横 22、波轮 25
//
// ============================ 轴映射 ==================================
// 本机 vJoy 设备 1 实测只有 5 个有效轴槽：X(0x30) Y(0x31) Z(0x32) Rz(0x35) Sl0(0x36)
// （0x33 Rx / 0x34 Ry / 0x37 Sl1 写进去返回成功但 Windows 读不到，已实测排除）
//   X  <- 左杆横   Y  <- 左杆纵(油门)   Z  <- 右杆横   Rz <- 右杆纵   Sl0 <- 波轮
//
// ============================ 手感三件事 ==============================
// 中位(crt)：杆在中位时输出落在哪，50=正中，30=中位即 30% 行程（油门悬停点靠这个）
// 反向(inv)：0/1
// 灵敏度(sens)：杆"滑到目标"要用多久，100=最快；tFull = 0.18 + 1.67*(1-sens/100) 秒
//               这不是延迟，是杆自己移动的速度——sens 调小，杆滑得慢，但目标位置不变
// ======================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <algorithm>

// 输出同时写控制台和同目录 vjoy.log
struct TeeOut {
    std::ofstream f;
    template <class T>
    TeeOut& operator<<(const T& v) {
        std::cout << v;
        if (f.is_open()) { f << v; f.flush(); }
        return *this;
    }
};
static TeeOut g_out;

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

static const int VJOY_ID = 1;
static const double RAW_RANGE = 660.0;   // 手机端原始范围 [-660,660]
static const double USB_MID   = 1024.0;  // SBUS 中位
static const double USB_HALF  = 660.0;   // SBUS 半行程

// ---- 官方 vJoyInterface.dll 动态加载 ----
typedef BOOL (WINAPI *PFN_vJoyEnabled)(void);
typedef BOOL (WINAPI *PFN_AcquireVJD)(UINT);
typedef BOOL (WINAPI *PFN_RelinquishVJD)(UINT);
typedef BOOL (WINAPI *PFN_ResetVJD)(UINT);
typedef int  (WINAPI *PFN_GetVJDStatus)(UINT);
typedef BOOL (WINAPI *PFN_SetAxis)(LONG, UINT, UINT);
typedef BOOL (WINAPI *PFN_SetBtn)(BOOL, UINT, BYTE);
typedef BOOL (WINAPI *PFN_UpdateVJD)(UINT, void*);
typedef BOOL (WINAPI *PFN_GetAxisRange)(UINT, UINT, LONG*);

typedef struct {
    BYTE bDevice;
    BYTE pad0[3];
    LONG Throttle;
    LONG Rudder;
    LONG Aileron;
    LONG AxisX;
    LONG AxisY;
    LONG AxisZ;
    LONG AxisXRot;
    LONG AxisYRot;
    LONG AxisZRot;
    LONG Slider;
    LONG Dial;
    LONG Wheel;
    LONG AxisVX;
    LONG AxisVY;
    LONG AxisVZ;
    LONG AxisVBRX;
    LONG AxisVBRY;
    LONG AxisVBRZ;
    UINT Buttons;
    UINT bHats;
    UINT bHatsEx1;
    UINT bHatsEx2;
    UINT bHatsEx3;
    UINT ButtonsEx1;
    UINT ButtonsEx2;
    UINT ButtonsEx3;
    UINT pad1;
} JOYSTICK_POSITION_V2;

static const UINT AXIS_X  = 0x30;
static const UINT AXIS_Y  = 0x31;
static const UINT AXIS_Z  = 0x32;
static const UINT AXIS_RX = 0x33;
static const UINT AXIS_RY = 0x34;
static const UINT AXIS_RZ = 0x35;
static const UINT AXIS_SL0 = 0x36;

struct VJoyApi {
    HMODULE h = nullptr;
    PFN_vJoyEnabled   Enabled = nullptr;
    PFN_AcquireVJD    Acquire = nullptr;
    PFN_RelinquishVJD Relinq  = nullptr;
    PFN_ResetVJD      Reset   = nullptr;
    PFN_GetVJDStatus  Status  = nullptr;
    PFN_SetAxis       SetAxis = nullptr;
    PFN_UpdateVJD     Update  = nullptr;
    PFN_SetBtn        SetBtn  = nullptr;
    PFN_GetAxisRange  AxisMax = nullptr;

    bool Load() {
        if (h) return true;
        const char* paths[] = {
            "vJoyInterface.dll",
            "C:\\Program Files\\vJoy\\x64\\vJoyInterface.dll",
            "C:\\Program Files (x86)\\vJoy\\x64\\vJoyInterface.dll",
            "C:\\Program Files\\vJoy\\x86\\vJoyInterface.dll"
        };
        for (const char* p : paths) {
            h = LoadLibraryA(p);
            g_out << "[加载] " << p << " -> " << (h ? "成功" : "失败") << "\n";
            if (h) break;
        }
        if (!h) return false;
        auto get = [&](const char* name) -> void* {
            void* fp = (void*)GetProcAddress(h, name);
            if (!fp) g_out << "[缺失] " << name << "\n";
            return fp;
        };
        Enabled = (PFN_vJoyEnabled)get("vJoyEnabled");
        Acquire = (PFN_AcquireVJD)get("AcquireVJD");
        Relinq  = (PFN_RelinquishVJD)get("RelinquishVJD");
        Reset   = (PFN_ResetVJD)get("ResetVJD");
        Status  = (PFN_GetVJDStatus)get("GetVJDStatus");
        SetAxis = (PFN_SetAxis)get("SetAxis");
        Update  = (PFN_UpdateVJD)get("UpdateVJD");
        SetBtn  = (PFN_SetBtn)get("SetBtn");
        AxisMax = (PFN_GetAxisRange)get("GetVJDAxisMax");
        return Enabled && Acquire && Relinq && Reset && Status && SetAxis && Update;
    }
};
static VJoyApi g_vjoy;

static LONG g_axisMin = 0;
static LONG g_axisMax = 32767;

static long Scale(int16_t raw) {
    double t = (double)raw / RAW_RANGE;
    if (t >  1.0) t =  1.0;
    if (t < -1.0) t = -1.0;
    double mid  = ((double)g_axisMin + (double)g_axisMax) / 2.0;
    double half = ((double)g_axisMax - (double)g_axisMin) / 2.0;
    double v = mid + t * half;
    if (v < (double)g_axisMin) v = (double)g_axisMin;
    if (v > (double)g_axisMax) v = (double)g_axisMax;
    return (long)v;
}

static long NormToVjoy(double t) {
    if (t >  1.0) t =  1.0;
    if (t < -1.0) t = -1.0;
    double mid  = ((double)g_axisMin + (double)g_axisMax) / 2.0;
    double half = ((double)g_axisMax - (double)g_axisMin) / 2.0;
    double v = mid + t * half;
    if (v < (double)g_axisMin) v = (double)g_axisMin;
    if (v > (double)g_axisMax) v = (double)g_axisMax;
    return (long)v;
}

static void PressKey(BYTE key)   { keybd_event(key, 0, 0, 0); }
static void ReleaseKey(BYTE key) { keybd_event(key, 0, KEYEVENTF_KEYUP, 0); }

// ---- 按键位定义 ----
static const DWORD K_GOHOME   = 1u << 0;
static const DWORD K_RECORD   = 1u << 1;
static const DWORD K_SHUTTER  = 1u << 2;
static const DWORD K_PLAYBACK = 1u << 3;
static const DWORD K_PAUSE    = 1u << 4;
static const DWORD K_C1       = 1u << 5;
static const DWORD K_C2       = 1u << 6;
static const DWORD K_C3       = 1u << 7;
static const DWORD K_FN       = 1u << 8;
static const DWORD K_FIVED    = 1u << 9;
static const DWORD K_FIVED_UP = 1u << 10;
static const DWORD K_FIVED_DN = 1u << 11;
static const DWORD K_FIVED_LF = 1u << 12;
static const DWORD K_FIVED_RT = 1u << 13;
static const DWORD K_MENU     = 1u << 14;
static const DWORD K_PVTOGGLE = 1u << 15;

struct KeyBinding { DWORD mask; BYTE vk; const char* name; };
static const KeyBinding KEYMAP[] = {
    { K_GOHOME,   'O',     "返航" },
    { K_RECORD,   VK_F9,   "录像" },
    { K_SHUTTER,  VK_F10,  "快门/拍照" },
    { K_PLAYBACK, VK_F11,  "回放" },
    { K_PAUSE,    VK_F12,  "暂停" },
    { K_C1,       'V',     "C1" },
    { K_C2,       'B',     "C2" },
    { K_C3,       'N',     "C3" },
    { K_FN,       'M',     "Fn" },
    { K_FIVED,    'K',     "五维按下" },
    { K_FIVED_UP, 'I',     "五维上" },
    { K_FIVED_DN, 'J',     "五维下" },
    { K_FIVED_LF, 'H',     "五维左" },
    { K_FIVED_RT, 'L',     "五维右" },
    { K_MENU,     VK_F8,   "菜单" },
    { K_PVTOGGLE, VK_F7,   "拍照/录像切换" },
};
static const int KEYMAP_N = (int)(sizeof(KEYMAP) / sizeof(KEYMAP[0]));

static UINT MaskToButtons(DWORD m) {
    UINT b = 0;
    if (m & K_GOHOME)   b |= 1u << 0;
    if (m & K_RECORD)   b |= 1u << 1;
    if (m & K_SHUTTER)  b |= 1u << 2;
    if (m & K_PLAYBACK) b |= 1u << 3;
    if (m & K_PAUSE)    b |= 1u << 4;
    if (m & K_C1)       b |= 1u << 5;
    if (m & K_C2)       b |= 1u << 6;
    if (m & K_FIVED)    b |= 1u << 7;
    return b;
}

static volatile bool g_running = true;

static BOOL WINAPI CtrlHandler(DWORD) {
    g_running = false;
    if (g_vjoy.Reset) g_vjoy.Reset(VJOY_ID);
    if (g_vjoy.Relinq) g_vjoy.Relinq(VJOY_ID);
    ExitProcess(0);
    return TRUE;
}

static int16_t ReadBE16(const unsigned char* p) {
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
static int16_t ReadLE16(const unsigned char* p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// ================================ 串口 ================================
struct SerialPort {
    HANDLE h = nullptr;
    std::string name;

    bool Open(const std::string& port, int baud) {
        Close();
        std::string path = "\\\\.\\" + port;
        HANDLE hh = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        // CreateFile 失败返回 INVALID_HANDLE_VALUE(-1)，不是 nullptr，必须两个都判
        if (hh == INVALID_HANDLE_VALUE || hh == nullptr) {
            g_out << "[串口] CreateFile(" << port << ") 失败, GetLastError="
                  << GetLastError() << "\n";
            return false;
        }
        h = hh;
        SetupComm(h, 4096, 4096);
        DCB dcb{};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(h, &dcb)) {
            g_out << "[串口] GetCommState(" << port << ") 失败, err=" << GetLastError() << "\n";
            Close();
            return false;
        }
        dcb.BaudRate = (DWORD)baud;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary  = TRUE;
        dcb.fParity  = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        if (!SetCommState(h, &dcb)) {
            g_out << "[串口] SetCommState(" << port << ") 失败, err=" << GetLastError() << "\n";
            Close();
            return false;
        }
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout         = MAXDWORD;  // 有数据立即返回
        to.ReadTotalTimeoutMultiplier  = 0;
        to.ReadTotalTimeoutConstant    = 0;
        to.WriteTotalTimeoutMultiplier = 0;
        to.WriteTotalTimeoutConstant   = 100;
        SetCommTimeouts(h, &to);
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        name = port;
        return true;
    }

    void Close() { if (h) { CloseHandle(h); h = nullptr; } }

    bool Write(const unsigned char* d, int n) {
        if (!h) return false;
        DWORD written = 0;
        return WriteFile(h, d, (DWORD)n, &written, nullptr) && written == (DWORD)n;
    }

    int Read(unsigned char* buf, int maxLen) {
        if (!h) return 0;
        DWORD got = 0;
        if (!ReadFile(h, buf, (DWORD)maxLen, &got, nullptr)) return -1;
        return (int)got;
    }
};

static const unsigned char PING[13] =
    { 0x55, 0x0d, 0x04, 0x33, 0x0a, 0x0e, 0x03, 0x00, 0x40, 0x06, 0x01, 0xf4, 0x4a };
static const unsigned char PING2[13] =
    { 0x55, 0x0d, 0x04, 0x33, 0x0a, 0x0e, 0x02, 0x00, 0x40, 0x06, 0x27, 0x84, 0x05 };

static std::vector<std::string> ListComPorts() {
    std::vector<std::string> out;
    // QueryDosDevice(NULL) 能列出系统里所有 MS-DOS 设备名（含 COM*），
    // 不需要真的打开端口，比"逐个 CreateFile 试探"安全，也不会和后面的打开冲突。
    {
        std::vector<char> buf(256 * 1024, 0);
        DWORD n = QueryDosDeviceA(nullptr, buf.data(), (DWORD)buf.size());
        if (n > 0) {
            const char* p = buf.data();
            while (*p) {
                std::string s(p);
                if (s.rfind("COM", 0) == 0 && s.size() >= 4) out.push_back(s);
                p += s.size() + 1;
            }
        }
    }
    // 补注册表里的（个别驱动只出现在 SERIALCOMM）
    HKEY k = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            char vname[256] = {0};
            char vdata[256] = {0};
            DWORD nl = sizeof(vname) - 1, dl = sizeof(vdata) - 1;
            DWORD type = 0;
            LONG r = RegEnumValueA(k, i, vname, &nl, nullptr, &type,
                                   (BYTE*)vdata, &dl);
            if (r != ERROR_SUCCESS) break;
            if (type == REG_SZ && vdata[0]) {
                std::string s(vdata);
                bool dup = false;
                for (const std::string& e : out) if (e == s) { dup = true; break; }
                if (!dup && s.rfind("COM", 0) == 0) out.push_back(s);
            }
        }
        RegCloseKey(k);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return atoi(a.c_str() + 3) < atoi(b.c_str() + 3);
    });
    return out;
}

// 发 ping 后看 0.6 秒内有没有 0x55 0x26，有就是遥控器
static bool ProbeIsRemote(SerialPort& sp) {
    sp.Write(PING, 13);
    sp.Write(PING2, 13);
    unsigned char tmp[512];
    std::vector<unsigned char> buf;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.6) {
        int n = sp.Read(tmp, (int)sizeof(tmp));
        if (n > 0) buf.insert(buf.end(), tmp, tmp + n);
        for (size_t i = 0; i + 1 < buf.size(); ++i) {
            if (buf[i] == 0x55 && buf[i + 1] == 0x26) return true;
        }
        if (buf.size() > 2048) buf.erase(buf.begin(), buf.begin() + 1024);
        Sleep(2);
    }
    return false;
}

// ================================ 配置 ================================
struct AxisCfg {
    int    inv  = 0;     // 0/1 反向
    double ctr  = 50.0;  // 中位百分比
    double sens = 100.0; // 灵敏度百分比
};

struct AppCfg {
    AxisCfg ax[4];            // 0=左横 1=左纵(油门) 2=右横 3=右纵
    double  deadzone = 3.0;   // 原始 SBUS 单位
    int     usb_baud = 115200;
};
static AppCfg g_cfg;

static std::string ExeDir() {
    char p[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p);
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : s.substr(0, slash + 1);
}

static void WriteDefaultCfg(const std::string& path) {
    std::ofstream f(path.c_str(), std::ios::out | std::ios::trunc);
    if (!f.is_open()) return;
    f << "# dji_vjoy USB 直连配置（改完重启 VJoy.exe 生效）\n";
    f << "#\n";
    f << "# inv  反向：0=不反向 1=反向\n";
    f << "# ctr  中位：杆在物理中位时，输出落在行程的百分之几（50=正中）\n";
    f << "#      穿越机油门用这个调悬停点：比如中位就想 45% 油门，就把第 2 行 ctr 改 45\n";
    f << "# sens 灵敏度：杆滑动到目标要多久，100=最快(0.18 秒走完全行程)，10=最慢(1.68 秒)\n";
    f << "#      这个不是延迟，是杆自己移动的速度，目标位置不变\n";
    f << "#\n";
    f << "# 顺序：左杆横(偏航) 左杆纵(油门) 右杆横(横滚) 右杆纵(俯仰)\n";
    f << "inv=0,0,0,0\n";
    f << "ctr=50,50,50,50\n";
    f << "sens=100,100,100,100\n";
    f << "\n";
    f << "# 中位死区（原始 SBUS 单位，3 很小，抖动大就加到 6~10）\n";
    f << "deadzone=3\n";
}

static std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

static bool LoadCfg(const std::string& path) {
    std::ifstream f(path.c_str());
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t p = line.find_first_not_of(" \t\r");
        if (p == std::string::npos || line[p] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(p, eq - p);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        std::string val = line.substr(eq + 1);
        if (key == "inv" || key == "ctr" || key == "sens") {
            std::vector<std::string> v = SplitComma(val);
            for (int i = 0; i < 4 && i < (int)v.size(); ++i) {
                if (v[i].empty()) continue;
                if (key == "inv")  g_cfg.ax[i].inv  = atoi(v[i].c_str()) ? 1 : 0;
                if (key == "ctr")  g_cfg.ax[i].ctr  = atof(v[i].c_str());
                if (key == "sens") g_cfg.ax[i].sens = atof(v[i].c_str());
            }
        } else if (key == "deadzone") {
            g_cfg.deadzone = atof(val.c_str());
        } else if (key == "baud") {
            g_cfg.usb_baud = atoi(val.c_str());
        }
    }
    // 钳位
    for (int i = 0; i < 4; ++i) {
        if (g_cfg.ax[i].ctr < 0)   g_cfg.ax[i].ctr = 0;
        if (g_cfg.ax[i].ctr > 100) g_cfg.ax[i].ctr = 100;
        if (g_cfg.ax[i].sens < 1)   g_cfg.ax[i].sens = 1;
        if (g_cfg.ax[i].sens > 100) g_cfg.ax[i].sens = 100;
    }
    if (g_cfg.deadzone < 0) g_cfg.deadzone = 0;
    return true;
}

// ============================ 手感处理 ================================
struct Smoother {
    double cur = 0.0;
    bool   init = false;
    // 目标 t，tFull = 走完整个行程(-1 -> +1)所需秒数
    double Step(double target, double tFull, double dt) {
        if (!init) { cur = target; init = true; return cur; }
        if (tFull < 0.01) tFull = 0.01;
        double maxStep = (2.0 / tFull) * dt;
        double d = target - cur;
        if (fabs(d) <= maxStep) cur = target;
        else cur += (d > 0 ? 1.0 : -1.0) * maxStep;
        return cur;
    }
};
static Smoother g_smooth[5];

// 原始值 -> [-1,1] 归一（含中位偏移与死区）
static double UsbToNorm(int16_t raw, const AxisCfg& c, double dzRaw) {
    double u = ((double)raw - USB_MID) / USB_HALF;
    if (u >  1.0) u =  1.0;
    if (u < -1.0) u = -1.0;
    // 死区作用在杆的物理位移上
    if (fabs((double)raw - USB_MID) <= dzRaw) u = 0.0;
    double ctr = (c.ctr - 50.0) / 50.0;          // -1..+1
    double t;
    if (u >= 0) t = ctr + u * (1.0 - ctr);       // 上段：中位 -> +1
    else        t = ctr + u * (1.0 + ctr);       // 下段：中位 -> -1
    if (c.inv) t = -t;
    if (t >  1.0) t =  1.0;
    if (t < -1.0) t = -1.0;
    return t;
}

static double TFffull(double sensPct) {
    if (sensPct > 100) sensPct = 100;
    if (sensPct < 1)   sensPct = 1;
    return 0.18 + 1.67 * (1.0 - sensPct / 100.0);
}

// ================================ vJoy ================================
static bool InitVJoy() {
    if (!g_vjoy.Load()) {
        g_out << "[错误] 找不到可用的 vJoyInterface.dll。\n";
        g_out << "       请先安装官方 vJoy 2.1.9.1，或把 vJoyInterface.dll 放到本程序同目录。\n";
        return false;
    }
    if (!g_vjoy.Enabled()) {
        g_out << "[错误] vJoy 驱动未启用（用 vJoyConfig enable on 开启）。\n";
        return false;
    }
    int st = g_vjoy.Status(VJOY_ID);
    g_out << "vJoy 驱动已启用，设备 " << VJOY_ID << " 状态: ";
    switch (st) {
        case 0: g_out << "OWNED（已被别的程序占用）\n"; break;
        case 1: g_out << "FREE（空闲，可以接管）\n"; break;
        case 2: g_out << "BUSY（正在被使用）\n"; break;
        case 3: g_out << "MISSING（设备不存在，需用 vJoyConfig 创建）\n"; break;
        default: g_out << st << "（未知状态）\n"; break;
    }
    if (st == 0 || st == 2) {
        g_out << "[警告] 设备被其它程序占用，请先关掉占用它的程序再运行。\n";
    }
    if (st == 3) {
        g_out << "[错误] 设备不存在。请用管理员运行: vJoyConfig 1 -f\n";
        return false;
    }
    if (!g_vjoy.Acquire(VJOY_ID)) {
        g_out << "[错误] AcquireVJD(1) 失败，程序无法写入 vJoy 设备 1。\n";
        g_out << "       多半是别的 VJoy.exe / 测试脚本还开着，关掉它再来。\n";
        return false;
    }
    g_out << "[诊断] Acquire 后状态 = " << g_vjoy.Status(VJOY_ID) << "（0=OWNED 表示已接管）\n";
    g_vjoy.Reset(VJOY_ID);
    if (g_vjoy.AxisMax) {
        LONG vmax = 0;
        if (g_vjoy.AxisMax(VJOY_ID, AXIS_X, &vmax) && vmax > 1000) g_axisMax = vmax;
        g_out << "[轴范围] 0 .. " << g_axisMax << "（中位 " << (g_axisMax / 2) << "）\n";
    }
    return true;
}

static void PrintCfgSummary() {
    const char* names[4] = { "左杆横(偏航)", "左杆纵(油门)", "右杆横(横滚)", "右杆纵(俯仰)" };
    g_out << "\n[手感配置]\n";
    for (int i = 0; i < 4; ++i) {
        g_out << "  " << names[i]
              << "  反向=" << g_cfg.ax[i].inv
              << "  中位=" << g_cfg.ax[i].ctr << "%"
              << "  灵敏度=" << g_cfg.ax[i].sens << "%"
              << "  (走完全行程 " << TFffull(g_cfg.ax[i].sens) << " 秒)\n";
    }
    g_out << "  死区=" << g_cfg.deadzone << " (原始单位)\n";
    g_out << "  配置文件: vjoy_usb.ini（改完重启本程序生效）\n\n";
}

static VOID CALLBACK NoopTimer(PVOID, BOOLEAN) {}

// ================================ USB 主循环 ==========================
static int RunUsb(const std::string& portSpec, bool keysMode) {
    if (!InitVJoy()) return 1;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    SerialPort sp;
    bool opened = false;
    std::vector<std::string> ports = ListComPorts();
    if (ports.empty()) {
        g_out << "[错误] 一个串口都没找到。遥控器插好了吗？\n";
        g_vjoy.Relinq(VJOY_ID);
        return 2;
    }
    g_out << "[串口] 系统里现有: ";
    for (size_t i = 0; i < ports.size(); ++i) g_out << (i ? ", " : "") << ports[i];
    g_out << "\n";

    if (!portSpec.empty()) {
        if (sp.Open(portSpec, g_cfg.usb_baud)) {
            opened = true;
            g_out << "[串口] 使用指定端口 " << portSpec << "\n";
        } else {
            g_out << "[警告] 打开 " << portSpec << " 失败，改为自动探测\n";
        }
    }
    if (!opened) {
        g_out << "[串口] 正在探测哪个是遥控器（每个口 0.6 秒）…\n";
        for (const std::string& p : ports) {
            if (!sp.Open(p, g_cfg.usb_baud)) {
                g_out << "        " << p << " 打不开（被占用或不是标准串口）\n";
                continue;
            }
            if (ProbeIsRemote(sp)) {
                opened = true;   // 保留这个已打开的句柄直接用，避免"关闭后立刻重开"被拒
                PurgeComm(sp.h, PURGE_RXCLEAR);
                g_out << "[串口] 找到遥控器: " << p << "\n";
                break;
            }
            sp.Close();
            g_out << "        " << p << " 不是\n";
        }
    }
    if (!opened) {
        g_out << "[错误] 没找到会回 0x26 轴帧的串口。\n";
        g_out << "       检查：遥控器开机了没 / 是不是数据线（不是充电线）/ DJI Assistant 是不是占着串口\n";
        g_vjoy.Relinq(VJOY_ID);
        return 3;
    }

    unsigned char tmp[2048];
    std::vector<unsigned char> buf;
    long long frames26 = 0, frames3a = 0;
    bool got26 = false;

    auto tPing  = std::chrono::steady_clock::now();
    auto tPing2 = std::chrono::steady_clock::now();
    auto tPrev  = std::chrono::steady_clock::now();
    auto tLog   = tPrev;
    auto tNoData = tPrev;
    long long loops = 0;

    // 当前轴原始值
    int16_t rawRh = 1024, rawRv = 1024, rawLv = 1024, rawLh = 1024, rawCam = 1024;
    unsigned char btnFrame[58] = {0};
    bool hasBtn = false;
    DWORD prevMask = 0xFFFFFFFF;
    LONGLONG prevAxis[5] = { -1, -1, -1, -1, -1 };
    UINT prevButtons = 0xFFFFFFFF;

    // 按键探测模式：记录上一次 0x3A 帧
    unsigned char prevBtnFrame[58] = {0};
    bool havePrevBtn = false;

    g_out << (keysMode ? "\n[按键探测模式] 逐个按遥控器上的键，看下面哪个字节变了。\n"
                       : "\n已接管 vJoy 设备 1，USB 直连运行中，Ctrl+C 退出。\n");

    if (!keysMode) {
        g_out << "先进游戏把杆推一遍看跟不跟手；不跟手就调 vjoy_usb.ini 的 sens。\n\n";
    }

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - tPrev).count();
        tPrev = now;
        if (dt > 0.2) dt = 0.2;      // 和手机 App 一样钳位
        if (dt <= 0)  dt = 0.001;

        // 定时 ping：这是打开数据流的关键，不发就只有心跳
        if (std::chrono::duration<double>(now - tPing).count() >= 0.010) {
            sp.Write(PING, 13);
            tPing = now;
        }
        if (std::chrono::duration<double>(now - tPing2).count() >= 0.025) {
            sp.Write(PING2, 13);
            tPing2 = now;
        }

        int n = sp.Read(tmp, (int)sizeof(tmp));
        if (n < 0) {
            g_out << "[警告] 串口读取出错，可能被拔掉了，退出。\n";
            break;
        }
        if (n == 0) {
            if (std::chrono::duration<double>(now - tNoData).count() > 3.0) {
                g_out << "[警告] 3 秒没收到任何数据。遥控器还连着吗？\n";
                tNoData = now;
            }
            Sleep(1);
            continue;
        }
        tNoData = now;
        buf.insert(buf.end(), tmp, tmp + n);
        if (buf.size() > 4096) buf.erase(buf.begin(), buf.begin() + (buf.size() - 2048));

        // 解析帧
        size_t i = 0;
        while (i + 1 < buf.size()) {
            if (buf[i] != 0x55) { ++i; continue; }
            int ln = -1;
            if (buf[i + 1] == 0x13) ln = 19;
            else if (buf[i + 1] == 0x26) ln = 38;
            else if (buf[i + 1] == 0x3a) ln = 58;
            if (ln < 0) { ++i; continue; }
            if (i + (size_t)ln > buf.size()) break;   // 帧没到齐
            const unsigned char* f = &buf[i];
            if (f[1] == 0x26) {
                rawRh  = ReadLE16(f + 13);
                rawRv  = ReadLE16(f + 16);
                rawLv  = ReadLE16(f + 19);
                rawLh  = ReadLE16(f + 22);
                rawCam = ReadLE16(f + 25);
                ++frames26;
                if (!got26) {
                    got26 = true;
                    g_out << "[连接] 收到首帧 0x26 轴帧（38 字节），数据流已打开\n";
                }
            } else if (f[1] == 0x3a) {
                memcpy(btnFrame, f, 58);
                hasBtn = true;
                ++frames3a;
                if (keysMode) {
                    if (havePrevBtn) {
                        for (int b = 0; b < 58; ++b) {
                            if (btnFrame[b] != prevBtnFrame[b]) {
                                g_out << "[按键探测] 字节[" << b << "] 0x"
                                      << std::hex << (int)prevBtnFrame[b] << " -> 0x"
                                      << (int)btnFrame[b] << std::dec << "\n";
                            }
                        }
                    }
                    memcpy(prevBtnFrame, btnFrame, 58);
                    havePrevBtn = true;
                }
            }
            i += (size_t)ln;
        }
        if (i > 0) buf.erase(buf.begin(), buf.begin() + i);

        if (keysMode) { Sleep(1); continue; }
        if (!got26) { Sleep(1); continue; }

        // ---- 轴：原始 -> 归一 -> 平滑 -> vJoy ----
        double t0 = UsbToNorm(rawLh,  g_cfg.ax[0], g_cfg.deadzone);
        double t1 = UsbToNorm(rawLv,  g_cfg.ax[1], g_cfg.deadzone);
        double t2 = UsbToNorm(rawRh,  g_cfg.ax[2], g_cfg.deadzone);
        double t3 = UsbToNorm(rawRv,  g_cfg.ax[3], g_cfg.deadzone);
        double t4 = ((double)rawCam - USB_MID) / USB_HALF;
        if (t4 >  1) t4 =  1;
        if (t4 < -1) t4 = -1;

        double s0 = g_smooth[0].Step(t0, TFffull(g_cfg.ax[0].sens), dt);
        double s1 = g_smooth[1].Step(t1, TFffull(g_cfg.ax[1].sens), dt);
        double s2 = g_smooth[2].Step(t2, TFffull(g_cfg.ax[2].sens), dt);
        double s3 = g_smooth[3].Step(t3, TFffull(g_cfg.ax[3].sens), dt);
        double s4 = g_smooth[4].Step(t4, 0.18, dt);

        LONG vx  = NormToVjoy(s0);
        LONG vy  = NormToVjoy(s1);
        LONG vz  = NormToVjoy(s2);
        LONG vrz = NormToVjoy(s3);
        LONG vsl = NormToVjoy(s4);

        // ---- 按键：0x3A 帧第 29 号字节（0x1d）是位掩码，实测会出现的位是
        //      0x01 / 0x20 / 0x40 / 0x80。这里不再写死哪一位是什么键，
        //      直接把 8 个位一一对到 vJoy 的按钮 1..8，哪个键按下去游戏里都能看到。
        DWORD mask = 0;
        if (hasBtn) {
            BYTE b = btnFrame[0x1d];
            for (int bit = 0; bit < 8; ++bit) {
                if (b & (1u << bit)) mask |= (1u << bit);
            }
        }

        JOYSTICK_POSITION_V2 st{};
        st.bDevice  = (BYTE)VJOY_ID;
        st.Throttle = vy;
        st.AxisX    = vx;
        st.AxisY    = vy;
        st.AxisZ    = vz;
        st.AxisZRot = vrz;
        LONG mid = (LONG)((g_axisMax + g_axisMin) / 2);
        st.AxisXRot = mid;
        st.AxisYRot = mid;
        st.Slider   = vsl;
        st.Dial     = mid;
        st.Wheel    = mid;
        st.AxisVX   = mid; st.AxisVY = mid; st.AxisVZ = mid;
        st.AxisVBRX = mid; st.AxisVBRY = mid; st.AxisVBRZ = mid;
        st.Buttons  = MaskToButtons(mask);
        st.bHats    = 0xFFFFFFFF;
        st.bHatsEx1 = 0xFFFFFFFF;
        st.bHatsEx2 = 0xFFFFFFFF;
        st.bHatsEx3 = 0xFFFFFFFF;

        UINT buttons = st.Buttons;
        bool changed = (vx != prevAxis[0] || vy != prevAxis[1] || vz != prevAxis[2] ||
                        vrz != prevAxis[3] || vsl != prevAxis[4] || buttons != prevButtons);
        if (changed) {
            g_vjoy.Update(VJOY_ID, &st);
            prevAxis[0] = vx; prevAxis[1] = vy; prevAxis[2] = vz;
            prevAxis[3] = vrz; prevAxis[4] = vsl;
            prevButtons = buttons;
        }

        // 键盘通道：只在状态变化时触发
        if (mask != prevMask) {
            if (prevMask != 0xFFFFFFFF) {
                for (int k = 0; k < KEYMAP_N; ++k) {
                    bool now2 = (mask & KEYMAP[k].mask) != 0;
                    bool was2 = (prevMask & KEYMAP[k].mask) != 0;
                    if (now2 && !was2) PressKey(KEYMAP[k].vk);
                    else if (!now2 && was2) ReleaseKey(KEYMAP[k].vk);
                }
            }
            prevMask = mask;
        }

        ++loops;
        if (std::chrono::duration<double>(now - tLog).count() >= 2.0) {
            tLog = now;
            g_out << "[数据] 轴帧 " << frames26 << "  按键帧 " << frames3a
                  << "  | 原始 LH=" << rawLh << " LV=" << rawLv
                  << " RH=" << rawRh << " RV=" << rawRv << " CAM=" << rawCam
                  << "  | 输出 X=" << vx << " Y=" << vy << " Z=" << vz
                  << " Rz=" << vrz << " Sl0=" << vsl;
            if (hasBtn) {
                g_out << "  | 键=0x" << std::hex << std::uppercase
                      << (int)(btnFrame[0x1d] & 0xFF)
                      << std::dec << std::nouppercase;
            }
            g_out << "\n";
        }

        Sleep(1);
    }

    sp.Close();
    g_vjoy.Relinq(VJOY_ID);
    g_out << "\n已释放 vJoy 设备 1，退出。\n";
    return 0;
}

// ================================ UDP 主循环 ==========================
static int RunUdp(unsigned short port) {
    if (!InitVJoy()) return 1;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        g_out << "[错误] WSAStartup 失败\n";
        g_vjoy.Relinq(VJOY_ID);
        return 1;
    }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        g_out << "[错误] socket 创建失败\n";
        WSACleanup();
        g_vjoy.Relinq(VJOY_ID);
        return 1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        g_out << "[错误] 端口 " << port << " 绑定失败（可能被别的程序占用）\n";
        closesocket(sock);
        WSACleanup();
        g_vjoy.Relinq(VJOY_ID);
        return 4;
    }
    g_out << "已接管 vJoy 设备 1，开始接收 UDP " << port << "。\n";
    g_out << "本机 IP（手机端填这个）：\n";
    char host[128] = {0};
    if (gethostname(host, sizeof(host)) == 0) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
            for (addrinfo* p = res; p; p = p->ai_next) {
                sockaddr_in* in = (sockaddr_in*)p->ai_addr;
                char b[64] = {0};
                inet_ntop(AF_INET, &in->sin_addr, b, sizeof(b));
                if (std::string(b).rfind("127.", 0) != 0) g_out << "  " << b << "\n";
            }
            freeaddrinfo(res);
        }
    }
    g_out << "\n等待手机连接，Ctrl+C 可退出。\n";
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    {
        sockaddr_in self{};
        self.sin_family = AF_INET;
        self.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &self.sin_addr);
        int sent = sendto(sock, "\x01", 1, 0, (sockaddr*)&self, sizeof(self));
        g_out << "[自测] sendto 自身返回 " << sent << "\n";
    }

    int rcvbuf = 4 * 1024 * 1024;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf)) != SOCKET_ERROR) {
        int got = 0, len0 = sizeof(got);
        getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&got, &len0);
        g_out << "[缓冲] 接收缓冲 = " << got << " 字节\n";
    }
    u_long nonblock = 1;
    ioctlsocket(sock, FIONBIO, &nonblock);

    unsigned char pkt[256];
    long long packetCount = 0, skippedTotal = 0;
    bool keyR = false, keyF = false;
    DWORD prevMask = 0;
    LONG prevVx = -1, prevVy = -1, prevVz = -1, prevVrz = -1;
    UINT prevButtons = 0xFFFFFFFF;

    while (g_running) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock, &rd);
        timeval tv{0, 2000};
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;

        unsigned char lastPkt[256];
        int lastLen = 0, drained = 0;
        for (;;) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            int len = recvfrom(sock, (char*)pkt, sizeof(pkt), 0, (sockaddr*)&from, &fromLen);
            if (len <= 0) break;
            if (len == 1) {
                char ip[64] = {0};
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                g_out << "[网络测试] 收到来自 " << ip << " 的测试包，连通正常\n";
                continue;
            }
            if (len < 12) continue;
            memcpy(lastPkt, pkt, (size_t)len);
            lastLen = len;
            ++drained;
        }
        if (lastLen == 0) continue;
        if (drained > 1) skippedTotal += drained - 1;
        memcpy(pkt, lastPkt, (size_t)lastLen);
        int len = lastLen;

        int16_t lx = ReadBE16(pkt + 0);
        int16_t ly = ReadBE16(pkt + 2);
        int16_t rx = ReadBE16(pkt + 4);
        int16_t ry = ReadBE16(pkt + 6);
        int16_t dial = ReadBE16(pkt + 8);

        int16_t rightDial = 0;
        DWORD mask = 0;
        if (len >= 16) {
            rightDial = ReadBE16(pkt + 10);
            (void)rightDial;
            BYTE b1 = pkt[12];
            BYTE b2 = pkt[13];
            if (b1 & 0x01) mask |= K_C1;
            if (b1 & 0x02) mask |= K_C2;
            if (b1 & 0x04) mask |= K_C3;
            if (b1 & 0x08) mask |= K_FN;
            if (b1 & 0x10) mask |= K_GOHOME;
            if (b1 & 0x20) mask |= K_FIVED;
            if (b1 & 0x40) mask |= K_PLAYBACK;
            if (b1 & 0x80) mask |= K_PAUSE;
            if (b2 & 0x01) mask |= K_RECORD;
            if (b2 & 0x02) mask |= K_SHUTTER;
            if (b2 & 0x04) mask |= K_PVTOGGLE;
            if (b2 & 0x08) mask |= K_MENU;
            if (b2 & 0x10) mask |= K_FIVED_UP;
            if (b2 & 0x20) mask |= K_FIVED_DN;
            if (b2 & 0x40) mask |= K_FIVED_LF;
            if (b2 & 0x80) mask |= K_FIVED_RT;
        } else {
            if (pkt[10] != 0) mask |= K_C1;
            if (pkt[11] != 0) mask |= K_GOHOME;
        }

        BOOL first = (packetCount == 0);
        LONG vx  = (LONG)Scale(lx);
        LONG vy  = (LONG)Scale((int16_t)(-ly));
        LONG vz  = (LONG)Scale(rx);
        LONG vrz = (LONG)Scale(ry);

        JOYSTICK_POSITION_V2 st{};
        st.bDevice  = (BYTE)VJOY_ID;
        st.Throttle = vy;
        st.AxisX    = vx;
        st.AxisY    = vy;
        st.AxisZ    = vz;
        st.AxisZRot = vrz;
        LONG mid = (LONG)((g_axisMax + g_axisMin) / 2);
        st.AxisXRot = mid;
        st.AxisYRot = mid;
        st.Slider   = mid;
        st.Dial     = mid;
        st.Wheel    = mid;
        st.AxisVX = mid; st.AxisVY = mid; st.AxisVZ = mid;
        st.AxisVBRX = mid; st.AxisVBRY = mid; st.AxisVBRZ = mid;
        st.Buttons  = MaskToButtons(mask);
        st.bHats    = 0xFFFFFFFF;
        st.bHatsEx1 = 0xFFFFFFFF;
        st.bHatsEx2 = 0xFFFFFFFF;
        st.bHatsEx3 = 0xFFFFFFFF;

        UINT buttons = st.Buttons;
        bool changed = (vx != prevVx || vy != prevVy || vz != prevVz || vrz != prevVrz || buttons != prevButtons);
        if (changed) {
            g_vjoy.Update(VJOY_ID, &st);
            prevVx = vx; prevVy = vy; prevVz = vz; prevVrz = vrz; prevButtons = buttons;
        }
        if (first) {
            g_out << "[连接] 收到首包（" << len << " 字节）X=" << vx << " Y=" << vy
                  << " Z=" << vz << " Rz=" << vrz << "\n";
        }
        if (mask != prevMask) {
            for (int i = 0; i < KEYMAP_N; ++i) {
                bool now2 = (mask & KEYMAP[i].mask) != 0;
                bool was2 = (prevMask & KEYMAP[i].mask) != 0;
                if (now2 && !was2) { PressKey(KEYMAP[i].vk); g_out << "[按键] " << KEYMAP[i].name << " 按下\n"; }
                else if (!now2 && was2) { ReleaseKey(KEYMAP[i].vk); g_out << "[按键] " << KEYMAP[i].name << " 松开\n"; }
            }
            prevMask = mask;
        }
        if (dial > 0) {
            if (!keyR) { PressKey('R'); keyR = true; }
            if (keyF)  { ReleaseKey('F'); keyF = false; }
        } else if (dial < 0) {
            if (!keyF) { PressKey('F'); keyF = true; }
            if (keyR)  { ReleaseKey('R'); keyR = false; }
        } else {
            if (keyR) { ReleaseKey('R'); keyR = false; }
            if (keyF) { ReleaseKey('F'); keyF = false; }
        }

        ++packetCount;
        if (packetCount % 500 == 1) {
            g_out << "[数据] X=" << vx << " Y=" << vy << " Z=" << vz << " Rz=" << vrz
                  << "  按钮=" << buttons << "  累计 " << packetCount << " 包"
                  << "  丢弃旧帧 " << skippedTotal << "\n";
        }
    }
    closesocket(sock);
    WSACleanup();
    g_vjoy.Relinq(VJOY_ID);
    return 0;
}

// ================================ main ================================
int main(int argc, char* argv[]) {
    SetConsoleOutputCP(65001);
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::string dir = ExeDir();
    g_out.f.open((dir + "vjoy.log").c_str(), std::ios::out | std::ios::trunc);

    // 配置
    std::string cfgPath = dir + "vjoy_usb.ini";
    if (!LoadCfg(cfgPath)) {
        WriteDefaultCfg(cfgPath);
        LoadCfg(cfgPath);
        g_out << "[配置] 首次运行，已生成 " << cfgPath << "\n";
    } else {
        g_out << "[配置] 已读取 " << cfgPath << "\n";
    }

    g_out << "==============================\n";
    g_out << "  dji_vjoy PC 端 (vJoy 2.1.9.1)\n";
    g_out << "  默认 = RC-N1 USB 直连\n";
    g_out << "==============================\n";

    std::string mode = "usb";
    std::string portSpec;
    bool keysMode = false;
    unsigned short udpPort = 8456;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--usb") {
            mode = "usb";
            if (i + 1 < argc) { std::string nx = argv[i + 1]; if (nx[0] != '-') { portSpec = nx; ++i; } }
        } else if (a == "--udp") {
            mode = "udp";
            if (i + 1 < argc) { std::string nx = argv[i + 1]; if (nx[0] != '-') { udpPort = (unsigned short)atoi(nx.c_str()); ++i; } }
        } else if (a == "--keys") {
            mode = "usb"; keysMode = true;
        } else if (a == "--scan") {
            mode = "scan";
        } else if (a.size() > 0 && a[0] != '-' && mode == "usb") {
            // 老用法：直接给端口号走 UDP
            mode = "udp";
            udpPort = (unsigned short)atoi(a.c_str());
        }
    }

    if (mode == "scan") {
        std::vector<std::string> ports = ListComPorts();
        if (ports.empty()) { g_out << "没有串口\n"; return 1; }
        SerialPort sp;
        for (const std::string& p : ports) {
            if (!sp.Open(p, g_cfg.usb_baud)) { g_out << p << " 打不开\n"; continue; }
            bool is = ProbeIsRemote(sp);
            sp.Close();
            g_out << p << (is ? "  <-- 是遥控器" : "  不是") << "\n";
        }
        return 0;
    }

    PrintCfgSummary();

    if (mode == "udp") return RunUdp(udpPort);

    int r = RunUsb(portSpec, keysMode);
    if (r == 2 || r == 3) {
        // USB 这条路没料（没插遥控器 / 找不到 0x26 轴帧）→ 自动落到手机 UDP，不用再开另一个程序
        g_out << "\n[自动回退] USB 没找到遥控器，改用手机 UDP 链路（端口 " << udpPort << "）\n";
        g_out << "           手机 App 里填本机 IP、端口 " << udpPort << "，点开始即可\n";
        g_out << "           只想用 USB：VJoy.exe --usb\n\n";
        return RunUdp(udpPort);
    }
    return r;
}
