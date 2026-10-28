# ESP8266Audio 自带的 TinySoundFont 整个被包在
#   tsf.h:53  #if !defined(ESP32)
# 里 —— 作者为了绕开 Xtensa 后端的编译器 bug, 在 ESP32 平台(含 C3, 因为 Arduino-ESP32
# 对所有 C3 也会定义 ESP32)把 tsf.h 的内容整个挖空了。
# 结果 AudioGeneratorMIDI.h 里 `tsf *g_tsf;` / `struct tsf_stream buffer;` 拿不到类型定义,
# 编译到 ESP8266Audio 库自己的源文件时就必定报:
#   'tsf' does not name a type
#   field 'buffer' has incomplete type 'tsf_stream'
#
# 我们只用 AudioGeneratorMP3a + AudioOutputI2S, 根本不需要 MIDI。
# 这里把这个源文件从构建里摘掉, 既绕开上游 bug, 又省下一段 flash。
# 用 AddBuildMiddleware 而不是改 libdeps 里的库源码 —— 后者会被 pio 重新下载覆盖。

Import("env")

_SKIP = (
    "AudioGeneratorMIDI.cpp",
)


def _skip_broken_midi(node):
    path = node.get_path().replace("\\", "/")
    for name in _SKIP:
        if path.endswith("/" + name):
            return None  # 返回 None -> SCons 不生成这个编译目标
    return node


env.AddBuildMiddleware(_skip_broken_midi, "*")
