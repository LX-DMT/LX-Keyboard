# 贡献说明

提交修改前先阅读 [LICENSE](LICENSE)、[许可说明](docs/LICENSING.md) 和[架构](docs/ARCHITECTURE.md)。原创贡献按本项目非商业许可提供；第三方内容需保留原许可证并说明来源。

## 修改范围

一次修改围绕一个具体问题，注明涉及 keyboard、knob、receiver、共用协议或文档。协议编号保持兼容；改变 NVS 格式、分区、校准版本和硬件引脚时说明迁移方案。不要提交个人路径、固定设备地址、账号和配对密钥。

## 验证

在 ESP-IDF 5.5.6 环境运行：

```sh
python scripts/verify_portability.py
python -m unittest discover -s tests -p "test_*.py"
python scripts/build.py all
python scripts/check_release.py --builds
```

仅文档修改可检查链接和相关说明；硬件行为变化还需真实设备验证。记录模式、硬件、操作和结果，区分模拟、串口自检和人工输入验收，不将其中一个替代另一个。

## 第三方更新

本版依赖快照包含本地修改，例如 LVGL 色环质量参数。升级需要检查许可证、API、模型和内存占用，更新 `third_party.lock.json`，重新从干净目录构建。不要为绕过校验而直接删除锁文件。

## 问题反馈

使用 Bug 模板说明复现步骤和预期/实际结果。不要贴包含网络凭据、Token、设备备份或私人录音的附件。与个人网络环境相关的问题先完成近距离、同模式对照，再记录差异。
