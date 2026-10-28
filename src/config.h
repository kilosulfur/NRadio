#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// I2S 音频输出 (MAX98357A)
// ---------------------------------------------------------------------------
#define I2S_BCLK        5
#define I2S_LRC         7
#define I2S_DIN         10
// MCLK 的"牺牲脚"。原因见 main.cpp setup():
// ESP8266Audio 的 AudioOutputI2S 里 mclkPin 是 uint8_t, 构造函数默认值恰好是 0,
// 也就是 GPIO0 —— SetPinout() 会把这块 pad 交给 I2S 驱动当 MCLK 输出 >_<。
// 而这个库又没法表达"不用 MCLK"(传 -1 会变成 255, i2s_set_pin() 直接报错返回,
// 还会在路由 BCLK/LRC/DIN 之前就退出 -> 完全没声音)。
// 所以只能给它一个真实的 GPIO, 代价是这个脚会输出 ~11.3MHz 的时钟。
// MAX98357A 本身不需要 MCLK, 挑一个没别的事干的脚让它占着就行 —— 悬空不要接东西。
#define I2S_MCLK        2

// MAX98357A 的 SD 脚: 高电平工作 / 低电平关断, 绝不能悬空。
// 如果你的模块已经把 SD 接到 VIN(常开), 保持 -1 即可(代码不接管)。
// 想让代码控制关断以消除待机噪声, 就把它改成实际 GPIO 编号。
#define AMP_SD_PIN      (-1)

// 板载 LED (SuperMini 是 GPIO8, 有源低电平点亮)
#define LED_PIN         8
#define LED_ON          LOW
#define LED_OFF         HIGH

// ---------------------------------------------------------------------------
// WiFi 凭据 (按顺序尝试) —— 改成你自己的
// ---------------------------------------------------------------------------
struct WiFiCredential {
  const char *ssid;
  const char *pass;
};

const WiFiCredential wifiList[] = {
  { "your-wifi-ssid",  "your-wifi-password" },
  { "another-ssid",    "another-password"   },
};
const size_t wifiCount = sizeof(wifiList) / sizeof(wifiList[0]);

// ---------------------------------------------------------------------------
// 电台列表 (全部为可公开访问的 HTTP MP3 流; ESP32-C3 无 TLS, 不要填 https)
// ---------------------------------------------------------------------------
struct RadioStation {
  const char *name;
  const char *url;
  const char *icon;
};

const RadioStation radioStations[] = {
  { "Radio Paradise",   "http://stream.radioparadise.com/mp3-128",                 "rp.bmp"         },
  { "BBC",              "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service",  "bbc.bmp"        },
  { "HIT FM",           "http://online.hitfm.ua/HitFM",                            "hitfm.bmp"      },
  { "MANGORADIO",       "http://mangoradio.stream.laut.fm/mangoradio",             "mangoradio.bmp" },
  { "ZZ959",            "http://lhttp.qtfm.cn/live/5021381/64k.mp3",               "zz959.bmp"      },
  { "Groove Salad",     "http://ice1.somafm.com/groovesalad-128-mp3",              "groove.bmp"     },
  { "Radio Swiss Jazz", "http://stream.srg-ssr.ch/m/rsj/mp3_128",                  "rsj.bmp"        },
  { "GD Music",         "http://lhttp.qtfm.cn/live/1260/64k.mp3",                  "gd.bmp"         },
  { "Lush",             "http://ice1.somafm.com/lush-128-mp3",                     "lush.bmp"       },
  { "ASIA Music",       "http://lhttp.qingting.fm/live/4581/64k.mp3",              "am.bmp"         },
  { "ASIA Music HD",    "http://lhttp.qtfm.cn/live/15318341/64k.mp3",              "amhd.bmp"       },
  { "FUN RADIO",        "http://solid9.streamupsolutions.com/proxy/wpgqrttp/stream", "fr.bmp"       },
};
const size_t stationCount = sizeof(radioStations) / sizeof(radioStations[0]);

// ---------------------------------------------------------------------------
// 人机输入
// ---------------------------------------------------------------------------
// 按键: GPIO0 <-> GND。按下为低电平, 用芯片内部上拉, 不需要外部电阻。
// 注意 GPIO0 是 strapping 脚 —— 上电/复位瞬间必须保持松开(高电平),
// 按着不放再上电会进 USB 下载模式, 表现得像"板子没启动"。
#define BTN_PIN         0
#define BTN_ACTIVE      LOW
#define BTN_DEBOUNCE_MS 30      // 短于此视作抖动, 忽略
#define BTN_LONG_MS     1200    // 按住超过这么久算长按

// 旋转编码器(鼠标滚轮那种): A/B 两相 + 中间公共脚。
// 公共脚接 GND, A/B 分别接下面两脚, 同理靠内部上拉。
#define ENC_A_PIN       4
#define ENC_B_PIN       6

