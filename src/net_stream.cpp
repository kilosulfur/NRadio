#include "net_stream.h"
#include "config.h"

#include <lwip/sockets.h>   // setsockopt(SO_RCVBUF): 放大 TCP 接收窗口

// 静态预分配, 杜绝反复 malloc 造成的堆碎片
static uint8_t s_ring[RING_SIZE];

NetStream::NetStream()
  : _port(80), _opened(false), _err(""), _total(0), _consumed(0),
    _ring(s_ring), _head(0), _tail(0), _count(0),
    _lowWater(0), _starves(0), _eofs(0) {}

NetStream::~NetStream() { close(); }

void NetStream::reset() {
  _head = 0;
  _tail = 0;
  _count = 0;
  _total = 0;
  _consumed = 0;
  _lowWater = 0;
}

bool NetStream::parseUrl(const char *url) {
  const char *p = url;
  if (strncasecmp(p, "http://", 7) == 0) {
    p += 7;
  } else if (strncasecmp(p, "https://", 8) == 0) {
    _err = "https unsupported (no TLS)";
    return false;
  }
  const char *slash = strchr(p, '/');
  String authority = slash ? String(p).substring(0, (size_t)(slash - p)) : String(p);
  _path = slash ? String(slash) : String("/");
  int colon = authority.indexOf(':');
  if (colon >= 0) {
    _host = authority.substring(0, colon);
    _port = (uint16_t)authority.substring(colon + 1).toInt();
  } else {
    _host = authority;
    _port = 80;
  }
  if (_host.length() == 0) {
    _err = "bad url";
    return false;
  }
  return true;
}

// 返回 HTTP 状态码, -1 表示超时/失败; 重定向地址放进 _loc
int NetStream::readResponseHeaders() {
  String line;
  int code = -1;
  bool firstLine = true;
  _loc = "";
  uint32_t t0 = millis();

  while (millis() - t0 < 5000) {
    if (_client.available() > 0) {
      char c = (char)_client.read();
      if (c == '\n') {
        line.trim();
        if (firstLine) {
          firstLine = false;
          int sp = line.indexOf(' ');
          if (sp >= 0) code = line.substring(sp + 1).toInt();
        } else if (line.length() == 0) {
          break;  // 头部结束
        } else if (line.startsWith("Location:")) {
          _loc = line.substring(9);
          _loc.trim();
        }
        line = "";
      } else if (c != '\r') {
        line += c;
        if (line.length() > 512) line = "";  // 防止畸形头部撑爆内存
      }
    } else {
      delay(1);
      yield();
    }
  }
  return code;
}

bool NetStream::httpGet(const char *url, int depth) {
  if (depth > 5) {
    _err = "too many redirects";
    return false;
  }
  if (!parseUrl(url)) return false;

  _client.stop();
  if (!_client.connect(_host.c_str(), _port, 8000)) {
    _err = "tcp connect failed";
    return false;
  }

  // 关键一步: 把 TCP 接收窗口顶大。
  // 默认 5760B 在 RTT 380ms 的跨洋链路上只够 15 kB/s, 跑不动 128kbps 的流。
  // 必须放在 connect() 之后 —— 窗口是随后续 ACK 通告给对端的, 太晚设就白等一轮 RTT。
  {
    int fd = _client.fd();
    if (fd >= 0) {
      int want = TCP_RCVBUF_TARGET;
      int rc = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
      int got = 0;
      socklen_t len = sizeof(got);
      getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &len);
      Serial.printf("[net ] rcvbuf set=%d -> %d (rc=%d)\n", want, got, rc);
    }
  }

  String req;
  req.reserve(256);
  req += "GET ";
  req += _path;
  req += " HTTP/1.0\r\n";
  req += "Host: ";
  req += _host;
  if (_port != 80) { req += ':'; req += String(_port); }
  req += "\r\n";
  req += "User-Agent: ESP32-C3-Radio/1.0\r\n";
  req += "Accept: audio/mpeg,*/*\r\n";
  // 注意: 故意不发送 "Icy-MetaData: 1"。
  // 请求了元数据服务器就会按 icy-metaint 间隔插入元数据块, 一旦解析错一个字节,
  // 元数据会被当成音频喂进解码器 -> MP3FindSyncWord 失败 -> 每帧丢掉整个 1600B 缓冲。
  // 反正没有显示屏, 曲名只能打串口, 不值得为它冒稳定性风险。
  req += "Icy-MetaData: 0\r\n";
  req += "Connection: close\r\n\r\n";
  _client.print(req);

  int code = readResponseHeaders();
  if (code >= 300 && code < 400 && _loc.length() > 0) {
    _client.stop();
    if (_loc.startsWith("http")) {
      if (_loc.startsWith("https")) {
        _err = "redirected to https (unsupported)";
        return false;
      }
      return httpGet(_loc.c_str(), depth + 1);
    }
    // 相对路径
    String abs = String("http://") + _host;
    if (_port != 80) { abs += ':'; abs += String(_port); }
    abs += _loc;
    return httpGet(abs.c_str(), depth + 1);
  }

  if (code != 200) {
    _err = "http status != 200";
    _client.stop();
    return false;
  }
  _err = "";
  return true;
}

