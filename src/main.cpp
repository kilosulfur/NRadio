#include <Arduino.h>
#include <WiFi.h>

#include "AudioFileSource.h"
#include "AudioGeneratorMP3a.h"
#include "AudioOutputI2S.h"
#include "AudioLogger.h"

#include "config.h"
#include "net_stream.h"
#include "controls.h"

// ---------------------------------------------------------------------------
// 全局对象
// ---------------------------------------------------------------------------
// 带计数的 I2S 输出。两个作用:
//   1) 数每秒真正写出去多少采样点 -> 实测 I2S 的真实输出频率。
//      C3 没 APLL, 44100Hz 是 160MHz 分频除出来的, 和标称值可能差好几个百分点,
//      而"播放偏快"就等于"消耗 MP3 数据快于实时", 环形缓冲会被慢慢抽干。
//   2) 借 AdjustI2SRate() 这个虚函数做软件微调, 不用改库也能补上那个偏差。
class MeteredI2S : public AudioOutputI2S {
public:
  MeteredI2S(int port, int mode, int dma, int apll)
    : AudioOutputI2S(port, mode, dma, apll), _samples(0) {}

  virtual bool ConsumeSample(int16_t s[2]) override {
    if (!AudioOutputI2S::ConsumeSample(s)) return false;
    _samples++;          // 只有真写进 DMA 的才算, DMA 满时被拒的不计
    return true;
  }
  virtual int AdjustI2SRate(int hz) override {
    return (int)(((int64_t)hz * (1000000L + I2S_RATE_TRIM_PPM)) / 1000000L);
  }
  uint32_t takeSamples() { uint32_t n = _samples; _samples = 0; return n; }

private:
  uint32_t _samples;
};

static NetStream net;                 // 网络流 + 环形缓冲(静态 RAM)
static MeteredI2S *out = nullptr;     // I2S 输出, 全程只建一次
static AudioGeneratorMP3a *mp3 = nullptr;

static uint8_t  stationIdx = 0;
static float    volume     = VOL_DEFAULT;
static bool     muted      = false;

enum State { ST_CONNECT, ST_PREFILL, ST_PLAY, ST_BACKOFF };
static State state = ST_CONNECT;

static uint32_t backoffMs    = 2000;  // 2s -> 4s -> 8s -> 16s -> 30s(封顶)
static uint32_t nextActionMs = 0;     // ST_BACKOFF 到期时间
static uint32_t prefillStartMs = 0;
static uint32_t playStartMs    = 0;
static uint32_t lastBytes      = 0;
static uint32_t lastGrowMs     = 0;
static uint8_t  softResyncs    = 0;
static uint32_t lastWiFiTryMs  = 0;

// ---- 卡顿定位用统计 (每次起播清零) ----
static uint32_t statMarkMs  = 0;   // 上次打健康报告的时刻
static uint32_t statLoops   = 0;   // 这段时间内 loop() 转了多少圈
static uint32_t maxLoopUs   = 0;   // 单圈 loop() 最长耗时 —— 超过 DMA_WINDOW_MS 就等于 DMA 被抽空
static uint32_t maxDecUs    = 0;   // mp3->loop() 最长耗时 (解码 + 推 I2S)
static uint32_t maxPumpUs   = 0;   // net.pump() 最长耗时 (lwIP 系统调用, 可能被网络栈拖住)
static uint32_t stallCount  = 0;   // 单圈耗时 > DMA_WINDOW_MS 的次数
static uint32_t statRx0     = 0;   // 本统计窗口起始时的 rx 累计值
static uint32_t statSamp    = 0;   // 本窗口内写进 DMA 的采样点数
// 上面 loop/dec/pump 只覆盖了状态机里的 switch。这三段时间同样能掐断播放:
static uint32_t maxLogUs    = 0;   // 串口 println (见 logStatus 里的注释, 真的会阻塞)
static uint32_t maxMiscUs   = 0;   // 串口轮询 + 按键/编码器 + WiFi 保活(含 DNS/重连)
static uint32_t maxTailUs   = 0;   // 循环尾部的统计打印

