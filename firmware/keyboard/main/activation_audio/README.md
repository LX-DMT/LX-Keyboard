# 本地绑定验证码语音

`0.wav`～`9.wav` 为数字，`10.wav` 为提示语；语音由 Windows Microsoft Huihui Desktop 离线语音，PCM16 / 16 kHz / 单声道。`clips.bin` 和 `clips.h` 为打包资源，正常构建直接使用。

在仓库根目录执行 `python scripts/pack_activation_audio.py` 可从这些 WAV 重建打包文件，不需要重新调用 TTS。唤醒应答和普通回复由小智服务提供，这些本地资源只用于激活验证码播报。

素材的再分发条件须在发布前确认，详见[许可说明](../../../../docs/LICENSING.md)。离线 `chat code test` 的测试数字不是可用绑定码。