// 音量: ESP8266Audio 的 SetGain() 是个乘法增益, 0.0 ~ 2.0 够用, 再大必削顶。
#define VOL_MIN         0.0f
#define VOL_MAX         2.0f
#define VOL_STEP        0.05f   // 编码器每一格的增减量

// ---------------------------------------------------------------------------
// 播放参数
// ---------------------------------------------------------------------------
// 抗卡顿的两道防线, 作用层次不同, 别混为一谈:
//
// 1) RING_SIZE —— 挡"网络抖动"。TCP 来一阵慢一阵, 环形缓冲就是蓄水池,
//    水深 = 能扛住的网络空档时长。128kbps = 16KB/s, 64KB ≈ 4 秒。
//    实测稳态时缓冲一直是满的, 所以它只在真出状况时才起作用 —— 但一旦需要,
//    24KB(1.5s) 和 64KB(4s) 的差别就是"卡一下"和"完全听不出来"。
//
// 2) DMA_BUF_COUNT —— 挡"CPU 调度抖动", 这一条才是卡顿的主因。
//    AudioOutputI2S 的 dma_buf_len 固定 128 帧, 每个缓冲 512B,
//    ESP32-C3 是单核, WiFi 任务(优先级 23)一来就会抢占播放任务(优先级 1),
//    抢占期间没人给 DMA 喂数据, DMA 里剩多少就撑多少毫秒。
//      8  个 -> 23ms   (库默认, 必卡)
//      32 个 -> 93ms
//      96 个 -> 278ms  (本项目取值)
//    代价只是切台时旧声音多响 0.28 秒, 听感上反而是"不爆音"。
#define RING_SIZE       65536   // 网络环形缓冲 (RAM)
#define DMA_BUF_COUNT   96      // I2S DMA 缓冲数, 96 * 128 帧 / 44100 ≈ 278ms

// 起播/重同步前要攒够的量。别设太大: 每次切台都要等这么久才出声。
#define PREFILL_TARGET  24576   // 128kbps 下约 1.5s
#define PREFILL_MIN     8192    // 或者: 已有这么多且 2.5s 不再增长, 也直接起播
#define PREFILL_TIMEOUT 12000

// 上面那道防线的时长(ms), 只用于统计里判定"这一圈循环超过 DMA 余量了吗"
#define DMA_WINDOW_MS   ((DMA_BUF_COUNT * 128 * 1000) / 44100)

// 软件微调 I2S 输出速率, 单位 ppm(百万分之一)。
// ESP32-C3 没有 APLL(soc_caps 里压根没有 APLL 相关的支持宏), 只能用 160MHz 主频分频,
// 44100Hz 除不尽 -> 实际输出频率和标称值有偏差。偏差意味着"消耗速度 != 实时速度",
// 环形缓冲会缓慢涨或跌, 直到撑满或饿死。用 [stat] 里的 out= 实测值反推需要多少:
//   偏差 ppm = (实测 - 标称) / 标称 * 1e6, 取负号填进来。
// 实测健康期稳定在 44058~44117Hz(±0.04%), 保持 0 即可。
#define I2S_RATE_TRIM_PPM  0

#define STAT_INTERVAL_MS 15000  // 播放中每 15s 打一行健康报告

// 【卡顿的根因之一】
// lwIP 编译时的 TCP 接收窗口只有 5760 字节(见 sdkconfig.h 的 CONFIG_LWIP_TCP_WND_DEFAULT),
// 而 TCP 的吞吐上限约等于 "窗口 / 往返时延"。Radio Paradise 服务器在美国, 实测 RTT=382ms:
//     5760 / 0.382 = 15.1 kB/s  <  128kbps 需要的 16.0 kB/s
// 差这 1 kB/s, 环形缓冲就永远填不满, 只会被慢慢抽干 —— 表现就是隔一阵卡一下。
// 电脑端测同一条流能到 47 kB/s(突发)/16 kB/s(稳态), 因为 PC 的窗口会自适应放大。
//
// 所以这里在 connect() 之后把 SO_RCVBUF 调大, 尽量把窗口顶上去。
// 上限受"窗口扩大因子"限制: 本工程用的 SDK 没开 CONFIG_LWIP_WND_SCALE,
// 窗口字段仍是可表达到 65535 的, 实测能顶到多少看 [net ] rcvbuf 那行日志。
#define TCP_RCVBUF_TARGET 32768

#define WATCHDOG_MS     12000   // 这么多毫秒没收到一个字节就判定假死
#define SOFT_RESYNC_MAX 3       // 连续软重同步超过这个数就降级为完整重连

#define VOL_DEFAULT     0.85f   // libhelix 输出电平偏高, 默认留点余量