// 前向声明: handleLost() 里要提前响应按键, 会调到下面才定义的这两个
static void serviceControls();
static void handleControlsImpl();
static void benchTcp(uint32_t ms);   // 串口 'b' 命令, 定义在命令处理之后

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
static void setAmp(bool on) {
#if AMP_SD_PIN >= 0
  digitalWrite(AMP_SD_PIN, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

static void led(bool on) { digitalWrite(LED_PIN, on ? LED_ON : LED_OFF); }

static void logStatus(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;

  // 【串口日志是会卡住音频的, 别不信】
  // USB CDC 的 write() 里先 xSemaphoreTake(tx_lock, tx_timeout_ms),
  // 再按 tries = tx_timeout_ms 每次等 1ms 往 TX FIFO 里推。
  // 也就是: 主机没及时读走 -> 一次 println 能阻塞几百毫秒。
  // 而整个 I2S DMA 只撑 DMA_WINDOW_MS(本项目 278ms), 超过就是听得到的断音。
  // 实测抓到过一次 2136ms 的单圈 loop, 源头就是播放中的一次 [vol ] 打印。
  // 所以这里: 空间不够就直接丢掉这行日志 —— 诊断信息丢了无所谓, 断音不行。
  int avail = Serial.availableForWrite();
  if (avail >= 0 && avail < n + 8) return;

  uint32_t t0 = micros();
  Serial.println(buf);
  uint32_t dt = micros() - t0;
  if (dt > maxLogUs) maxLogUs = dt;
}

// 音量统一出口。静音时只把增益压到 0, 不清掉用户拧好的值。
static void applyVolume() {
  if (out) out->SetGain(muted ? VOL_MIN : volume);
}

static void resetStats() {
  statMarkMs = millis();
  statLoops  = 0;
  maxLoopUs  = 0;
  maxDecUs   = 0;
  maxPumpUs  = 0;
  stallCount = 0;
  statRx0    = net.totalBytes();
  statSamp   = 0;
  maxLogUs   = 0;
  maxMiscUs  = 0;
  maxTailUs  = 0;
  if (out) out->takeSamples();
  net.resetStats();
}

// 一行看清楚"卡顿到底卡在哪":
//   max loop  —— 单圈 loop() 最长耗时。超过 DMA_WINDOW_MS 就等于 DMA 被抽空过。
//   max dec   —— 解码一帧 + 推 I2S 的耗时, 看 CPU 够不够。
//   max pump  —— lwIP 系统调用耗时, 看是不是被网络栈拖住。
//   low       —— 播放期间环形缓冲的最低位, 0 说明网络真的饿过。
static void printStats() {
  uint32_t window = millis() - statMarkMs;
  uint32_t rx     = net.totalBytes() - statRx0;
  logStatus("[stat] win=%lus loops=%lu | max loop=%lums dec=%lums pump=%lums | stalls=%lu",
            (unsigned long)(window / 1000), (unsigned long)statLoops,
            (unsigned long)(maxLoopUs / 1000), (unsigned long)(maxDecUs / 1000),
            (unsigned long)(maxPumpUs / 1000), (unsigned long)stallCount);
  logStatus("[stat]   segments: log=%lums misc=%lums tail=%lums",
            (unsigned long)(maxLogUs / 1000), (unsigned long)(maxMiscUs / 1000),
            (unsigned long)(maxTailUs / 1000));
  logStatus("[stat] buf=%u/%u low=%u | %.1f KB/s | starve=%u eof=%u | heap=%u",
            (unsigned)net.buffered(), (unsigned)RING_SIZE, (unsigned)net.lowWater(),
            window ? (double)rx / (double)window : 0.0,
            (unsigned)net.starves(), (unsigned)net.eofs(), (unsigned)ESP.getFreeHeap());
  // out = 实测 I2S 输出频率(应 ≈ 流的采样率); mp3 = 实测被消耗的码率(应 ≈ icy-br)。
  // 两者任何一个对不上, 环形缓冲就会朝一个方向漂, 撑满或饿死只是时间问题。
  if (out) statSamp += out->takeSamples();
  logStatus("[stat] out=%.0f Hz | mp3=%.0f kbps | consumed=%u",
            window ? (double)statSamp * 1000.0 / (double)window : 0.0,
            window ? (double)net.consumed() * 8.0 / (double)window : 0.0,
            (unsigned)net.consumed());
  resetStats();
}

// 编码器每一格转一圈 + 一圈 -, 串口 +/- 命令也走这里共享同一套 clamp 和日志。
static void nudgeVolume(int deltaSteps) {
  if (!deltaSteps) return;
  volume += deltaSteps * VOL_STEP;
  if (volume < VOL_MIN) volume = VOL_MIN;
  if (volume > VOL_MAX) volume = VOL_MAX;
  if (volume > VOL_MIN) muted = false;   // 往回拧自动解除静音
  applyVolume();
  logStatus("[vol ] %s%.2f", muted ? "(muted) " : "", muted ? 0.0f : volume);
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static bool tryConnectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  for (size_t i = 0; i < wifiCount; i++) {
    logStatus("[wifi] try %s ...", wifiList[i].ssid);
    WiFi.begin(wifiList[i].ssid, wifiList[i].pass);
    uint32_t t0 = millis();
    while (millis() - t0 < 8000) {
      if (WiFi.status() == WL_CONNECTED) break;
      // 连 WiFi 最多要试 3 个 AP x 8 秒, 这期间别把按键完全饿死
      serviceControls();
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      // 第一件事: 关掉 modem sleep。默认开着的休眠会让 WiFi 周期性休眠,
      // 唤醒瞬间产生几十毫秒接收空档, 直接把音频 DMA 饿空 -> 断续 + 咔哒声。
      WiFi.setSleep(false);
      logStatus("[wifi] connected: %s  RSSI=%d  IP=%s",
                WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
      return true;
    }
    WiFi.disconnect();
  }
  logStatus("[wifi] all credentials failed");
  return false;
}

static void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  // 掉线重连限频 20s, 否则掉线期间会把整个 loop 拖死
  if (millis() - lastWiFiTryMs < 20000) return;
  lastWiFiTryMs = millis();
  if (tryConnectWiFi()) {
    state = ST_CONNECT;   // 网络恢复, 立刻重建流
    backoffMs = 2000;
  }
}

// ---------------------------------------------------------------------------
// 播放控制
// ---------------------------------------------------------------------------
static void teardownGenerator() {
  // 千万别调 mp3->stop(): 它内部会 output->stop() -> i2s_zero_dma_buffer()
  // 并把 i2sOn 置 false, 下次 begin() 就重新 install I2S 驱动 -> 切台必爆音。
  // 直接 delete: ~AudioGeneratorMP3a 只做 MP3FreeDecoder, 不会碰 output。
  if (mp3) { delete mp3; mp3 = nullptr; }
}

static void teardownStream() {
  teardownGenerator();
  if (net.isOpen()) net.close();
  net.reset();
}

static void bumpBackoff() {
  backoffMs = backoffMs >= 30000 ? 30000 : backoffMs * 2;
  if (backoffMs > 30000) backoffMs = 30000;   // 封顶 30s, 永远不设"放弃"终态
  nextActionMs = millis() + backoffMs;
  state = ST_BACKOFF;
  setAmp(false);
  led(false);
}

static bool openCurrentStation() {
  const char *url = radioStations[stationIdx].url;
  logStatus("[net ] open: %s", url);
  if (!net.open(url)) {
    logStatus("[net ] open failed: %s", net.lastError());
    return false;
  }
  logStatus("[net ] connected");
  return true;
}

static bool startDecoder() {
  mp3 = new AudioGeneratorMP3a();
  if (!mp3->begin(&net, out)) {
    logStatus("[dec ] begin failed");
    delete mp3;
    mp3 = nullptr;
    return false;
  }
  out->SetGain(volume);
  applyVolume();          // 音量入口统一到 applyVolume(), 保持静音态也可以跨台维持
  setAmp(true);
  led(true);
  playStartMs  = millis();
  lastBytes    = net.totalBytes();
  lastGrowMs   = millis();
  softResyncs  = 0;
  resetStats();
  logStatus("[dec ] playing: %s", radioStations[stationIdx].name);
  return true;
}

// 起播前预填充。网络慢时提前放弃, 死等只会让起播体验更差。
static bool prefetch(uint32_t target, uint32_t minBytes, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  uint32_t last = 0;
  uint32_t lastGrow = t0;

  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() != WL_CONNECTED) return false;
    net.pump();

    // 预填充最长要等 12 秒, 若不在这里继续喂控制逻辑, 期间按键会完全没反应
    serviceControls();
    if (Controls::hasPending()) return false;   // 有按键排队 -> 收手, 交回状态机处理

    uint32_t b = net.buffered();
    if (b >= target) return true;
    if (b > last) { last = b; lastGrow = millis(); }
    if (b >= minBytes && (millis() - lastGrow) > 2500) return true;
    if (!net.isOpen() && b == 0) return false;
    delay(2);
    yield();
  }
  return net.buffered() > 0;
}

