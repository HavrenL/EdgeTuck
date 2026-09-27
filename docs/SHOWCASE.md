# 展示素材

README 的截图和动图来自可选工具 `EdgeTuckShowcase`。工具复用真实的抽屉、设置窗口、缩略图和玻璃合成代码，在单独的背景窗口上演示。它不是 HTML 或图片拼装的界面模型，也不代表完整多显示器桌面环境的测试。

## 内容来源

- 背景和示例图片由 `tools/showcase.cpp` 中的渐变、几何曲线生成。
- 示例目录为“工作”“常用”“灵感”，文件名和内容是固定的公开样本。
- 截图保留真实绘制结果；GIF 由连续屏幕帧缩放到 960×600、统一调色板后编码。捕获开销、编码和播放器会影响观感，不用它测量应用帧率。
- 背景在内存中完整绘制后一次提交，避免玻璃采到清屏或只绘制了一部分的画面。录制与写帧在单独线程运行，界面线程持续处理悬停和合成消息；鼠标轨迹按实际经过时间推进。

## 隔离范围

工具不调用正常启动流程，不加载或保存日常配置，不注册自启动、全局桌面事件钩子或“此电脑”入口，不停放桌面图标，不移动用户文件。仅创建自己的示例文件和临时窗口。

录制期间会在主显示器显示一个 1280×800 场景，并短暂移动鼠标演示悬停；结束后恢复鼠标位置。运行前请确保该区域可显示，并暂时停止鼠标操作。背景窗口被其他进程遮挡时，工具拒绝继续保存画面。输出目录必须不存在，不会覆盖旧素材。

## 重新生成

在 Windows 交互式桌面构建；主显示器至少需要容纳场景及折射取样留边（1360×900）：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DEDGETUCK_BUILD_SHOWCASE=ON
cmake --build build --config Release --target EdgeTuckShowcase --parallel
$capture = Join-Path (Get-Location) 'artifacts/showcase-new'
Start-Process -FilePath .\build\Release\EdgeTuckShowcase.exe -ArgumentList ('"' + $capture + '"') -WindowStyle Hidden -Wait
```

成功时目录包含三张 PNG、BMP 序列、采样时间和 `capture.json`；失败时检查 `error.txt`。预览截图并确认内容后，再复制三张 PNG 到 `docs/assets/`。

动图编码额外需要 Python 和 Pillow，仅用于制作文档，不属于应用运行或构建依赖：

```powershell
python tools/encode-showcase.py artifacts/showcase-new docs/assets/drawer-demo.gif
```

关闭可选工具构建：

```powershell
cmake -S . -B build -DEDGETUCK_BUILD_SHOWCASE=OFF
```

发布源码时只保留 `docs/assets/` 下的最终素材。原始帧、临时示例文件及捕获日志保留在被忽略的 `artifacts/` 中。
