#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RC-N1 轴位诊断 —— 推杆，看哪个字节偏移真的在动

用法：双击 轴位诊断.bat，或
      python 轴位诊断.py [COM9]

会实时显示 8 个候选轴位的当前值，并记录每个轴见过的最小/最大值。
把四个方向各推到底、再放开，然后看最后那张汇总表：
  变化大的偏移 = 真实轴；一直不动的 = 常量或没有这个轴。
"""
import sys, time

PING  = bytes([0x55,0x0d,0x04,0x33,0x0a,0x0e,0x03,0x00,0x40,0x06,0x01,0xf4,0x4a])
PING2 = bytes([0x55,0x0d,0x04,0x33,0x0a,0x0e,0x02,0x00,0x40,0x06,0x27,0x84,0x05])
FRAME_LEN = {0x13: 19, 0x26: 38, 0x3a: 58}
CANDS = [13, 16, 19, 22, 25, 28, 31, 34]
MID = 1024


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
                if b"\x55\x26" in buf:
                    s.close(); return p
            s.close()
        except Exception:
            pass
    return None


def le16(b, off):
    if off + 1 >= len(b):
        return None
    return b[off] | (b[off + 1] << 8)


def main():
    import serial
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port:
        print("找不到遥控器串口。插好数据线再试，或手动指定：python 轴位诊断.py COM9")
        return
    print("串口: %s" % port)
    try:
        ser = serial.Serial(port, 115200, timeout=0.005, write_timeout=0.5)
    except Exception as e:
        print("打不开串口: %s" % e)
        print("（多半是 DJI2vJoy / VJoy.exe 还占着它，先关掉）")
        return

    buf = b""
    lo = {c: 99999 for c in CANDS}
    hi = {c: -99999 for c in CANDS}
    cur = {c: MID for c in CANDS}
    n26 = 0
    t_ping = t_ping2 = 0.0
    t_print = 0.0
    print("\n现在把【左杆】上下左右、再【右杆】上下左右，各推到底再放开。")
    print("按 Ctrl+C 结束并看汇总。\n")

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
                    if t == 0x26:
                        n26 += 1
                        for c in CANDS:
                            v = le16(f, c)
                            if v is None: continue
                            cur[c] = v
                            if v < lo[c]: lo[c] = v
                            if v > hi[c]: hi[c] = v
            if now - t_print >= 0.3:
                t_print = now
                line = "  ".join("off%-2d=%4d" % (c, cur[c]) for c in CANDS)
                print("\r" + line + "   ", end="", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    print("\n\n==== 汇总（推杆后各偏移的取值范围）====")
    print("偏移  最小  最大  变化   判定")
    for c in CANDS:
        span = hi[c] - lo[c]
        if lo[c] == 99999:
            verdict = "没收到数据"
        elif span > 400:
            verdict = "★ 真实轴（在动）"
        elif span > 40:
            verdict = "抖动（可能死区边缘）"
        else:
            verdict = "常量（这个偏移没有轴）"
        print(" %3d  %5d %5d %5d   %s" % (c, lo[c], hi[c], span, verdict))
    print("\n把上面这张表发我，我按真实偏移改 VJoy.exe 的轴映射。")
    print("（帧里是每 3 字节一组，一共 8 组；脚本原来只取了前 5 组 13/16/19/22/25）")


if __name__ == "__main__":
    main()
