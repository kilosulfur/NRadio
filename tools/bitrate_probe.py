"""测量直播流的真实字节速率。

用法: python bitrate_probe.py [url] [seconds]
用途: 判断"ESP 上测到的吞吐低"到底是服务器只发这么多, 还是 ESP 自己收不动。
"""
import socket
import sys
import time

DEFAULT_URL = "http://stream.radioparadise.com/mp3-128"


def probe(url, seconds):
    assert "://" in url
    rest = url.split("://", 1)[1]
    slash = rest.find("/")
    authority = rest if slash < 0 else rest[:slash]
    path = "/" if slash < 0 else rest[slash:]
    if ":" in authority:
        host, port = authority.split(":", 1)
        port = int(port)
    else:
        host, port = authority, 80

    req = (
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: bitrate-probe/1.0\r\n"
        "Accept: audio/mpeg,*/*\r\n"
        "Icy-MetaData: 0\r\n"
        "Connection: close\r\n\r\n" % (path, host)
    )

    s = socket.create_connection((host, port), timeout=10)
    s.sendall(req.encode())

    # 读响应头, 顺便看看服务器自己广播的码率
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(1)
        if not chunk:
            break
        buf += chunk
    head = buf.decode("latin-1")
    print("--- response ---")
    for line in head.split("\r\n"):
        low = line.lower()
        if low.startswith("icy-br") or low.startswith("icy-name") or \
           low.startswith("content-type") or low.startswith("http/"):
            print("   ", line)

    # 分片打印: 直播流刚连上时会先灌一波特发数据(服务器把缓冲一次性推过来),
    # 之后才回落到标称码率。只看总平均值会把这波突发算进去, 得出虚高的数。
    t0 = time.time()
    total = 0
    slice_t0 = t0
    slice_n = 0
    end = t0 + seconds
    print("\n--- 每 5 秒的实际速率 ---")
    while time.time() < end:
        s.settimeout(max(0.1, end - time.time()))
        try:
            d = s.recv(65536)
        except socket.timeout:
            break
        if not d:
            break
        total += len(d)
        slice_n += len(d)
        now = time.time()
        if now - slice_t0 >= 5.0:
            r = slice_n / (now - slice_t0)
            print("   t=%5.1fs  %7.1f kB/s  (%.0f kbit/s)" %
                  (now - t0, r / 1000.0, r * 8 / 1000.0))
            slice_t0 = now
            slice_n = 0
    dt = time.time() - t0
    s.close()

    bps = total / dt if dt > 0 else 0
    print("\nreceived %d bytes in %.2fs" % (total, dt))
    print("  = %.1f kB/s  (= %.0f kbit/s)  <-- 含开头突发, 比真实码率偏高" %
          (bps / 1000.0, bps * 8 / 1000.0))


if __name__ == "__main__":
    url = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    print("probing %s for %.0fs ..." % (url, secs))
    probe(url, secs)