// 缓冲被抽干但 TCP 还活着 -> 软重同步: 只让解码器重新找 MP3 帧同步字,
// 省掉 TCP 握手 + 重新预填充的巨大代价, 恢复快得多。
static bool softResync() {
  teardownGenerator();
  if (!prefetch(PREFILL_TARGET, PREFILL_MIN, PREFILL_TIMEOUT)) return false;
  return startDecoder();
}

static void beginConnect() {
  teardownStream();
  if (WiFi.status() != WL_CONNECTED) { bumpBackoff(); return; }
  if (!openCurrentStation()) { bumpBackoff(); return; }
  prefillStartMs = millis();
  state = ST_PREFILL;
}

static void handleLost() {
  // AudioGeneratorMP3a::loop() 只有在 file->read() 返回 0 时才变 false,
  // 即"环形缓冲被抽干", 不是解码器坏了。
  logStatus("[dec ] stream lost (soft %d, open=%d)", softResyncs, net.isOpen());
  if (net.isOpen() && softResyncs < SOFT_RESYNC_MAX) {
    softResyncs++;
    if (softResync()) return;
    // 重同步过程中按了键: 立刻响应, 别再走退避让用户干等
    if (Controls::hasPending()) { teardownStream(); handleControlsImpl(); return; }
  }
  teardownStream();
  bumpBackoff();
}

