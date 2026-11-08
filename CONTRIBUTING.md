# 构建与发布 / Contributing

[English](CONTRIBUTING.en.md)

本仓库使用 `gdi`（默认）和 `qml` 两个长期分支。公共计算逻辑和协议修改单独提交，再同步到另一支；界面修改留在对应分支。用户可见文档中英文同步。

## 构建

Windows x64、CMake、MSVC、Python 3.10+。QML 另需 Qt 6.12.0 和 `release.json` 指定的 WinShun 源码提交（仓库 `ra9fael/WinShun`）。

```powershell
cmake -S . -B build/release -A x64 -DBINARYLAB_OUTPUT_DIR="$PWD/build/bin"
cmake --build build/release --config Release
ctest --test-dir build/release -C Release --output-on-failure
python tools/package.py --build-dir build/bin --tag qml-v0.4.0
```

QML 配置额外传入 `-DCMAKE_PREFIX_PATH=<Qt kit>` 和 `-DWINSHUN_DIR=<固定提交源码目录>`。
独立输出目录不会改动源码目录旁正在运行的 EXE/DLL。不得为构建停止用户进程。

## 发布

1. 更新 `plugin.json.version` 和 `binarylab.rc`；`release.json` 声明类型与预览状态，QML 还固定构建依赖。版本保留纯数字。
2. 更新双语 README、CHANGELOG、相关截图和验证证据。验证全新安装、双向替换、数据保留、查询与窗口图标；QML 未完成的页面明确列出。
3. 使用干净提交构建，运行相关 CTest 和真实宿主验证。通过后推送分支，等待 CI 成功。
4. 创建对应分支的 `<variant>-vX.Y.Z` 标签并推送；CI 校验身份，构建、测试并发布包和校验值。QML 预览版不会成为 latest。
5. 串行核对 GitHub Release 附件和校验值；不替换已发布资产，修复时升补丁版本。

两版 ID 必须保持 `ra9fael.binarylab`。无 Qt 依赖的 GDI 不打包 DLL；QML 缺少 `BinaryLabApp.dll` 时打包必须失败。历史 JSONL 格式保持兼容；破坏性变更须先备份并迁移。开发目录链接不能通过安装包替换，验证时使用隔离配置。

提交使用 `type(scope): subject`。只提交本次改动，不提交 build / dist 或本机路径和凭据。SDK `winshun.h` 来自宿主，应单独更新。
