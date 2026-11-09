#pragma once
#include <Arduino.h>

// 按键编号。一颗按键对应一个事件队列, 互不干扰。
enum BtnId {
  BTN_MAIN = 0,     // GPIO0: 短按切台 / 长按静音
  BTN_VOL_UP,       // GPIO4: 音量 +
  BTN_VOL_DOWN,     // GPIO6: 音量 -
  BTN_COUNT
};

// 按键事件。一次 press/release 只产生一个事件, 存在对应队列里直到被 takePending() 取走。
enum BtnEvent { BTN_NONE = 0, BTN_SHORT, BTN_LONG };

namespace Controls {

  void      begin();
  void      poll();                              // 推进按键状态机。可在任意阻塞循环里反复调用。
  bool      hasPending(BtnId id = BTN_MAIN);     // 有尚未取走的事件?
  BtnEvent  takePending(BtnId id = BTN_MAIN);    // 取走并清空

}