// ===========================================================================
// 按键 / 编码器
// ===========================================================================
// 这个要挂在所有可能长时间阻塞的地方(预填充最多要等 12s, 起播前也必须 obey 按键),
// 否则用户按一下要等十几秒才有反应。
// 注意: 这段每圈 loop() 都要跑一遍, 播放时一分钟要跑几千次。
// 只在真的有事件时才允许它碰 Serial —— 打印是潜伏的卡顿源。
static void serviceControls() {
  Controls::poll();

  int d = Controls::takeEncoderDelta();
  if (d) nudgeVolume(d);
}

// 取出排队的按键事件并执行。只在状态机的安全边界调用 —— 执行过程中会拆重建整套流。
static void handleControlsImpl() {
  switch (Controls::takePending()) {
    case BTN_SHORT:   // 短按: 下一个电台
      stationIdx = (stationIdx + 1) % stationCount;
      logStatus("[btn ] next -> [%d/%d] %s", stationIdx + 1, stationCount,
                radioStations[stationIdx].name);
      backoffMs = 2000;
      beginConnect();
      break;

    case BTN_LONG:    // 长按: 静音开关
      muted = !muted;
      applyVolume();
      logStatus("[btn ] mute %s", muted ? "ON" : "OFF");
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// 串口命令 (逐字符读, 绝不用 readStringUntil —— 它默认超时 1s, 会阻塞整个 loop 把解码饿死)
// ---------------------------------------------------------------------------
static String cmdLine;

static void handleCommand(const String &c) {
  if (c.length() == 0) return;
  char ch = c.charAt(0);

  if (ch == 'n' || ch == 'N') {
    stationIdx = (stationIdx + 1) % stationCount;
    logStatus("[cmd ] next -> [%d] %s", stationIdx, radioStations[stationIdx].name);
    backoffMs = 2000;
    beginConnect();
  } else if (ch == 'p' || ch == 'P') {
    stationIdx = (stationIdx + stationCount - 1) % stationCount;
    logStatus("[cmd ] prev -> [%d] %s", stationIdx, radioStations[stationIdx].name);
    backoffMs = 2000;
    beginConnect();
  } else if (ch >= '0' && ch <= '9') {
    uint8_t i = (uint8_t)(ch - '0');
    if (i < stationCount) {
      stationIdx = i;
      logStatus("[cmd ] select -> [%d] %s", stationIdx, radioStations[stationIdx].name);
      backoffMs = 2000;
      beginConnect();
    }
  } else if (ch == '+' || ch == '=') {
    nudgeVolume(2);     // 和编码器共用一套 + clamp + 日志
  } else if (ch == '-' || ch == '_') {
    nudgeVolume(-2);
  } else if (ch == 'm' || ch == 'M') {
    muted = !muted;
    applyVolume();
    logStatus("[cmd ] mute %s", muted ? "ON" : "OFF");
  } else if (ch == 'i' || ch == 'I') {
    logStatus("[info] sta=%s rssi=%d heap=%u", WiFi.SSID().c_str(), WiFi.RSSI(),
              (unsigned)ESP.getFreeHeap());
    logStatus("[info] [%d/%d] %s  buf=%u rx=%u", stationIdx + 1, stationCount,
              radioStations[stationIdx].name, net.buffered(), net.totalBytes());
    logStatus("[info] btn=%s vol=%.2f%s",
              digitalRead(BTN_PIN) == BTN_ACTIVE ? "PRESSED" : "idle",
              muted ? 0.0f : volume, muted ? " (muted)" : "");
    logStatus("[info] dma=%dms  max loop=%lums dec=%lums pump=%lums  stalls=%lu",
              (int)DMA_WINDOW_MS, (unsigned long)(maxLoopUs / 1000),
              (unsigned long)(maxDecUs / 1000), (unsigned long)(maxPumpUs / 1000),
              (unsigned long)stallCount);
    logStatus("[info]   segments: log=%lums misc=%lums tail=%lums",
              (unsigned long)(maxLogUs / 1000), (unsigned long)(maxMiscUs / 1000),
              (unsigned long)(maxTailUs / 1000));
  } else if (ch == 'r' || ch == 'R') {
    logStatus("[cmd ] reconnect");
    backoffMs = 2000;
    beginConnect();
  } else if (ch == 'l' || ch == 'L') {
    for (size_t i = 0; i < stationCount; i++) logStatus("  [%2d] %s", i, radioStations[i].name);
  } else if (ch == 'b' || ch == 'B') {
    benchTcp(15000);
  }
}

// 只收不播: 用来量 ESP 这一侧的 TCP 极限吞吐。
// 和 PC 端探测结果对比, 就能判断"环形缓冲喂不饱"是网络收不动, 还是播放器吃太快。
// 注意直播流刚连上会灌一波特发数据, 所以测出来的数会高于标称码率 —— 正是要看它。
static void benchTcp(uint32_t ms) {
  teardownStream();
  if (WiFi.status() != WL_CONNECTED) { logStatus("[bench] no wifi"); return; }
  if (!openCurrentStation()) { logStatus("[bench] open failed: %s", net.lastError()); return; }

  uint32_t t0    = millis();
  uint32_t total = 0;
  while (millis() - t0 < ms) {
    net.pump();
    total += net.buffered();
    net.dropAll();        // 丢掉, 让环形缓冲始终有空间, 只测网络
    yield();
  }
  uint32_t dt = millis() - t0;
  logStatus("[bench] %u bytes in %ums = %.1f kB/s (%.0f kbit/s)",
            (unsigned)total, (unsigned)dt, (double)total / (double)dt,
            (double)total * 8.0 / (double)dt);
  teardownStream();
  beginConnect();
}

static void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      handleCommand(cmdLine);
      cmdLine = "";
    } else if (cmdLine.length() < 32) {
      cmdLine += c;
    }
  }
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== ESP32-C3 network radio ===");

  // audioLogger 默认是 &silencedLogger, 库的内部错误一个字都不打印。
  // 调试期必须打开, 否则"连不上流"完全没线索。
  audioLogger = &Serial;

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);  // 不显式驱动时板载 LED 是悬浮输入, 会被耦合翻转并往电源灌噪声
#if AMP_SD_PIN >= 0
  pinMode(AMP_SD_PIN, OUTPUT);
  digitalWrite(AMP_SD_PIN, LOW);
