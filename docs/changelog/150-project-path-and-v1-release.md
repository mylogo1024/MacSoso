# 150: 切换项目路径并准备 v1.0.0 首次发布

## 背景

2026-10-26 项目目录从旧的本地 checkout 路径切换为：

```text
/Volumes/mac/project_new/Everything/MacSoso
```

该目录成为后续构建、测试、打包与 GitHub Actions 发布的唯一项目根目录。
远端仓库为 `https://github.com/mylogo1024/MacSoso.git`。

尝试通过 GitHub Actions 的 `workflow_dispatch` 输入 `v1.0.0` 发布时，
`actions/checkout@v4` 报错：

```text
A branch or tag with the name 'v1.0.0' could not be found
```

原因是远端仓库尚未创建或推送 `v1.0.0` 标签；`workflow_dispatch` 只负责
选择一个已经存在的标签，不会自动创建标签。

## 改动

- `MacSoSo/Info.plist` 从内部开发版本 `1.3.0 (4)` 调整为首次公开发布
  版本 `1.0.0 (1)`。
- `docs/RELEASE.md` 的本地打包、标签、公证示例统一改为 `1.0.0`，并记录
  当前远端仓库地址。
- `README.md` 与 `README_EN.md` 的源码克隆地址改为
  `https://github.com/mylogo1024/MacSoso.git`，克隆目录改为 `MacSoso`。
- `.github/workflows/release.yml` 新增 `preflight` 任务：
  - 先校验标签名格式；
  - 再校验远端仓库确实存在该标签；
  - 缺失时输出创建和推送标签的命令，避免在 checkout 阶段报出难以理解的
    分支/标签不存在错误。
- 后续 `test`、`build`、`publish` 任务统一使用 `preflight` 输出的标签与
  版本号，不再分别解析事件输入。

## 发布步骤

在项目根目录执行：

```bash
git add .github/workflows/release.yml MacSoSo/Info.plist README.md README_EN.md \
  docs/CHANGES_FROM_UPSTREAM.md docs/RELEASE.md \
  docs/changelog/150-project-path-and-v1-release.md docs/changelog/README.md
git commit -m "Prepare MacSoso v1.0.0 release"
git push origin main
git tag -a v1.0.0 -m "MacSoSo 1.0.0"
git push origin v1.0.0
```

推送 `v1.0.0` 后，Release 工作流会自动执行测试、构建 arm64/x86_64 DMG，
并创建 GitHub Release。
