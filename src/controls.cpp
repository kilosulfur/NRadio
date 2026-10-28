#include "controls.h"
#include "config.h"

#include <driver/gpio.h>   // gpio_get_level() 是 IRAM_ATTR, 可以在中断里直接读寄存器
#include <esp_timer.h>     // esp_timer_get_time() 同样 IRAM_ATTR

namespace Controls {

// 毫秒计数器。不能用 millis(): 它内部走 xTaskGetTickCount(), 从 ISR 调不安全。
inline uint32_t IRAM_ATTR nowMs() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

// ===========================================================================
// 旋转编码器
// ===========================================================================
// 两相方波差 90°, 合起来走格雷码环: 00 -> 01 -> 11 -> 10 -> 00。
// 把"上次/这次"两状态拼成 4bit 索引查表, 直接得方向与有效性。
// 好处是抖动造成的非法跳变(如 00 -> 11)查表得 0, 自动被吃掉一大半,
// 比"在 A 的上升沿看一眼 B"那种土办法稳。
// 表是标准 Ben Buxton 表: 索引 = (旧 << 2) | 新, 值 +1 表示顺时针。
static const int8_t kEncTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0,
};

static volatile int     sEncDelta = 0;
static volatile uint8_t sEncState = 0;

// 必须用 gpio_get_level() 而不是 digitalRead(): 后者代码在 flash 里,
// 一旦某时刻 flash cache 被禁用(写 flash/NVS 时就会), IRAM 中断调它直接崩。
inline uint8_t readAB() {
  return (uint8_t)(((gpio_get_level((gpio_num_t)ENC_A_PIN) ? 1 : 0) << 1) |
                    (gpio_get_level((gpio_num_t)ENC_B_PIN) ? 1 : 0));
}

// A/B 两个脚都挂到这一个回调上, 任何边沿都重读一遍两相状态。
// 用中断而不是主循环轮询的原因: 播放时 loop() 每转一圈要花 20~100ms 解 MP3 帧,
// 这个采样率跟不上手拧的速度, 会明显丢格。
static void IRAM_ATTR onEncoderEdge() {
  uint8_t cur  = readAB();
  uint8_t prev = sEncState;
  sEncState    = cur;
  int8_t d     = kEncTable[(prev << 2) | cur];
  if (d) sEncDelta += d;
}

// ===========================================================================
// 按键: 同样走中断, 靠脉宽区分短按/长按
// ===========================================================================
// 轮询在这儿不够用: 杜邦线碰一下 GND 常常只有十几毫秒, 而 loop() 一圈要几十毫秒,
// 轮询很可能整个采样窗口都错过这段低电平, 表现为"按了没反应"。
static volatile bool     sActive  = false;
static volatile uint32_t sFallMs  = 0;
static volatile uint32_t sRiseMs  = 0;
static volatile uint32_t sBtnSeq  = 0;   // 每次电平翻转自增, 主循环靠它判断有没有新事件

static uint32_t sLastSeq   = 0;
static bool     sLongDone  = false;
static BtnEvent sPending   = BTN_NONE;

inline bool pinActive() {
  const bool hi = (gpio_get_level((gpio_num_t)BTN_PIN) != 0);
  return (BTN_ACTIVE == LOW) ? !hi : hi;
}

static void IRAM_ATTR onBtnEdge() {
  const bool     act = pinActive();
  const uint32_t t   = nowMs();
  if (act) {
    if (!sActive) { sActive = true; sFallMs = t; sBtnSeq++; }
  } else {
    if (sActive) { sActive = false; sRiseMs = t; sBtnSeq++; }
  }
}

// ---------------------------------------------------------------------------

void begin() {
#if BTN_ACTIVE == LOW
  pinMode(BTN_PIN, INPUT_PULLUP);
#else
  pinMode(BTN_PIN, INPUT_PULLDOWN);
#endif
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  delayMicroseconds(50);

  // 上电时如果按键已经被按着(或线还短接着 GND), 不能让它立刻算一次按压 ——
  // 否则开机即跳台/静音。把它记成"这次按压已处理过", 等用户真正松开再按才开始计数。
  sActive = pinActive();
  if (sActive) { sFallMs = nowMs(); sLongDone = true; }
  sBtnSeq = sLastSeq = 0;

  // 先读出初始状态, 否则第一次中断会拿 0 当旧状态, 白给一格
  sEncState = readAB();

  attachInterrupt(digitalPinToInterrupt(BTN_PIN),  onBtnEdge,     CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), onEncoderEdge, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), onEncoderEdge, CHANGE);
}

void poll() {
  // 长按: 按住期间到点就报一次
  if (sActive) {
    if (!sLongDone && (nowMs() - sFallMs) >= BTN_LONG_MS) {
      sLongDone = true;
      sPending  = BTN_LONG;
    }
  } else {
    sLongDone = false;
  }

  // 释放沿 -> 结算按压时长, 落在 [debounce, long) 区间内才算短按
  const uint32_t seq = sBtnSeq;
  if (seq != sLastSeq) {
    sLastSeq = seq;
    if (!sActive && sRiseMs) {
      const uint32_t held = sRiseMs - sFallMs;
      sRiseMs = 0;
      if (held >= BTN_DEBOUNCE_MS && held < BTN_LONG_MS) sPending = BTN_SHORT;
    }
  }
}

bool hasPending() { return sPending != BTN_NONE; }

BtnEvent takePending() {
  BtnEvent e = sPending;
  sPending   = BTN_NONE;
  return e;
}

int takeEncoderDelta() {
  // 和中断共用变量: 必须关中断再取/清零, 否则并发的那次 += 会被整体抹掉
  noInterrupts();
  int d = sEncDelta;
  sEncDelta = 0;
  interrupts();
  return d;
}

}  // namespace Controls