bool NetStream::open(const char *url) {
  close();
  reset();
  if (!httpGet(url, 0)) return false;
  _opened = true;
  return true;
}

bool NetStream::close() {
  _client.stop();      // 没有 setReuse, 这里会真正断开 TCP
  _opened = false;
  _head = _tail = _count = 0;
  return true;
}

bool NetStream::isOpen() { return _opened && _client.connected(); }

void NetStream::pump() {
  if (!_opened) return;
  if (!_client.connected()) return;

  int avail = _client.available();
  if (avail < 0) return;

  uint32_t budget = 16384;  // 单次搬运上限, 别把主循环独占太久
  while (avail > 0 && budget > 0) {
    uint32_t space = RING_SIZE - _count;
    if (space == 0) break;
    uint32_t contig = RING_SIZE - _head;
    uint32_t want = (uint32_t)avail;
    if (want > space) want = space;
    if (want > contig) want = contig;
    if (want > budget) want = budget;

    int n = _client.read(_ring + _head, want);
    if (n <= 0) break;
    _head = (_head + (uint32_t)n) % RING_SIZE;
    _count += (uint32_t)n;
    _total += (uint32_t)n;
    budget -= (uint32_t)n;
    avail -= n;
  }
}

// ⚠️ AudioFileSource::read() 的契约是"要么填满 len, 要么返回 0 表示 EOF"——
// libhelix 和 AudioGeneratorMP3a::FillBufferWithValidFrame 就是照这个写的。
// 之前这里的 120ms 是按"本次调用累计时长"算的, 网络稍慢时每个 read() 只返回几百字节,
// 解码器于是反复拿到半帧数据: 轻则不断 "stream lost", 重则 libhelix 顺着残缺的帧头
// 越过结构体边界写入 -> bitstream.c:232 Store access fault 直接死机(实测)。
// 改成: 只要数据还在flow就一直收; 只有真的连一个新字节都收不到超过 READ_STARVE_MS 才放弃。
uint32_t NetStream::read(void *data, uint32_t len) {
  uint8_t *dst = (uint8_t *)data;
  uint32_t got = 0;
  uint32_t lastActivity = millis();

  while (got < len) {
    if (_count > 0) {
      uint32_t contig = RING_SIZE - _tail;
      uint32_t n = len - got;
      if (n > _count) n = _count;
      if (n > contig) n = contig;
      memcpy(dst + got, _ring + _tail, n);
      _tail = (_tail + n) % RING_SIZE;
      _count -= n;
      got += n;
      lastActivity = millis();   // 数据在动 -> 继续等剩余的
      continue;
    }

    pump();
    if (_count > 0) continue;

    if (!_opened || !_client.connected()) break;          // 真断线
    if (millis() - lastActivity >= READ_STARVE_MS) {
      _starves++;
      break;                                               // 枯竭, 交给上层软重同步
    }
    delay(1);
    yield();
  }
  if (_count < _lowWater) _lowWater = _count;   // 记下缓冲最低位, 用来判断网络有没有真饿过
  _consumed += got;
  if (got == 0) _eofs++;
  return got;
}
