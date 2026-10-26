# 构建与发布流程

本文说明如何在本地打包，以及如何通过 GitHub Actions 自动构建并发布
Release。发布物是内嵌 RE2/Abseil 动态库的 DMG，同时提供 SHA256。

## 1. 本地打包

环境要求：

- macOS 14+（构建机需安装 Xcode，命令行工具不够）
- Homebrew 的 `re2` 与 `abseil`
- 对应架构的 Homebrew（Apple Silicon 为 `/opt/homebrew`，Intel 为 `/usr/local`）

```bash
brew install re2 abseil

# 当前机器架构
./scripts/package_release.sh

# 指定架构与版本
./scripts/package_release.sh --arch arm64 --version 1.3.0
./scripts/package_release.sh --arch x86_64 --version 1.3.0
```

产物位于 `dist/`：

```
MacSoSo-1.3.0-arm64.dmg
MacSoSo-1.3.0-arm64.dmg.sha256
```

脚本会：

1. 用 `xcodebuild` 执行 Release 构建（未签名），并通过
   `RE2_PREFIX` / `ABSEIL_PREFIX` 覆盖 Homebrew 路径。
2. 递归复制所有非系统动态库到 `Contents/Frameworks`，把安装名改写为
   `@rpath/...`（App）与 `@loader_path/...`（库之间）。
3. 校验没有残留的 `/opt/homebrew` 或 `/usr/local` 依赖。
4. 使用指定 identity 签名；默认 `-`（ad-hoc）。
5. 写入版本号、打包 DMG（含 `/Applications` 快捷方式、LICENSE、NOTICE、
   第三方许可证）并生成 SHA256。

常用参数：

| 参数 | 说明 |
|------|------|
| `--version x.y.z` | 覆盖 `CFBundleShortVersionString` |
| `--build N` | 覆盖 `CFBundleVersion`，默认取 `Info.plist` 中的值 |
| `--arch arm64\|x86_64` | 目标架构，默认当前主机 |
| `--output-dir DIR` | 输出目录，默认 `dist/` |
| `--sign-identity ID` | 代码签名证书；`-` 为 ad-hoc |
| `--skip-build` | 跳过编译，直接打包已有产物 |

## 2. GitHub Actions 自动发布

工作流：[`.github/workflows/release.yml`](../.github/workflows/release.yml)

触发方式：

- 推送形如 `v1.3.0` 的标签。
- 在 Actions 页面手动运行 `Release`，输入一个已存在的标签。

流程：

1. `macos-26` 上运行 `make test-fast`（12162 个断言）。
2. 两个 runner 并行构建：
   - `macos-26` → arm64
   - `macos-26-intel` → x86_64
3. 每个架构执行 `package_release.sh`，校验 SHA256 后上传 artifact。
4. 汇总两个 DMG 与校验文件，用 `gh release create` 创建 Release，
   release notes 内含改编声明、安装步骤与改动摘要。

### 首次推送与发布

本地仓库尚未配置远端时：

```bash
# 在 GitHub 创建空仓库后
git remote add origin https://github.com/<你的账号>/MacSoSo.git
git push -u origin main

# 打标签即触发线上构建与 Release
git tag -a v1.3.0 -m "MacSoSo 1.3.0"
git push origin v1.3.0
```

工作流需要仓库的 `contents: write` 权限（文件内已声明）；若仓库设置了
Actions 权限限制，请在 Settings → Actions → General 中允许工作流创建
Release。

## 3. 签名与公证

默认产物是 **ad-hoc 签名**，可以运行，但下载后首次启动会被 Gatekeeper
拦截，需要右键「打开」或：

```bash
xattr -dr com.apple.quarantine /Applications/MacSoSo.app
```

要无警告分发，需要 Apple Developer ID：

```bash
./scripts/package_release.sh \
  --sign-identity "Developer ID Application: Your Name (TEAMID)"
```

然后对 DMG 公证并装订：

```bash
xcrun notarytool submit dist/MacSoSo-1.3.0-arm64.dmg \
  --apple-id "you@example.com" \
  --team-id "TEAMID" \
  --password "app-specific-password" \
  --wait

xcrun stapler staple dist/MacSoSo-1.3.0-arm64.dmg
```

若要在 CI 中自动签名/公证，可在 workflow 的构建步骤前导入
`Developer ID Application` 证书到临时 keychain，并把 `--sign-identity`
传给脚本，再在打包后调用 `notarytool`。证书与密码应放在 GitHub Secrets，
不要提交到仓库。

## 4. 架构与兼容性

- 本项目生成 **单架构** 产物：arm64 或 x86_64。Intel Mac 可通过 Rosetta
  运行 arm64 版本，但推荐使用对应架构的 DMG。
- 部署目标为 macOS 14.0；应用未启用 App Sandbox，需要用户授予完全磁盘
  访问权限才能索引受保护目录。
- re2/abseil 来自 Homebrew，打包时会复制实际链接到的版本。升级 Homebrew
  依赖后请重新打包，脚本会重新解析依赖。

## 5. 常见问题

**`xcodebuild: error: tool 'xcodebuild' requires Xcode`**

`xcode-select` 指向了 Command Line Tools。脚本会自动查找
`/Applications/Xcode*.app`；也可以在运行前显式设置：

```bash
export DEVELOPER_DIR=/Applications/Xcode_26.1.app/Contents/Developer
```

**构建产物仍引用 `/opt/homebrew`**

脚本本应在签名前报错并退出。检查是否使用了 `--skip-build` 打包了旧产物，
或重新执行完整打包。

**Release 下载后提示“已损坏”**

通常是 Gatekeeper 对 ad-hoc 签名文件的提示，不是文件损坏。用上面的
`xattr` 命令清除 quarantine，或配置 Developer ID 公证。
