# NRadio — ESP32-C3 网络收音机

用一块 **ESP32-C3 SuperMini** + 一片 **MAX98357A** 搭的联网收音机：
WiFi 拉取 HTTP MP3 流 → libhelix 软解码 → I2S 出声。短按切台，旋转编码器调音量，长按静音。

全部资源跑在单核 240MHz 的 C3 上：没有 PSRAM，没有硬件解码器，没有外部 DAC。

```
RAM:   32.3% (105,956 / 327,680 B)
Flash: 58.9% (771,426 / 1,310,720 B)
```

## 功能

- 12 个电台（`src/config.h` 里一行一个，随便加）
- GPIO0 按键：**短按**切下一台 / **长按**静音（中断驱动，播放卡顿时也不丢按键）
- 旋转编码器：调音量（同样中断驱动，正交解码）
- 串口命令行：`n/p` 切台、`0-9` 选台、`+/-` 音量、`m` 静音、`i` 信息、`l` 电台列表、`r` 重连、`b` 纯 TCP 吞吐基准
- 断线指数退避重连（2s→30s 封顶，稳定 30s 后重置），缓冲枯竭时优先"软重同步"省去重新握手
- 串口健康报告（`[stat]`）：缓冲水位、实测 I2S 输出频率、实测消耗码率、单圈最坏耗时

## 硬件接线

| MAX98357A | ESP32-C3 | 说明 |
|---|---|---|
| BCLK | GPIO5 | |
| LRC  | GPIO7 | |
| DIN  | GPIO10 | |
| GND / VIN | GND / 5V | |
| SD   | VIN 或悬空按模块默认 | 见 `config.h` 的 `AMP_SD_PIN` |

| 外设 | ESP32-C3 | 说明 |
|---|---|---|
| 按键 | GPIO0 ↔ GND | 内部上拉，无需电阻。**上电瞬间必须松开**（strapping 脚） |
| 编码器 A | GPIO4 | 内部上拉 |
| 编码器 B | GPIO6 | 内部上拉 |
| 编码器公共脚 | GND | |

**GPIO2 是牺牲脚**：库会把 I2S MCLK 路由到它上面，会输出 ~11.3MHz 时钟，**悬空别接任何东西**。原因见下文"踩过的坑"。

## 编译烧录

[PlatformIO](https://platformio.org/)，插上板子（板载原生 USB，无需驱动）：

```bash
pio run -t upload          # 编译 + 烧录
pio device monitor         # 看串口, 115200
```

WiFi 和电台都改 `src/config.h`。电台必须是 **http** 明文流 —— C3 没有 TLS，https 连不上。

## 踩过的坑（都写进了代码注释，这里汇总）

这个项目里大部分代码量不是"功能"，而是绕开各种坑。列举几个最疼的：

1. **lwIP 的 TCP 接收窗口默认只有 5760 字节**，且没开窗口扩大。跨洋电台 RTT 382ms 时
   吞吐上限 = 5760/0.382 = 15.1 kB/s，跑不动 128kbps（需要 16.0）。`connect()` 后
   `setsockopt(SO_RCVBUF, 32768)` 顶上去，这是"隔一阵卡一下"的头号根因。
2. **`AudioFileSourceICYStream` 有 `goto retry` 死循环**：`setReuse(true)` 会让
   `http.end()` 变成空操作，一次 500ms 网络空档就永久卡死。所以自己写了 `NetStream`
   （HTTP + 静态环形缓冲）。
3. **`AudioFileSource::read()` 的契约是"要么填满 len，要么返回 0"**。返回半帧数据时
   libhelix 会顺着残缺帧头越界写 → `bitstream.c` Store access fault 直接死机。
4. **ESP8266Audio 的 `SetPinout()` 会把 MCLK 路由到 GPIO0**（`mclkPin` 默认值恰好是 0），
   把按键的 pad 抢走，症状是"没人碰按钮它自己静音/切台"。且没法传 -1 关掉 ——
   `mclkPin` 是 `uint8_t`，-1 变 255 后 `i2s_set_pin()` 在校验阶段就返回，
   连 BCLK/LRC/DIN 都不路由了。只能给一个真实的牺牲脚（GPIO2）。
5. **串口 `Serial.println()` 会阻塞几百毫秒**（`HWCDC::write` 里的信号量 + 重试循环）。
   播放中打日志 = 主动制造断音。`logStatus()` 里用 `availableForWrite()` 做了非阻塞守卫：
   空间不够就丢日志，绝不挡音频。
6. **按键/编码器必须中断驱动**：播放时每圈 loop 要 20~100ms 解 MP3，轮询会整个错过
   几十毫秒的短按键。
7. **ESP8266Audio 1.9.9 的 MIDI 生成器在 ESP32 上必编译失败**（上游把 TinySoundFont
   被 `#if !defined(ESP32)` 挖空了但类型还在用）。`exclude_midi.py` 用构建中间件
   把这一个文件摘出构建，不修改库源码。

更完整的排查手册（含卡顿判读对照表）在开发笔记里，核心思路：

> `[stat]` 里的 `out`(实测输出频率) 掉到标称采样率以下、且
> `out/44100 ≈ 实测吞吐/所需码率` 时，是**持续带宽缺口**，不是软件问题 ——
> 再大的缓冲也只能推迟发作。

## 目录结构

```
src/config.h        WiFi、电台、引脚、缓冲参数 —— 你唯一需要改的文件
src/main.cpp        状态机(ST_CONNECT/PREFILL/PLAY/BACKOFF) + 串口命令 + 健康统计
src/net_stream.*    自写 HTTP 流源: 重定向解析 + SO_RCVBUF + 64KB 环形缓冲
src/controls.*      按键/编码器中断驱动 + 防抖 + 长按判定
tools/              开发期用的串口抓取/电台探测/码率基准脚本
exclude_midi.py     构建中间件: 摘掉上游编译不过的 MIDI 源文件
```

## License

[MIT](LICENSE)
