"""在 PC 上逐个检测电台 URL 是否能在 C3 上用。

C3 端代码是裸 WiFiClient, 不支持 TLS, 所以:
  - 最终落在 https 上的   -> 必然连不上
  - 返回 30x 且 Location 是 https 的 -> 必然连不上
  - 返回 200 但前几个字节不是 MP3 帧同步字/ID3 的 -> 连上了但喂不进解码器

用法: python tools/station_probe.py
"""
import http.client
import socket
import sys
import urllib.parse

STATIONS = [
    ("Radio Paradise", "http://stream.radioparadise.com/mp3-128"),
    ("BBC", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"),
    ("HIT FM", "http://online.hitfm.ua/HitFM"),
    ("MANGORADIO", "http://mangoradio.stream.laut.fm/mangoradio"),
    ("ZZ959", "http://lhttp.qtfm.cn/live/5021381/64k.mp3"),
    ("Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"),
    ("Radio Swiss Jazz", "http://stream.srg-ssr.ch/m/rsj/mp3_128"),
    ("GD Music", "http://lhttp.qtfm.cn/live/1260/64k.mp3"),
    ("Lush", "http://ice1.somafm.com/lush-128-mp3"),
    ("ASIA Music", "http://lhttp.qingting.fm/live/4581/64k.mp3"),
    ("ASIA Music HD", "http://lhttp.qtfm.cn/live/15318341/64k.mp3"),
    ("FUN RADIO", "http://solid9.streamupsolutions.com/proxy/wpgqrttp/stream"),
]


def probe(url, timeout=12.0):
    """返回 (状态串, 说明, 码率信息)"""
    for _ in range(5):
        parts = urllib.parse.urlsplit(url)
        scheme = parts.scheme
        host = parts.hostname
        port = parts.port or (443 if scheme == "https" else 80)
        path = parts.path or "/"
        if parts.query:
            path += "?" + parts.query

        if scheme == "https":
            return "FAIL", "转到 https, C3 无 TLS", ""

        try:
            conn = http.client.HTTPConnection(host, port, timeout=timeout)
            conn.request(
                "GET", path,
                headers={
                    "Host": host,
                    "User-Agent": "ESP32-C3-Radio/1.0",
                    "Accept": "audio/mpeg,*/*",
                    "Icy-MetaData": "0",
                    "Connection": "close",
                },
            )
            resp = conn.getresponse()
            code = resp.status
            loc = resp.getheader("Location", "")
            ct = resp.getheader("Content-Type", "")
            metaint = resp.getheader("icy-metaint", "")
            br = resp.getheader("icy-br", "") or resp.getheader("Icy-Br", "")

            if 300 <= code < 400 and loc:
                conn.close()
                url = urllib.parse.urljoin(url, loc)
                continue

            if code != 200:
                conn.close()
                return "FAIL", "HTTP %d" % code, ""

            chunk = resp.read(4096)
            conn.close()
            kind = sniff(chunk)
            note = "HTTP 200, Content-Type=%s" % (ct or "-")
            if metaint:
                note += ", icy-metaint=%s" % metaint
            if br:
                note += ", icy-br=%s" % br
            return kind, note, br
        except (socket.timeout, socket.error, OSError) as exc:
            return "FAIL", "%s: %s" % (type(exc).__name__, exc), ""
    return "FAIL", "重定向过多", ""


def sniff(chunk):
    """看头几个字节能不能喂给 libhelix"""
    if not chunk:
        return "EMPTY"
    if chunk[:3] == b"ID3":
        return "OK"
    # MPEG 帧同步字: 11 位全 1
    n = min(len(chunk) - 1, 2048)
    i = 0
    while i < n:
        if chunk[i] == 0xFF and (chunk[i + 1] & 0xE0) == 0xE0:
            return "OK"
        i += 1
    return "BAD"


def main():
    print("%-18s %-6s %s" % ("STATION", "RESULT", "NOTE"))
    print("-" * 90)
    for name, url in STATIONS:
        result, note, _br = probe(url)
        print("%-18s %-6s %s" % (name, result, note))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
