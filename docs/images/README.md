# 🖼️ README 演示图

这些 PNG 使用项目内真实的 Overlay、SaveExportDialog、RecordingIndicator 和 SettingsDialog 控件离屏渲染；工作台内容、选区和录制统计均为合成演示样例。没有截取用户桌面，也不展示个人窗口或文件内容。

## 🔨 重新生成

在项目根目录运行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target seashot_readme_demo -j 2
./build/src/seashot_readme_demo docs/images
```

生成工具默认不参与应用构建。它强制使用 Qt offscreen 平台和临时 QSettings 路径，不启动截图后端，不注册全局快捷键，也不写入剪贴板。

## 🎨 内容与样式

- 配图源代码：`probes/readme-demo.cpp`。
- 输出：四张 1440×1000 PNG，展示截图标注、保存导出、录制导出和设置。
- 界面使用 Fusion 样式和示例调色板；安装 Noto Sans CJK SC 字体与 Breeze 图标主题，可得到与当前配图接近的效果。
- 页首及窗口装饰使用 `resources/icons/seashot.png`。
- 修改界面后重新生成图片，将配图与代码一起提交即可。
