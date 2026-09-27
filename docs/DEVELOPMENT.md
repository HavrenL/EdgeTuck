# 开发与验证

## 构建

使用 Windows x64、MSVC C++20、Windows 11 SDK 和 CMake 3.24+。默认构建方式见 [README](../README.md)。`build.ps1` 会构建、执行 CTest，并生成便携目录。

开发工具启动进程时可能继承与真实桌面不同的配置视图。正常验收使用：

```powershell
.\build.ps1 -Run
# 已有构建时：
.\build\Release\EdgeTuckExplorerLaunch.exe (Resolve-Path .\dist\EdgeTuck.exe).Path --tray
```

请向启动助手传入可执行文件的绝对路径，或在参数中使用 `(Resolve-Path .\dist\EdgeTuck.exe).Path`，避免依赖 Explorer 的工作目录。

## 自动测试

```powershell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

测试涵盖配置与布局、排序、文件操作、目录身份与迁移、菜单辅助进程、缩略图、GPU 材质及原生窗口行为。窗口/GPU 测试需要交互式 Windows 桌面，可能显示测试背景；请先退出日常轻屉实例。

文件测试只操作自建的临时数据；配置恢复、迁移或删除逻辑不以用户文件作为测试样本。菜单辅助进程进入命令执行阶段后，不按进程名批量结束。

`tests/*_probe.*` 为手动诊断工具，不是全部自动执行的 CTest 项。使用前阅读其参数和前置条件；部分 UI 探针面向常规实例，会触发真实设置操作。不要将这些工具批量运行当作无副作用检查。

## 提交范围

源码、资源、构建脚本、测试、当前文档及公开演示素材纳入发布。`build/`、`dist/`、`artifacts/`、本机日志、配置副本、第三方研究下载及历史私人排查记录不纳入源码仓库。

`tools/export-source.ps1` 按明确文件清单生成可审阅的源码目录和 SHA-256 清单；该脚本不会创建 GitHub 仓库或提交文件。

## 修改约定

- 普通文件夹和内容的独立可用性优先，冲突时保留数据。
- 系统菜单扩展只在菜单辅助进程中加载。
- 新增长驻刷新机制前说明原因，测量真实成本。
- 报告区分编译、自动测试、目视检查与性能测量。
- 提交截图和日志前检查其中的路径、文件名和个人信息。

本项目采用 [MIT 许可证](../LICENSE)。发布源码与便携版时保留 `LICENSE`；引入第三方代码时，核对并保留其署名与许可要求。
