# 开关机提示音

`startup.pcm` / `shutdown.pcm` 为 16 kHz、单声道、PCM16 little-endian，由 WAV 转换。`manifest.json` 记录来源名称和内容摘要；来源路径不作为构建依赖。

原始 WAV 未包含在本包中，正常构建直接嵌入 PCM。对外发布前需要确认原音频的再分发授权，见[许可说明](../../../../docs/LICENSING.md)。
