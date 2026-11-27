# CrossPointer 🎯

<p align="center">
  <strong>参考 macOS 26 (Tahoe) Liquid Glass 设计美学的极简、纯绿色 Windows 屏幕十字辅助线工具</strong>
  <br />
  <em>A minimalist, pure-green mouse pointer crosshair overlay for Windows, crafted with unified macOS 26 Liquid Glass aesthetics and zero-idle-wakeup Direct2D performance.</em>
</p>

<p align="center">
  <a href="https://github.com/Crucialmer/CrossPointer/releases/latest"><img src="https://img.shields.io/github/v/release/Crucialmer/CrossPointer?color=007AFF&label=Release" alt="Release"></a>
  <img src="https://img.shields.io/badge/Language-C%2B%2B20-blue" alt="C++20">
  <img src="https://img.shields.io/badge/Platform-Windows%2010%2F11-0078D4" alt="Platform">
  <img src="https://img.shields.io/badge/Direct2D-Zero--Stall%20240Hz-green" alt="Direct2D">
  <img src="https://img.shields.io/badge/High%20DPI-Up%20to%208K%20Ready-orange" alt="8K Ready">
  <img src="https://img.shields.io/badge/License-MIT-brightgreen" alt="License">
  <img src="https://img.shields.io/badge/Portable-100%25%20Pure%20Green-success" alt="Pure Green">
</p>

---

## ✨ 为什么选择 CrossPointer？

在日常浏览超长 Excel 表格、审阅复杂数据、代码审校、绘图或对齐设计时，我们经常需要快速准确定位鼠标所在的行与列。

**CrossPointer** 专为轻量高效而生：
- **极简小巧**：单一原生 C++20 可执行文件（约 **574 KB**，压缩包约 **326 KB**），静态链接零外部依赖，内存占用低于 10 MB。
- **100% 纯绿色**：零安装、零注册表垃圾、零外部文件外溢，所有配置严格保存在程序同目录下。
- **全局 macOS 26 Liquid Glass 美学**：从托盘右键菜单、偏好设置面板、关于软件卡片、实时进度更新窗口到系统提示弹窗，全链路统一液态玻璃光学质感与深浅双模式自适应。

---

## 🌟 核心特性 (v1.0)

- 🪟 **全局 macOS 26 (Tahoe) Liquid Glass 视觉体系**：
  - **统一液态玻璃视觉引擎 (`liquid_glass`)**：全抗锯齿 GDI+ 离屏双缓冲渲染，融合光学垂直玻璃渐变、顶部镜面高光（Specular Top Sheen）、玻璃折射边框与 Windows 11 DWM 原生圆角。
  - **定制 macOS 26 托盘右键菜单**：内置同心顶部状态卡片（微缩 Squircle 图标 + 发光呼吸状态点 + 版本号）、SF-Symbols 风格矢量图标、蓝色胶囊悬停高亮、快捷键右侧提示与红色退出警示，支持键盘方向键导航与多屏边缘防裁切。
  - **独立「软件更新」窗口与实时流式进度条**：无边框居中液态玻璃卡片，支持启动后台静默检查；下载阶段内嵌 **6px 细长圆角胶囊进度条**，实时刷新已下载大小、百分比与速率，支持随时**取消下载**与一键原地热替换重启。
  - **独立「关于软件」卡片与全套液态玻璃提示弹窗**：关于窗口集成居中英雄图标、版本号徽标、核心特性玻璃卡片与可点击 GitHub 链接；全部系统提示升级为统一 Liquid Glass 光学圆球弹窗。
  - **纯正 macOS 26 偏好设置面板**：右对齐胶囊开关（Toggle Switch）、全圆角胶囊按钮、双缓冲抗锯齿自定义滑块、SF-Symbols 风格分栏图标、触感修饰键帽、双模式色彩编辑（自动主题 / 独立固定色）与实时指针留白画卷预览。
- ⚡ **零待机功耗与 240Hz 零阻塞 Direct2D 渲染引擎**：
  - **待机 0% 唤醒**：`timeBeginPeriod(1)` 精密调度与跟踪定时器仅在十字线显示时按需激活，静默常驻托盘时实现真正零定时中断与零电量消耗。
  - **零阻塞立即呈现**：Direct2D 启用 `D2D1_PRESENT_OPTIONS_IMMEDIATELY`，单次 GPU `Clear` 渲染并按需重绘，120Hz/144Hz/240Hz 高刷屏丝滑跟手。
- 🛡️ **工程健壮性与纯绿色便携**：
  - **Explorer 重启自愈**：监听 `TaskbarCreated` 自动恢复托盘图标；单击/双击防抖分离不误闪十字线。
  - **配置原子落盘与热键冲突回滚**：`.tmp` 原子替换写入 `CrossPointer.ini`，完整支持全套 OEM 标点热键往返序列化。
  - **单键智能复用 & 无序修饰键 & 8K 高 DPI 适配**：显隐与退出可设为同一组合键，修饰键任意顺序触发，`PerMonitorV2` 跨屏无损缩放。

---

## 🚀 快速上手

1. 前往 **[Releases 页面](https://github.com/Crucialmer/CrossPointer/releases/latest)** 下载绿色压缩包 `CrossPointer.zip` 并解压。
2. 直接双击运行解压出的 `CrossPointer.exe`（默认静默常驻托盘，按下 `Shift + Alt + E` 即可呼出十字线）。
3. **常用交互**：
   - **默认切换显隐快捷键**：`Shift + Alt + E`（可在设置中自定义，支持单键复用）
   - **默认退出程序快捷键**：`Shift + Alt + Q`（可在设置中自定义，支持与显隐设为同一键）
   - **系统托盘**：单击切换显隐，双击打开偏好设置，右键呼出 macOS 26 Liquid Glass 托盘菜单。

---

## 🛠️ 本地编译构建

**环境要求**：
- Windows 10 / 11
- MSVC (Visual Studio 2022 或 C++ Build Tools)
- CMake >= 3.24
- Ninja

```powershell
# 1. 生成构建目录
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# 2. 编译为纯绿色单文件 (/MT 静态链接)
cmake --build build -j 8
```
编译完成后将在根目录输出 `CrossPointer.exe`。

---

## 📄 开源许可证

本项目基于 [MIT License](LICENSE) 开源。
