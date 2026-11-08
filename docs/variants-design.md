# GDI / QML 分支与分发设计

[English](variants-design.en.md)

## 关键裁决

一个仓库的 `gdi` / `qml` 两支使用同一插件 ID `ra9fael.binarylab`。安装另一版就是替换当前版；名称、标签和包名区分类型。用户于 2026-11-09 批准此方案。

## 依据

源码检查：宿主 `PluginSettings::installFrom` 按 ID 询问替换，`updates::replace` 停止旧进程，`install::place` 整体更换安装目录并支持复制失败回滚。数据目录在安装目录外。QML 目前仅 bits 窗口完成，其他页面是占位页。

## 架构与核心逻辑

GDI 使用静态 CRT 的单 EXE；QML 使用静态 CRT 启动器、`BinaryLabApp.dll` 和宿主 Qt 6.12.0（同主次版本、最低该补丁）。构建固定 WinShun 控件源码提交。计算核心保持纯 C++，公共修复独立提交后同步到另一支。

`release.json` 声明 variant、prerelease 和 QML 构建依赖；`plugin.json` 是版本和运行声明的权威。GDI 标签 `gdi-vX.Y.Z`，QML 标签 `qml-vX.Y.Z`。安装包 `<id>-<variant>-<version>.wsplugin`。

## 交互与图标

名称明确标注 GDI / QML，QML 窗口注明 Preview。同源图标用于 manifest、EXE、窗口和标题栏；ICO 八尺寸，Qt 嵌入对应 PNG。命令 ID、关键字和动作 ID 保持不变。

## 权限与数据

不增加权限。沿用同 ID 的配置、快捷键和独立数据目录。历史 JSONL 使用共同格式；未来破坏性迁移须先备份。开发目录链接不参与替换安装。手动选择 GDI / QML 更新，未来的自动更新必须识别类型。

## 失败矩阵

| 条件 | 处理 |
|---|---|
| 标签类型或版本不符 | 写包前报错 |
| QML DLL / 图标或双语版本说明缺失 | 写包前报错 |
| GDI 目录存在旧 QML DLL | 白名单运行文件排除它 |
| 安装目标是开发链接 | 宿主拒绝覆盖 |
| 替换复制失败 | 宿主回滚旧目录 |
| QML 宿主 Qt 不匹配 | 宿主预检拒绝启动 |
| QML 页面未完成 | 预览版说明，不发布为稳定版 |

## 交付清单

两支源码、统一打包脚本与流水线、双语文档、同源图标、两个安装包与 SHA256。QML 标为预览，不成为 GitHub latest。GitHub CLI 操作串行执行，避免高频轮询。

## 测试计划

- L0：manifest、标签、运行文件、图标资源、确定性包和校验值。
- L1：已有计算逻辑测试；QML 表达式规格测试。
- L2：打包负例与协议初始化、查询、动作、退出。
- L3：真实宿主加载两包；调用宿主生产安装实现验证双向替换、降级与数据保留。
- L4：独立窗口截图、大小图标和 DPI；未自动覆盖项目明确记录。
- L5：干净提交构建、CI 与 Release 附件核对。

## 诚实笔记与交付报告

设计依据是源码检查，不将其冒充实际安装验证。最终实测与未测边界记录到 `docs/variant-validation.md`。QML 其他工作台页面的完整移植不属于本次预览分发的完成条件。
