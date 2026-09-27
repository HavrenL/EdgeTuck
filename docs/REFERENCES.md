# 参考来源与资源说明

本文区分实现研究、构建依赖和随仓库分发的资源。历史研究记录不等于已经采用对应实现，也不构成兼容性保证。

## 随项目分发的资源

| 内容 | 来源与生成方式 |
| --- | --- |
| `resources/app.svg` | 项目内的几何图标源文件 |
| `resources/app.ico` | 由 `resources/generate-icon.ps1` 中的几何绘制生成 |
| `docs/assets/*.png` | `tools/showcase.cpp` 调用本项目真实 UI 生成的隔离场景截图 |
| `docs/assets/drawer-demo.gif` | 同一场景的实际开合帧，经 `tools/encode-showcase.py` 编码 |
| 展示背景及图片缩略图 | 展示工具用几何曲线和渐变绘制，无外部壁纸或图库素材 |
| 截图中的系统文件夹/文本图标 | 运行系统的 Windows Shell 输出，作为应用界面的一部分出现在截图中；未提取为独立图标包 |

源码构建链接 Windows SDK 和系统库。当前 CMake 没有通过包管理器下载或打包以下参考项目。

## 材质研究

2026-09-06 的设计研究参考了以下项目的视觉效果、处理阶段与参数组织：

| 项目 | 当时读取的提交 | 参考范围 |
| --- | --- | --- |
| [liquid-glass-studio](https://github.com/iyinchao/liquid-glass-studio) | `d13c3e53813ebc1d8b52d878071b63212a550ebc` | 背景预模糊、折射、RGB 分离与边缘亮带 |
| [liquidDX11](https://github.com/poncippg-spec/liquidDX11) | `00c2b2b19eb5523abde473b2c08220326d4146d4` | 原生桌面背景获取和材质管线的组织 |
| [LiquidGlassWinUI](https://github.com/luckyelysia/LiquidGlassWinUI) | `647fd60c3ded87dc2a7472f813ec86a265529509` | 模糊与折射分离、材质参数组织 |
| [kube.io](https://kube.io/blog/liquid-glass-css-svg/) | 研究文章 | 折射、表面曲线与位移的解释 |

本项目包含独立的 WebGL 材质实验及对应的原生 HLSL 实现。研究记录说明未整段复制参考 shader；这不是对所有代码来源的法律审计。后续实际复制第三方代码时，应按具体文件保留署名与许可。

## 桌面与 Shell 研究

以下项目用于理解桌面组织、视图过滤、菜单和辅助进程设计：

- [Windhawk mods](https://github.com/ramensoftware/windhawk-mods)：桌面接入范围、生命周期和菜单内部行为的研究。
- [Tablacus Explorer](https://github.com/tablacus/TablacusExplorer)：自建 Shell 视图的逐项过滤机制；不等于能直接过滤已有原生桌面。
- [Files](https://github.com/files-community/Files)：获取系统菜单命令及界面组织。
- [ContextMenuForWindows11](https://github.com/ikas-mc/ContextMenuForWindows11)：为 Explorer 新式菜单添加命令，不是抽屉内完整菜单的现成实现。
- [DeskBox](https://github.com/Tianyu199509/DeskBox)：分类目录组织与菜单辅助进程的调用结构。
- [Pickets](https://github.com/Creeptones/Pickets)、[NoFences](https://github.com/Twometer/NoFences)、[desktop_box](https://github.com/kof2000git/desktop_box)、[DesktopFramesPlusKai](https://github.com/superlanboy/DesktopFramesPlusKai)、[QTTabBar](https://github.com/indiff/qttabbar)：桌面展示与集成方案对照。

这些研究下载保留在本机开发资料中，不进入公开源码目录。EdgeTuck 采用 [MIT 许可证](../LICENSE)；参考项目的许可证各不相同，不能用 EdgeTuck 的许可证替代其各自条款。
