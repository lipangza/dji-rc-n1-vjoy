#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RC-N1 按键诊断 v2 —— 记录每一次"按下事件"，确定每个位对应哪个键

已确认：按键全部在 0x3A 帧的第 29 号字节（0x1d），是位掩码。
已见过的位：0x01 / 0x20 / 0x40 / 0x80。还要把剩下的键按出来。

怎么用：
  1. 不要推杆（推杆会让轴副本字节一起变，干扰判断）
  2. 静置 1 秒（采基线，此时所有键都松开）
  3. 按下面的顺序，每个动作按一下或拨一下、停 1 秒再下一个：
       ① 拍照/录像键
       ② 云台回中/朝下键（如果有）
       ③ 三档拨杆 -> 第 1 档
       ④ 三档拨杆 -> 第 2 档
       ⑤ 三档拨杆 -> 第 3 档
       ⑥ 其他任何你能按的键，一个一个来
  4. Ctrl+C 结束，会打出「第 1 个动作 = 0x??」这样的事件序列

对照你自己按的顺序，就能把位名定下来。
"""
import sys, time

PING  = bytes([0x55,0x0d,0x04,0x33,0x0a,0x0e,0x03,0x00,0x40,0x06,0x01,0xf4,0x4a])
PING2 = bytes([0x55,0x0d,0x04,0x33,0x0a,0x0e,0x02,0x00,0x40,0x06,0x27,0x84,0x05])
FRAME_LEN = {0x13: 19, 0x26: 38, 0x3a: 58}
BTN_IDX = 29          # 0x1d —— 已实测确认的按键字节


def list_com():
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
        out, i = [], 0
        while True:
            try:
                _, val, _ = winreg.EnumValue(k, i)
                out.append(val); i += 1
            except OSError:
                break
        return out
    except Exception:
        return []


def find_port():
    try:
        import re, subprocess
        ps = ("Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -like "
              "'*DJI USB Virtual COM*' } | ForEach-Object { $_.Name }")
        out = subprocess.check_output(["powershell", "-NoProfile", "-Command", ps],
                                      stderr=subprocess.DEVNULL, timeout=20)
        m = re.search(r"COM(\d+)", out.decode("mbcs", "ignore"))
        if m:
            return "COM" + m.group(1)
    except Exception:
        pass
    import serial
    for p in list_com():
        try:
            s = serial.Serial(p, 115200, timeout=0.005, write_timeout=0.5)
            s.write(PING); s.write(PING2)
            t0 = time.time(); buf = b""
            while time.time() - t0 < 0.4:
                buf += s.read(256)
                if b"\x55\x3a" in buf or b"\x55\x26" in buf:
                    s.close(); return p
            s.close()
        except Exception:
            pass
    return None


def main():
    import serial
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port:
        print("找不到遥控器串口。插好数据线再试，或手动指定：python 按键诊断.py COM9")
        return
    print("串口: %s" % port)
    try:
        ser = serial.Serial(port, 115200, timeout=0.005, write_timeout=0.5)
    except Exception as e:
        print("打不开串口: %s" % e)
        print("（先关掉 DJI2vJoy.exe / VJoy.exe，它们会独占串口）")
        return

    print("\n===== 操作顺序 =====")
    print(" 1) 静置 1 秒（别碰任何键和杆）")
    print(" 2) 依次：拍照/录像键 -> 云台回中键(若有) -> 拨杆第1档 -> 第2档 -> 第3档 -> 其它键")
    print("    每做一个动作停 1 秒，做的时候不要推杆")
    print(" 3) 全部做完按 Ctrl+C")
    print("===================\n")

    buf = b""
    n3a = 0
    t_ping = t_ping2 = 0.0
    base_t0 = None
    baseline = None
    vals = []
    all_vals = set()
    events = []          # (相对秒, 'down'/'up', 值)
    last = None
    t_start = time.time()
    t_print = 0.0

    try:
        while True:
            now = time.time()
            if now - t_ping >= 0.010:
                ser.write(PING); t_ping = now
            if now - t_ping2 >= 0.025:
                ser.write(PING2); t_ping2 = now
            data = ser.read(2048)
            if data:
                buf += data
                while True:
                    i = buf.find(b"\x55")
                    if i < 0:
                        buf = b""; break
                    buf = buf[i:]
                    if len(buf) < 2: break
                    t = buf[1]
                    ln = FRAME_LEN.get(t)
                    if ln is None:
                        buf = buf[1:]; continue
                    if len(buf) < ln: break
                    f = buf[:ln]; buf = buf[ln:]
                    if t != 0x3a:
                        continue
                    n3a += 1
                    if base_t0 is None:
                        base_t0 = now
                    v = f[BTN_IDX]
                    all_vals.add(v)
                    # 前 1 秒当基线
                    if baseline is None:
                        vals.append(v)
                        if now - base_t0 >= 1.0 and len(vals) >= 6:
                            baseline = max(set(vals), key=vals.count)
                            last = baseline
                            print("基线 = 0x%02X（此时应所有键都松着）\n" % baseline)
                        continue
                    # 事件检测：从基线跳到非基线 = 按下；回到基线 = 松开
                    if v != last:
                        k = now - t_start
                        if v != baseline and last == baseline:
                            events.append((k, "down", v))
                            print("  [%6.2fs] 按下  0x%02X" % (k, v))
                        elif v == baseline and last != baseline:
                            events.append((k, "up", last))
                            print("  [%6.2fs] 松开  (刚才那个键)" % k)
                        last = v
                    # 实时显示当前值
                    if now - t_print >= 0.5:
                        t_print = now
                        print("\r    当前字节[%d] = 0x%02X   " % (BTN_IDX, v), end="", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    print("\n\n================ 汇总 ================")
    if baseline is None:
        print("没采到足够的 0x3A 帧")
        return
    print("收到按键帧 %d 个，基线 0x%02X\n" % (n3a, baseline))
    if not events:
        print("没有检测到任何按下事件。")
    else:
        n = 0
        for k, typ, v in events:
            if typ == "down":
                n += 1
                print("  第 %d 个动作  [%6.2fs]  ->  0x%02X  (二进制 %s)"
                      % (n, k, v, format(v, "08b")))
    print("\n字节[29] 一共出现过的值：%s"
          % " ".join("0x%02X" % v for v in sorted(all_vals)))
    print("\n把这份事件序列发我，并说明你按的顺序，我就能把位名对上。")
    print("已知：0x01 / 0x20 / 0x40 / 0x80 这四个位出现过。")


if __name__ == "__main__":
    main()
