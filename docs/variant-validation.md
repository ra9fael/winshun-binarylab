# 双版本分发验证 · 2026-11-09

[English](variant-validation.en.md)

## 问题是什么

GDI 在 main，QML 在 qml，二者共用无类型标识的包名。QML 缺 DLL 也能打包，CI 没配置 Qt / 宿主源码依赖，EXE / Qt 窗口图标没有接齐。

## 为什么

发布流程源自单 EXE 模板；图标只设置了 QML 自绘标题栏，没有设置启动器资源与 Qt 应用图标。宿主已有同 ID 替换能力，不需要增加冲突字段。

## 改了什么

改为 gdi / qml 两支，标签、包名和显示名称区分类型；同 ID 互斥安装。独立构建目录、统一资源图标、打包前校验、固定 QML 依赖、双语说明及分支 CI。QML 明确保持预览状态。

## 实测

- MSVC 19.51、Windows x64 Release；GDI CTest 2/2，QML CTest 3/3。两支都包含 6 项打包回归测试；QML 另外运行表达式规格测试。
- 打包回归覆盖缺 DLL、错误类型标签、旧格式标签、版本不匹配、缺英文说明、GDI 排除残留 DLL，以及确定性包 / SHA256。
- 通过生产 `install::inspect/place` 对真实包执行 GDI 全新安装 → QML → QML 同版覆盖 → GDI 降级。每一步只有一个插件目录，QML DLL 仅在 QML 存在；history.jsonl 和独立设置/快捷键文件样本逐字节不变。旧 EXE 禁止删除共享时，替换失败且 GDI 完整保留。见 [安装输出](variant-install.txt)。
- 真实宿主 Host / Runner（wsbench）运行包内程序，分别查询 bits / bytes / struct（两版均返回 6 / 5 / 5 行），并对 `bits f32 1.5` 触发 openEditor；LOW 完整性、无提权、job 内运行，正常退出。
- 75 项窗口检查通过，见 [机器结果](variant-checks.json)。两版 WM_GETICON 返回大小图标，与 app.ico 像素一致（Qt 预乘 alpha 允许 1 级差异）。GDI 模拟 96 / 120 / 144 / 192 DPI 消息，大小与像素正确。
- 已检查真实窗口截图：[GDI](gdi-window.png)、[QML](qml-window.png)。标题栏正确显示图标与版本类型。
- 只启动和结束本次测试进程。安装测试临时配置自动回收；不改用户正在运行的插件或配置。构建、日志在工作台 `build/binarylab-variants-20261109/`，包在 `dist/binarylab-variants-20261109/`。

## 没测的

- QML 四次运行设置了 QT_SCALE_FACTOR，但原生图标均在实际 96 DPI 桌面验证；不能据此宣称原生跨 DPI 已验证。真实跨显示器拖动、任务栏与 Alt+Tab 视觉检查未自动覆盖。
- 未在 GUI 中逐次点击安装确认或恢复快捷键；安装目录替换使用宿主生产实现，设置文件保留使用独立样本。运行中替换的完整 GUI 流程未自动覆盖，仅测试文件锁拒绝覆盖；停止旧进程依据宿主源码。
- QML bytes / struct / 设置 / 帮助窗口页仍在移植。未重测所有计算边界、全部 Windows 版本与硬件。


## 公开发布包复核

两条标签的 GitHub 构建、测试、发布全部通过。GDI `gdi-v0.3.2` 为稳定版和 latest；QML `qml-v0.4.0` 为预览版。默认分支为 `gdi`。首次 QML CI 因 aqt 3.3.0 使用旧版 Qt 下载目录而失败，固定到支持 Windows 编译器独立目录的上游安装工具提交后通过。

已下载公开附件，将 SHA256 文件、实算结果和 GitHub 资产 digest 三方核对。检查包内 ID / 版本、DLL 清单、EXE 文件版本、命名图标资源及全部八个尺寸。对下载包重跑宿主生产安装实现的双向替换及 **48 项窗口检查**。见 [包与源码提交](published-packages.json)、[安装结果](published-install.txt)、[窗口结果](published-window-checks.json)。

未替换已发布附件。保留正式包与证据后清理测试部署副本和构建产物；原有工作目录及先前已有二进制文件保持原样。
