# 149: 更换 MacSoSo 应用图标

## 背景

2026-10-23 维护者提供了新的 MacSoSo 图标源图，需要替换 Xcode AppIcon
资源集中的全部尺寸。源图为 2048×2048 RGB PNG，外围是白色页面背景；直接
缩放会让 macOS Dock 和访达显示白色方块，因此需要先移除与画布边缘相连的
背景，并保留放大镜与蓝色圆球主体。

## 改动

- 将源图保存到 `assets/appicon/macsoso_icon.png`。
- 新增 `scripts/generate_app_icons.py`：
  - 仅移除与图片边缘相连的近白色背景，不误删图形内部的白色高光。
  - 对透明边界做轻微羽化，缩小后减少锯齿。
  - 按 macOS 图标规范将图形置于 1024×1024 透明画布，主体最大边为
    824×824。
  - 生成 `icon_16/32/64/128/256/512/1024.png`，并替换
    `MacSoSo/Assets.xcassets/AppIcon.appiconset/` 中的现有资源。
  - 对 64 px 及以下尺寸进行轻度锐化，改善小尺寸辨识度。
- Makefile 新增 `make icons`，便于以后从同一源图重新生成图标。
- README 中英文版继续引用 `icon_256.png`，会随资源替换自动显示新图标。
- 重新构建 `MacSoSo.app` 与 `dist/MacSoSo-1.3.0-arm64.dmg`。

## 验证

- 7 个 AppIcon PNG 尺寸分别为 16、32、64、128、256、512、1024，均包含
  透明外背景。
- Xcode Release 构建成功，`actool` 已重新生成 `AppIcon.icns` 和
  `Assets.car`。
- 从构建产物 `AppIcon.icns` 导出的 256 px 图像与新源图一致。
- 应用元数据保持为 `MacSoSo` / `com.macsoso.app` / 1.3.0 (4)，ad-hoc
  签名校验通过。
- 重新构建并重启本地开发版应用，确认加载新图标。
- 新 DMG 大小约 3.6 MB，SHA256：
  `cd4158ec99820061f167dc622846eb5a3bb9300d2ec8954933c147f64d31e5c4`。
