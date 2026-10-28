"""抓一段时间的串口输出, 每行带上相对时间。

`pio device monitor` 要求交互式终端, 管道/重定向下跑不起来, 所以直接用 pyserial。

用法:
    python tools/serial_capture.py [秒数] [波特率] [--cmd 时刻:内容 ...]

--cmd 例: --cmd 8:i --cmd 20:n   # 第 8 秒发 i, 第 20 秒发 n
"""
import argparse
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("secs", nargs="?", type=float, default=30.0)
    ap.add_argument("baud", nargs="?", type=int, default=115200)
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--cmd", action="append", default=[], metavar="秒:内容")
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    cmds = []
    for item in args.cmd:
        at, payload = item.split(":", 1)
        cmds.append((float(at), payload.replace("\\n", "\n").encode("utf-8", "replace")))
    cmds.sort()

    import serial

    ser = serial.Serial(args.port, args.baud, timeout=0.2)

    if not args.no_reset:
        # 拉一把 RTS 让板子复位, 好看到完整开机日志
        ser.dtr = False
        ser.rts = True
        time.sleep(0.15)
        ser.rts = False

    start = time.time()
    end = start + args.secs
    next_cmd = 0
    pending = bytearray()

    def stamp():
        return "[%6.2fs] " % (time.time() - start)

    out = bytearray()
    while time.time() < end:
        now = time.time()
        if next_cmd < len(cmds) and now - start >= cmds[next_cmd][0]:
            ser.write(cmds[next_cmd][1])
            out.extend(("\n" + stamp() + ">>> sent %r\n" % cmds[next_cmd][1]).encode())
            next_cmd += 1

        waiting = ser.in_waiting
        if waiting:
            pending.extend(ser.read(waiting))
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                out.extend((stamp() + text + "\n").encode())
        else:
            time.sleep(0.02)

    if pending.strip():
        out.extend((stamp() + pending.decode("utf-8", "replace") + "\n").encode())

    ser.close()
    sys.stdout.write(out.decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
