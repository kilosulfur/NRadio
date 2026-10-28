#pragma once
#include <Arduino.h>

// 按键事件。一次 press/release 只产生一个事件, 存在 Controls 内部直到被 takePending() 取走。
enum BtnEvent { BTN_NONE = 0, BTN_SHORT, BTN_LONG };

namespace Controls {

  void      begin();
  void      poll();               // 推进按键状态机。可在任意阻塞循环里反复调用。
  bool      hasPending();         // 有尚未取走的按键事件?
  BtnEvent  takePending();        // 取走并清空
  int       takeEncoderDelta();   // 取走编码器净增量并清零(正 = 顺时针)

}
