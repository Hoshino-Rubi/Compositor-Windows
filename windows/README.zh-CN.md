# Compositor Windows 原生版

C++20 / Qt 6 Widgets，版本 **0.3.0**。保留原版 Swift / AppKit / Metal 工程；Windows 应用没有浏览器运行时。

## 运行

从 [GitHub Releases](https://github.com/Hoshino-Rubi/Compositor-Windows/releases/latest) 下载便携包。

解压 `Compositor-0.3.0-win-x64.zip` 后运行 `Compositor.exe`。保留 DLL、插件、translations、models 和 licenses 文件夹。Windows 10 / 11 x64，无需另装 Qt、Python 或 Node.js。
右上角即时切换 English、简体中文、繁體中文，语言选择会保存。

## 本次补齐的功能

- **PSD / PSB**：8 位 RGB 分层导入，Raw / PackBits RLE / ZIP / ZIP Prediction，Unicode 名称、组、显隐、不透明度、混合模式、像素/矢量蒙版和剪贴关系。色阶、曲线、色相/饱和度、反相可成为调整层。矩形、圆角矩形和椭圆可成为形状层；文本导入内容、字体与颜色片段，保留原始栅格外观。
- **RAW**：使用 LibRaw 解码实际传感器数据。相机白平衡、16 位线性数据、曝光、色温、色调和增强控制，显影后进入图层；没有用嵌入式缩略图代替 RAW。具体相机及压缩格式取决于 LibRaw 0.22.2。
- **Camera Raw**：光线、颜色/白平衡、参数与 RGB 曲线、8 色混合器、8 个点颜色、四组色彩分级、纹理/清晰度/去朦胧、三种辉光、三种暗角、颗粒、锐化和降噪、光学/色差/去边、几何与引导校正、相机校准。复用原版 C 像素内核，提供直方图、矢量示波器、裁切提示、锐化蒙版预览、分组开关和 JSON 预设。预览异步计算，确认后全分辨率应用，一次撤销可恢复。
- **液化与涂抹**：模糊工具的参数栏可切换，支持大小、硬度、流量、图像/蒙版和选区限制，完成笔划可撤销。
- **主体/对象选择与去背景**：本地 U²-Netp + ONNX Runtime CPU 推理。对象模式点击后选取模型前景中的连通对象；去背景创建可撤销的蒙版。模型随包提供，运行不下载、不上传图片。
- HEIC/HEIF（HEVC 解码）与 SVG 导入；SVG 导入为像素图层。
- 多边形套索、选区羽化、软边选区绘画/填充/滤镜/复制/蒙版、范围式 HSV 与可调边界、动态模糊调整层。
- 多图层/组的缩放与旋转，四角自由扭曲，组图层效果。
- 画布文字行内编辑、混合字体与颜色片段、快捷键自定义与冲突检查。
- `.comp` 外部修改检测：干净标签自动重新加载；有未保存编辑的标签保留本地内容并显示提示。先检查文件时间/尺寸，发生变化才后台重读图片。

## 工作区与已有功能

文档标签栏、随工具变化的参数栏、58px 左侧工具轨、可调宽度的图层面板。矢量工具图标 24 逻辑像素、按钮 36 逻辑像素，支持 DPI 缩放。小窗口的工具与参数可滚动。
每个标签分别保留撤销、选区和缩放。图层支持多选、拖放排序/嵌套、编组、合并、复制、24 种混合模式及不透明度。

移动与吸附、旋转/缩放手柄；矩形/椭圆选框、魔棒、加选/减选/交集、反选、扩展/收缩；画笔/橡皮、克隆、修复、渐变、内容识别填充；文字与形状；像素蒙版和剪贴蒙版；色相/饱和度、色阶、曲线、曝光、黑白、色彩平衡、渐变映射、颗粒/杂色、模糊；六类图层效果；标尺、参考线、网格、裁剪、画布/图像大小和透明边缘裁切。PNG、JPEG、WebP、BMP、TIFF 导入，全分辨率 PNG/JPEG 导出。

## 常用操作

- `.comp` 选择整个项目目录；PSD、RAW 等使用“导入”、拖放或命令行文件路径。
- `V` 移动，`Ctrl+T` 多层/组变换；图层菜单 → 自由扭曲。
- `M` 选框，`L` 套索（参数栏选择多边形），`W` 魔棒/对象选择，`Shift+F6` 羽化。
- `B` 画笔，`E` 橡皮，`J` 修复，`S` 克隆（Alt 点击设置来源）。`R` 模糊/涂抹/液化。
- `G` 渐变，`U` 矩形，`Shift+U` 椭圆，`T` 文字，`I` 吸管，`H` 抓手，`Z` 缩放。
- 已有文字层使用文字工具点击，`Ctrl+Enter` 应用行内编辑、`Esc` 取消；图层菜单 → 编辑文字可设置字体、颜色等。
- 调整菜单 → Camera Raw；曲线上点击增加点、右键删除中间点。颜色页点击预览拾取白平衡，点颜色页勾选拾取后点击图片。几何页开启引导后拖出最多两条线。
- 右键 Camera Raw 图表切换直方图/矢量示波器；Alt 拖动光线滑块查看裁切。预览提示不会写入最终图像。
- 空格拖动画布，Ctrl 滚轮缩放；`Ctrl+0` 适合画布，`Ctrl+1` 100%。
- `Ctrl+A` 全选，`Ctrl+D` 取消选区，`Ctrl+Shift+I` 反选；Shift 加选、Alt 减选。
- `Alt+Backspace` 前景填充，`Ctrl+Backspace` 背景填充，`Shift+F5` 内容识别填充。
- `Ctrl+G` 编组，`Ctrl+Shift+G` 取消编组，`Ctrl+E` 合并，`Ctrl+W` 关闭标签。编辑菜单 → 快捷键可修改并保存。

## 兼容范围

继续读取上游 `.comp` version 1–11，写入 version 11。未触碰的 PNG 字节及未知非渲染元数据保留。文字保留 fontRuns/colorRuns；Windows 缺少的字体可能替代，重新编辑会按 Windows 字体排版。像素滤镜/绘画/自由扭曲会栅格化文字和形状。选区与历史为会话状态。

PSD 的范围与原版导入器一致为 8 位 RGB；不支持 CMYK、16/32 位 PSD。Photoshop 智能对象、复杂路径/文字变形与部分效果保留栅格后备或给出转换报告，不承诺 Photoshop 所有结构都可继续编辑。导入不是 PSD 导出。
Camera Raw 对应本项目原版的功能，不是 Adobe 插件；镜头配置使用原版通用校正内核，不带 Adobe 相机/镜头数据库。
对象识别使用 Windows 本地模型，效果可能与 macOS Vision 不同；它对模型识别出的前景做连通区域选择，不能保证识别图中任意对象。
CPU 合成、字体、几何插值、模糊与发光边缘可能与 Metal 输出不同，不宣称逐像素一致。

画布最大单边 16,384px、总计 64MP，图像和蒙版分别合计 100MP，最多 1,000 层。大图滤镜需要更多内存和时间。保存使用暂存目录、摘要检查及恢复备份；目录替换不保证断电事务原子性。

## 构建与验证

需要 Visual Studio 2022 C++ / Windows SDK、CMake 3.24+、PowerShell 7、Qt 6.8.3 MSVC 2022 x64（Qt Image Formats 和 SVG）。

```powershell
pwsh -File ./windows/scripts/prepare-dependencies.ps1
pwsh -File ./windows/scripts/build.ps1 -QtDir C:/Qt/6.8.3/msvc2022_64
pwsh -File ./windows/scripts/package.ps1 -QtDir C:/Qt/6.8.3/msvc2022_64 -SkipBuild
```

首次准备依赖需要联网，后续运行不需要。vcpkg 提交、ONNX Runtime 1.22.1 和模型哈希固定在 dependencies.lock.json。构建运行 Qt Test；测试包括实际 PSD/PSB/DNG 解码、分层/文本/矢量、Camera Raw、液化手势及撤销、软选区、组效果与变换、主体模型、导入流程、外部重载、国际化与原有编辑回归。
fixtures 是自行生成的小型测试图；可从仓库根目录用 Python 执行 `windows/tests/generate_fixtures.py` 再生成。Python 不是应用运行依赖。

```powershell
./Compositor.exe --sample --language zh-CN --capture-size 1180x820 --capture ./sample.png
./Compositor.exe --render-project ./Example.comp --output ./flattened.png
```

截图只渲染应用自己的窗口。三份 JSON 文案位于 translations/；Qt 对话框使用 Qt 翻译，系统文件对话框跟随 Windows。
macOS/Xcode 测试没有在 Windows 上执行。源码沿用 MIT 许可证，第三方动态库、模型、许可与源码位置见 licenses/。程序未签名。
