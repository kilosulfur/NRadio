# NRadio — ESP32-C3 网络收音机

用一块 **ESP32-C3 SuperMini** + 一片 **MAX98357A** 搭的联网收音机：
WiFi 拉取 HTTP MP3 流 → libhelix 软解码 → I2S 出声。三颗按键控制切台 / 音量 / 静音。

全部资源跑在单核 160MHz 的 C3 上：没有 PSRAM，没有硬件解码器，没有外部 DAC。

```
RAM:   32.4% (106,036 / 327,680 B)
Flash: 58.9% (771,504 / 1,310,720 B)
```

## 功能

- 11 个电台（`src/config.h` 里一行一个，随便加）
- **三颗按键**（全部中断驱动，播放卡顿时也不丢按键）：

  | 按键 | 行为 |
  |---|---|
  | GPIO0 短按 | 切下一台 |
  | GPIO0 长按 ≥1.2s | 静音开关 |
  | GPIO4 音量 + | 点按 +0.10；**按住不放每 250ms 连发** |
  | GPIO6 音量 − | 点按 −0.10；按住连发 |

- 串口命令行：`n/p` 切台、`0-9` 选台、`+/-` 音量、`m` 静音、`i` 信息、`l` 电台列表、`r` 重连、`b` 纯 TCP 吞吐基准
- 断线指数退避重连（2s→30s 封顶，稳定 30s 后重置），缓冲枯竭时优先"软重同步"省去重新握手
- 串口健康报告（`[stat]`）：缓冲水位、实测 I2S 输出频率、实测消耗码率、单圈最坏耗时及各段归因

## 硬件接线

| MAX98357A | ESP32-C3 | 说明 |
|---|---|---|
| BCLK | GPIO5 | |
| LRC  | GPIO7 | |
| DIN  | GPIO10 | |
| GND / VIN | GND / 5V | |
| SD   | VIN 或悬空按模块默认 | 见 `config.h` 的 `AMP_SD_PIN`；**接 GND 会整机静音** |

| 外设 | ESP32-C3 | 说明 |
|---|---|---|
| 主键 | 一脚 GPIO0，另一脚 GND | 内部上拉，无需电阻。**上电瞬间必须松开**（strapping 脚） |
| 音量 + | 一脚 GPIO4，另一脚 GND | 内部上拉 |
| 音量 − | 一脚 GPIO6，另一脚 GND | 内部上拉 |

**GPIO2 是牺牲脚**：库会把 I2S MCLK 路由到它上面，会输出 ~11.3MHz 时钟，**悬空别接任何东西**。原因见下文"踩过的坑"。

## 从零跑起来（换电脑 / 在家调试）

```bash
git clone https://github.com/kilosulfur/NRadio.git
cd NRadio
```

1. **装 PlatformIO**（VS Code 插件或 `pip install platformio`），板子插 USB 即可，原生 USB 无需装驱动。
2. **改 `src/config.h` 里的 WiFi** —— 仓库里是占位符 `your-wifi-ssid`，必须换成自己的，否则开机每个 AP 白等 8 秒然后一直重连。
   电台必须是 **http** 明文流 —— C3 没有 TLS，https 连不上。
3. 编译烧录：`pio run -t upload`。`platformio.ini` **不写死串口号**，PlatformIO 会自动找板子；
   认不出来或同时插了多块板时用 `pio run -t upload --upload-port COM7` 覆盖。
4. 看日志：`pio device monitor`（115200）；或者用仓库自带的工具，它按 `VID:PID=303A:1001` 自动找串口、每行带时间戳、还能定时发命令：

   ```bash
   pip install pyserial
   python tools/serial_capture.py 60 115200 --cmd 20:n --cmd 40:i
   ```

   （Windows PowerShell 里 `\n` 写作 `--cmd '20:n\n'`。）

> 没接功放也能调试：解码器和 I2S 照常跑，`[stat]` / `i` 一切正常，只是没声音。

## 日志怎么读

正常播放时每 15 秒一行：

```
[stat] win=15s loops=130422 | max loop=5ms dec=5ms pump=2ms | stalls=0
[stat] buf=65353/65536 low=60652 | 7.0 KB/s | starve=0 eof=0 | heap=75072
[stat] out=23996 Hz | mp3=56 kbps | consumed=105000
```

- `out` = 实测 I2S 输出频率，应 ≈ 电台采样率；`mp3` = 实测消耗码率，应 ≈ 电台 `icy-br`
- `buf` 贴满、`low` 很高、`starve=0`、`stalls=0` = 健康
- **持续带宽缺口的判据**：`out/标称采样率 ≈ 实测吞吐/所需码率`。例如 128kbps 的台只收到
  14.4 kB/s，`out` 就从 44100 掉到 ~41000。这是链路喂不饱，**再大的缓冲也只能推迟发作**，
  换低码率台或换网络才是解（实测某条移动热点只有 6.2 kB/s，BBC 56kbps 完全正常）。
- `max loop` 里出现几秒的大数值，多半是软重同步的 `prefetch()` / `b` 基准在阻塞主循环，
  不是解码卡顿 —— 看 `segments: log/misc/tail` 是否都接近 0 就能区分。

## 完全没声音时

按这个顺序查（曾经踩过一整轮）：

1. MAX98357A 的 **SD 脚**是否悬空或接 VIN；碰 GND = 静音
2. BCLK/LRC/DIN 是否错插到 GPIO4/6（音量键）上，把时钟短路到地
3. 喇叭、VIN(5V)、GND 是否牢靠
4. 串口若一切正常（`i` 显示未静音、`[stat]` 有数据消耗），那就是纯硬件侧问题

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
6. **按键必须中断驱动**：播放时每圈 loop 要 20~100ms 解 MP3，轮询会整个错过
   几十毫秒的短按键。这里的三颗键各自挂 CHANGE 中断，用 `esp_timer_get_time()`
   计时（ISR 里不能调 `millis()`）。
7. **C3 原生 USB 的 `Serial` 需要两个宏成对出现**：`ARDUINO_USB_MODE=1` +
   `ARDUINO_USB_CDC_ON_BOOT=1`，只写后者会编译报 `Serial was not declared`。
8. **WiFi modem sleep 必须关**（`WiFi.setSleep(false)`）：默认休眠唤醒时会有几十毫秒
   接收空档，直接把音频 DMA 饿空。
9. **ESP8266Audio 1.9.9 的 MIDI 生成器在 ESP32 上必编译失败**（上游把 TinySoundFont
   被 `#if !defined(ESP32)` 挖空了但类型还在用）。`exclude_midi.py` 用构建中间件
   把这一个文件摘出构建，不修改库源码。

## 目录结构

```
src/config.h        WiFi、电台、引脚、缓冲参数 —— 你唯一需要改的文件
src/main.cpp        状态机(ST_CONNECT/PREFILL/PLAY/BACKOFF) + 串口命令 + 健康统计
src/net_stream.*    自写 HTTP 流源: 重定向解析 + SO_RCVBUF + 64KB 环形缓冲
src/controls.*      三按键中断驱动 + 防抖 + 长短按判定 + 音量键连发
tools/              串口抓取(自动找口) / 电台探测 / 码率基准脚本
exclude_midi.py     构建中间件: 摘掉上游编译不过的 MIDI 源文件
```

## License

[MIT](LICENSE)
