# 发布检查

[返回首页](../README.md)

## 内容检查

- 检查源码、Windows 程序和三板工厂固件的版本一致性。
- 检查使用说明、烧录地址、最低硬件配置及下载链接。
- 运行依赖、文档链接、构建和相关设备功能检查。
- 核对许可证与素材授权，确认 dl_fft 对应版本的完整许可文本。
- 排除设备备份、NVS、账户凭据、私人日志和编译缓存。
- 校验 ZIP 和 SHA-256 清单。

## 本地校验

在 ESP-IDF 5.5.6 环境运行：

```sh
python scripts/verify_portability.py
python -m unittest discover -s tests -p "test_*.py"
python scripts/build.py all
python scripts/check_release.py --builds
```

涉及硬件的修改需在对应设备验证。源码 ZIP、Windows 上位机 ZIP 和预编译固件 ZIP 分别作为发布附件；工厂固件操作见[免编译烧录](PREBUILT_FLASH.md)。

## Git 收录

收录源码、VS Code 配置及依赖锁文件，排除下载缓存和构建产物：

```sh
git init -b main
python scripts/stage_source.py
git diff --cached --stat
```

脚本只暂存按源码包规则筛选的文件，不自动提交或上传。`.gitattributes` 保持第三方文件的原始字节；不要无差别强制添加构建目录和设备数据。
