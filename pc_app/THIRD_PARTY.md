# 上位机第三方依赖

自有代码采用仓库根目录的非商业许可，以下第三方依赖保留原许可。

| 组件 | 版本 | 许可文本 |
| --- | --- | --- |
| CPython / 随附运行库 | 3.13 | [Python](licenses/Python-LICENSE.txt)，含所列第三方声明 |
| Tcl / Tk | 8.6 | [Tcl](licenses/Tcl-license.terms) · [Tk](licenses/Tk-license.terms) |
| Pillow | 12.2.0 | [Pillow](licenses/Pillow-LICENSE.txt)，含所列第三方声明 |
| pyserial | 3.5 | [BSD](licenses/pyserial-LICENSE.txt) |
| PyWinRT 运行时及 Windows API 投影 | 3.2.1 | [MIT](licenses/PyWinRT-LICENSE.txt) |
| typing_extensions（间接依赖） | 4.16.0 | [PSF](licenses/typing_extensions-LICENSE.txt) |
| PyInstaller 引导程序及构建工具 | 6.16.0 | [GPL 与引导程序例外](licenses/PyInstaller-COPYING.txt) |

直接依赖见 `requirements.txt`，打包工具见 `requirements-build.txt`。Windows 系统 API 和随 Python 提供的 Microsoft 运行库仍适用各自条款。

许可来源：Python、Tk、Pillow、typing_extensions 和 PyInstaller 文本取自对应版本的安装包；pyserial 取自 [v3.5](https://github.com/pyserial/pyserial/blob/v3.5/LICENSE.txt)，PyWinRT 取自 [v3.2.1](https://github.com/pywinrt/pywinrt/blob/v3.2.1/LICENSE)，Tcl 取自 [8.6.15](https://github.com/tcltk/tcl/blob/core-8-6-15/license.terms)。
