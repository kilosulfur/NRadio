#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
#include "AudioFileSource.h"

// read() 里"连一个字节都收不到"多久算枯竭: 超时后返回已读字节数,
// 让上层走软重同步/重连流程。要大于 I2S DMA 缓冲时长(32*128 帧 ≈ 93ms),
// 否则一次网络抖动就会把正在流畅播放的流判死。
#define READ_STARVE_MS 250

// 为什么不直接用 AudioFileSourceICYStream:
//   ICYStream::open() 里调了 http.setReuse(true), 而 Arduino-ESP32 的 setReuse() 会立刻把
//   _canReuse 置 true, 于是 http.end() 在 disconnect() 里走 "keep open for reuse" 分支变成空操作,
//   TCP 根本没断 -> http.connected() 仍为 true -> readInternal 里
//   "NODATA -> end() -> goto retry -> 还是 connected -> 再等 500ms -> NODATA" 无限循环。
//   只要有一次 500ms 的网络空档就永久卡死, 且永远看不到 "Stream disconnected"。
// 所以这里自己写一个极简 HTTP 流源:
//   * 读路径绝不做任何阻塞式 http.end()/重连, 断线判定只交给 isOpen() (client.connected())
//   * 只读 available() 范围内的字节, 永不阻塞等网络
//   * 数据先进静态预分配的环形缓冲, 解码器永远从 RAM 取
class NetStream : public AudioFileSource {
public:
  NetStream();
  virtual ~NetStream() override;

  bool open(const char *url) override;
  virtual uint32_t read(void *data, uint32_t len) override;
  virtual bool seek(int32_t pos, int dir) override { (void)pos; (void)dir; return false; }
  virtual bool close() override;
  virtual bool isOpen() override;
  virtual uint32_t getSize() override { return 0; }

  // 由主循环驱动: 把 TCP 收到的数据搬进环形缓冲
  void pump();
  uint32_t buffered() const { return _count; }
  void reset();
  void dropAll() { _head = _tail = _count = 0; }   // 只给吞吐测试用

  // 累计从网络收到的字节数, 用于看门狗判定"连上了但没数据"
  uint32_t totalBytes() const { return _total; }
  const char *lastError() const { return _err; }

  // ---- 健康度统计 (起播时调 resetStats()) ----
  void     resetStats()        { _lowWater = _count; _starves = 0; _eofs = 0; _consumed = 0; }
  uint32_t lowWater()   const { return _lowWater; }   // 播放期间缓冲的最低位
  uint32_t starves()    const { return _starves; }    // read() 等到枯竭的次数
  uint32_t eofs()       const { return _eofs; }       // read() 返回 0 的次数(会触发重同步)
  uint32_t consumed()   const { return _consumed; }   // 解码器实际取走的字节数

private:
  bool httpGet(const char *url, int depth);
  bool parseUrl(const char *url);
  int  readResponseHeaders();
  void push(const uint8_t *src, uint32_t n);

  WiFiClient _client;
  String  _host;
  uint16_t _port;
  String  _path;
  String  _loc;       // 重定向目标
  bool    _opened;
  const char *_err;

  uint32_t _total;
  uint32_t _consumed;

  // 环形缓冲
  uint8_t *_ring;
  uint32_t _head;
  uint32_t _tail;
  uint32_t _count;

  // 健康度统计
  uint32_t _lowWater;
  uint32_t _starves;
  uint32_t _eofs;
};