#endif

  setAmp(false);

  // I2S 输出只建一次。反复 new/delete 会反复 install/uninstall 驱动, 切台爆音且不稳。
  out = new MeteredI2S(0, AudioOutputI2S::EXTERNAL_I2S, DMA_BUF_COUNT,
                       AudioOutputI2S::APLL_DISABLE);
  // C3 上库默认的 I2S 引脚(26/25/22)不存在, 必须显式指定。
  //
  // 【坑】第四个参数 mclk 不能省。AudioOutputI2S 构造函数里写着 mclkPin = 0,
  // 而 mclkPin 是 uint8_t, 且三参数版 SetPinout() 也不改它 —— 于是 i2s_set_pin()
  // 会把 MCLK 路由到 GPIO0, 这块 pad 就从 GPIO 手里被抢走了: 它变成 I2S 的输出脚,
  // digitalRead() 永远读到 LOW, 症状就是"按钮像一直按着"。
  // 但也不能传 I2S_PIN_NO_CHANGE(-1): uint8_t 会把 -1 存成 255, i2s_set_pin()
  // 在 mck_io_num 校验这里就报 "mclk config failed" 并提前返回,
  // 压根走不到路由 BCLK/LRC/DIN 那一步 —— 那样连声音都没了。
  // 所以给它一个真实的、悬空的 GPIO(见 config.h 里的 I2S_MCLK)。
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DIN, I2S_MCLK);
  out->SetGain(volume);
  logStatus("[cfg ] ring=%uKB dma=%d*128f=%dms heap=%u", (unsigned)(RING_SIZE / 1024),
            (int)DMA_BUF_COUNT, (int)DMA_WINDOW_MS, (unsigned)ESP.getFreeHeap());

  Controls::begin();
  logStatus("[io  ] button GPIO%d (short press = next, long press = mute)", BTN_PIN);
  logStatus("[io  ] encoder A=GPIO%d B=GPIO%d (common -> GND)", ENC_A_PIN, ENC_B_PIN);
  if (digitalRead(BTN_PIN) == BTN_ACTIVE) {
    logStatus("[io  ] WARN: button already pressed at boot -> ignored until released");
  }

  if (!tryConnectWiFi()) {
    logStatus("[wifi] initial connect failed, will retry in loop");
  }

  Serial.println("commands: n/p next/prev, 0-9 select, +/- volume, m mute, i info, l list, r reconnect");
}

