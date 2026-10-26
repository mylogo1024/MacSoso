# 151: 更换为橙色放大镜应用图标

## 背景

2026-10-26 维护者提供了新的源图（`1745571779_280601.png`，256×256 RGB
PNG，橙色放大镜轮廓），要求把 MacSoSo 的图标换成这一张。上一版图标
（条目 149）是放大镜加蓝色圆球的组合，新图去掉了圆球，只保留放大镜本体。

## 改动

- 用新源图覆盖 `assets/appicon/macsoso_icon.png`，其 MD5 与维护者提供的
  `1745571779_280601.png` 一致（`c08b1ab60ee6e58c6de86010d3e5d454`）。
- 运行 `make icons`，由 `scripts/generate_app_icons.py` 重新生成
  `icon_16/32/64/128/256/512/1024.png` 并写入
  `MacSoSo/Assets.xcassets/AppIcon.appiconset/`。
  脚本逻辑保持不变：移除与画布边缘相连的近白色背景、透明边界轻微羽化、
  按 macOS 网格居中到 1024×1024 画布（主体最大边 824×824）、
  64 px 及以下轻度锐化。
- 重新构建 `MacSoSo.app`，`actool` 生成新的 `AppIcon.icns` 与 `Assets.car`。

## 验证

- `assets/appicon/macsoso_icon.png` 与用户提供的源图 MD5 相同。
- AppIcon 资源集 7 个尺寸文件全部在 2026-10-26 14:24 重新生成。
- Release 构建成功后，从
  `build/DerivedData/Build/Products/Release/MacSoSo.app/Contents/Resources/AppIcon.icns`
  导出的图像确认为橙色放大镜，与源图一致。
