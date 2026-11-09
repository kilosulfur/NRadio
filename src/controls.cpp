#include "controls.h"
#include "config.h"

#include <driver/gpio.h>   // gpio_get_level() 是 IRAM_ATTR, 可以在中断里直接读寄存器
#include <esp_timer.h>     // esp_timer_get_time() 同样 IRAM_ATTR

namespace Controls {

// 毫秒计数器。不能用 millis(): 它内部走 xTaskGetTickCount(), 从 ISR 调不安全。
inline uint32_t IRAM_ATTR nowMs() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

// ===========================================================================
// 按键: 每颗脚挂 CHANGE 中断, 靠脉宽区分短按/长按
// ===========================================================================
// 轮询在这儿不够用: 杜邦线碰一下 GND 常常只有十几毫秒, 而 loop() 一圈要几十毫秒,
// 轮询很可能整个采样窗口都错过这段低电平, 表现为"按了没反应"。
struct BtnState {
  int      pin;
  bool     repeat;                 // true: 按住不放会连发 SHORT(音量键用)
                                   // false: 长按发一次 LONG, 短按发一次 SHORT

  volatile bool     active;        // 当前电平处于按下态?
  volatile uint32_t fallMs;        // 按下时刻
  volatile uint32_t riseMs;        // 松开时刻
  volatile uint32_t seq;           // 每次电平翻转自增, 主循环靠它判断有没有新边沿

  // --- 主循环侧(无并发) ---
  uint32_t lastSeq;
  bool     longDone;               // 本次按压已发过 LONG(主键), 开机即按住时也置位以吞掉这次
  bool     repeated;               // 本次按压已连发过 -> 松手不再补 SHORT
  uint32_t lastRepeatMs;
  BtnEvent pending;
};

static BtnState sBtn[BTN_COUNT] = {
  /* BTN_MAIN      */ { BTN_PIN,     false },
  /* BTN_VOL_UP    */ { VOL_UP_PIN,  true  },
  /* BTN_VOL_DOWN  */ { VOL_DOWN_PIN, true },
};

// 必须用 gpio_get_level() 而不是 digitalRead(): 后者代码在 flash 里,
// 一旦某时刻 flash cache 被禁用(写 flash/NVS 时就会), IRAM 中断调它直接崩。
static void IRAM_ATTR btnEdgeISR(uint8_t i) {
  BtnState &b    = sBtn[i];
  const bool hi  = (gpio_get_level((gpio_num_t)b.pin) != 0);
  const bool act = (BTN_ACTIVE == LOW) ? !hi : hi;
  const uint32_t t = nowMs();
  if (act) {
    if (!b.active) { b.active = true; b.fallMs = t; b.seq++; }
  } else {
    if (b.active) { b.active = false; b.riseMs = t; b.seq++; }
  }
}

// attachInterrupt 只收 void(*)(), 不能带参, 每颗脚包一层
static void IRAM_ATTR onMainEdge() { btnEdgeISR(BTN_MAIN); }
static void IRAM_ATTR onUpEdge()   { btnEdgeISR(BTN_VOL_UP); }
static void IRAM_ATTR onDownEdge() { btnEdgeISR(BTN_VOL_DOWN); }

// ---------------------------------------------------------------------------

void begin() {
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    BtnState &b = sBtn[i];
#if BTN_ACTIVE == LOW
    pinMode(b.pin, INPUT_PULLUP);
#else
    pinMode(b.pin, INPUT_PULLDOWN);
#endif
    delayMicroseconds(50);
    const bool hi  = (digitalRead(b.pin) != 0);
    const bool act = (BTN_ACTIVE == LOW) ? !hi : hi;

    // 上电时如果按键已经被按着(或线还短接着 GND), 不能让它立刻算一次按压 ——
    // 否则开机即跳台/静音。把它记成"这次按压已处理过", 等用户真正松开再按才开始计数。
    b.active   = act;
    b.fallMs   = act ? nowMs() : 0;
    b.longDone = act;
    b.repeated = false;
    b.seq = b.lastSeq = 0;
    b.lastRepeatMs = 0;
    b.pending = BTN_NONE;
  }

  attachInterrupt(digitalPinToInterrupt(BTN_PIN),      onMainEdge, CHANGE);
  attachInterrupt(digitalPinToInterrupt(VOL_UP_PIN),   onUpEdge,   CHANGE);
  attachInterrupt(digitalPinToInterrupt(VOL_DOWN_PIN), onDownEdge, CHANGE);
}

void poll() {
  const uint32_t t = nowMs();
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    BtnState &b = sBtn[i];

    // 按住期间的事件(长按 / 连发)
    if (b.active) {
      const uint32_t held = t - b.fallMs;
      if (b.repeat) {
        // 音量键: 过了防抖期立刻给一步, 之后每 VOL_REPEAT_MS 连发一步。
        // "先给一步再连发"比"等 600ms 才开始动"手感好得多。
        if (held >= BTN_DEBOUNCE_MS) {
          if (b.lastRepeatMs == 0) {
            b.lastRepeatMs = t;
            b.repeated     = true;
            b.pending      = BTN_SHORT;
          } else if ((t - b.lastRepeatMs) >= VOL_REPEAT_MS) {
            b.lastRepeatMs = t;
            b.pending      = BTN_SHORT;
          }
        }
      } else if (!b.longDone && held >= BTN_LONG_MS) {
        b.longDone = true;
        b.pending  = BTN_LONG;
      }
    }

    // 释放沿 -> 结算按压时长
    const uint32_t seq = b.seq;
    if (seq != b.lastSeq) {
      b.lastSeq = seq;
      if (!b.active && b.riseMs) {
        const uint32_t held = b.riseMs - b.fallMs;
        b.riseMs = 0;
        if (b.repeat) {
          // 连发过就不再补, 纯快速点按(没到防抖期之外的连发节奏)给一步
          if (!b.repeated && held >= BTN_DEBOUNCE_MS) b.pending = BTN_SHORT;
          b.repeated     = false;
          b.lastRepeatMs = 0;
        } else {
          // 落在 [debounce, long) 区间内才算短按
          if (!b.longDone && held >= BTN_DEBOUNCE_MS && held < BTN_LONG_MS)
            b.pending = BTN_SHORT;
        }
      }
    }
  }
}

bool hasPending(BtnId id) { return sBtn[id].pending != BTN_NONE; }

BtnEvent takePending(BtnId id) {
  BtnEvent e   = sBtn[id].pending;
  sBtn[id].pending = BTN_NONE;
  return e;
}

}  // namespace Controls