void loop() {
  const uint32_t loopT0  = micros();
  const bool     wasPlay = (state == ST_PLAY);
  const uint32_t miscT0  = micros();

  pollSerial();
  serviceControls();
  handleControlsImpl();
  ensureWiFi();

  uint32_t dMisc = micros() - miscT0;
  if (dMisc > maxMiscUs) maxMiscUs = dMisc;

  switch (state) {
    case ST_CONNECT:
      if (WiFi.status() == WL_CONNECTED) beginConnect();
      else { delay(200); }
      break;

    case ST_PREFILL:
      if (!prefetch(PREFILL_TARGET, PREFILL_MIN, PREFILL_TIMEOUT)) {
        // 预填充中途按了键: 这半壶水直接作废, 立刻去执行切换
        if (Controls::hasPending()) { handleControlsImpl(); break; }
        logStatus("[net ] prefetch failed: %s", net.lastError());
        teardownStream();
        bumpBackoff();
        break;
      }
      if (Controls::hasPending()) { handleControlsImpl(); break; }
      if (!startDecoder()) {
        teardownStream();
        bumpBackoff();
        break;
      }
      state = ST_PLAY;
      break;

    case ST_PLAY:
      {
      uint32_t tPump1 = micros();
      net.pump();
      uint32_t dPump1 = micros() - tPump1;
      if (dPump1 > maxPumpUs) maxPumpUs = dPump1;

      uint32_t tDec = micros();
      bool ok = (mp3 && mp3->loop());
      uint32_t dDec = micros() - tDec;
      if (dDec > maxDecUs) maxDecUs = dDec;
      if (!ok) { handleLost(); break; }

      // mp3->loop() 一次要解一帧并把它推到 I2S DMA, 这期间是收不到数据的。
      // 出来后补搬一趟, 否则单次循环里的 TCP 吞吐跟不上 128kbps 的播放消耗,
      // 环形缓冲被一路抽干 -> 每播 2~3 秒就 "stream lost" 一次。
      uint32_t tPump2 = micros();
      net.pump();
      uint32_t dPump2 = micros() - tPump2;
      if (dPump2 > maxPumpUs) maxPumpUs = dPump2;
      }

      // 数据看门狗: 字节计数长时间不增长就强制重启, 兜住"连上了但没数据"的假死
      if (net.totalBytes() != lastBytes) {
        lastBytes  = net.totalBytes();
        lastGrowMs = millis();
      } else if (millis() - lastGrowMs > WATCHDOG_MS) {
        logStatus("[net ] watchdog: no data for %d ms", WATCHDOG_MS);
        teardownStream();
        bumpBackoff();
        break;
      }

      // 稳定播放超过 30s 就清零退避计数, 偶发断流能立刻快速重连
      if (millis() - playStartMs > 30000) {
        playStartMs = millis();
        backoffMs   = 2000;
        softResyncs = 0;
      }
      break;

    case ST_BACKOFF:
      if (millis() >= nextActionMs) {
        logStatus("[net ] retry in %lus", (unsigned long)(backoffMs / 1000));
        beginConnect();
      } else {
        delay(50);
      }
      break;
  }

  // 卡顿定位: 只统计 ST_PLAY 的圈耗时。单圈耗时超过 DMA_WINDOW_MS 就意味着
  // 这一圈里没人喂过 DMA, 而 DMA 恰好在这期间放完 -> 一次可听见的断音。
  if (wasPlay) {
    const uint32_t tailT0 = micros();

    statLoops++;
    uint32_t dt = micros() - loopT0;
    if (dt > maxLoopUs) maxLoopUs = dt;
    if (dt > (uint32_t)DMA_WINDOW_MS * 1000UL) stallCount++;
    if (millis() - statMarkMs >= STAT_INTERVAL_MS) printStats();

    uint32_t dTail = micros() - tailT0;
    if (dTail > maxTailUs) maxTailUs = dTail;
  }

  yield();
}
