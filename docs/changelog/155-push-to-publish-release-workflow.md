# 155: 推送到 main 即自动构建并发布 Release

## 背景

147/150 建立的 `.github/workflows/release.yml` 要求标签**事先存在**，
CI 只做校验：

```
Run if [[ ! "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]]; then
Release tag 'v1.0.0' does not exist in origin. Create and push it first:
  git tag -a v1.0.0 -m "MacSoSo 1.0.0" && git push origin v1.0.0
Error: Process completed with exit code 1.
```

维护者的诉求是「后续自动推送之后，自动编译和发布」——也就是把
`git tag && git push origin v1.0.0` 这一步从人手里拿掉。

## 改动（`.github/workflows/release.yml` 重写）

### 触发方式

`on.push` 同时监听 `branches: [main]` 和 `tags: ["v*"]`，另加
`workflow_dispatch.inputs.ref` 供手动指定分支 / 标签 / 提交。
`concurrency.group: release-${{ github.ref }}` 且 `cancel-in-progress: false`，
避免同一 ref 的两个发布互相踩。`permissions: contents: write` 是建标签和发 Release 的前提。

### 新增 `preflight`：把「发哪个版本」变成一条可推导的流水线

一个 `id: release` 的步骤产出 `sha / tag / version / should_publish / tag_exists / tag_sha`
六个输出，后续 job 全部 `needs: preflight` 并 checkout 到 `outputs.sha`：

1. **提交**：默认取 HEAD（push 事件时 checkout 已经站在被推的那个 ref 上），
   只有 dispatch 显式给了 `ref` 才走 `resolve_commit()`，依次尝试
   `refs/tags/`、`refs/remotes/origin/`、裸 7～40 位 SHA。
2. **标签**：推 `v*` 标签时直接用它；dispatch 给了 `v…` 时用它；否则
   **从该提交的 `MacSoSo/Info.plist` 推导** `TAG="v$(plist_version $SHA)"`。
   `git show` + `plistlib.loads(sys.stdin.buffer.read())`——注意不能用
   `plistlib.load(stdin)`，管道不可 seek 会抛 `UnsupportedOperation`。
   版本号仍然是 CI 产物的一部分，因此 bump `CFBundleShortVersionString`
   就是发布开关。
3. **格式校验**：不匹配 `^v[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$` 直接失败，
   把 plist 写坏的成本压在 preflight 几秒钟，而不是压在两台机器构建完之后。
4. **是否发布**：标签已存在且指向别的提交，而这次又是分支推送 →
   `should_publish=no`，并打一条 `::notice title=No release::` 告诉对方
   「测试跑了但什么都没发，要发就抬版本号」。这解决了「同版本号反复推 main
   会不会覆盖已发布 Release」的问题：不会。

### `test` 每次推送都跑

`make test-fast`（`macos-26`，`brew install re2 abseil`）在
`should_publish` 判断之前，所以只跑测试不发布的推送依然有门禁意义。

### `build` 与 `publish` 受 `should_publish == 'yes'` 门控

arm64 / x86_64 双架构矩阵各自 `./scripts/package_release.sh --version … --arch …`，
`shasum -a 256 -c ./*.sha256` 自检，产物用
`actions/upload-artifact@v4`（`if-no-files-found: error`）上传。
`package_release.sh` 完全不依赖 git 或标签，所以「标签还不存在时先构建」是安全的。

### `publish` 先建标签、再幂等发布

顺序很关键：**只有测试和两个 DMG 都成功才把标签推上去**，构建失败不会留下一个
指向坏提交的标签。

```bash
if gh release view "$TAG" --repo "$GITHUB_REPOSITORY" >/dev/null 2>&1; then
  gh release upload "$TAG" dist/*.dmg dist/*.sha256 --clobber --repo "$GITHUB_REPOSITORY"
  gh release edit "$TAG" --repo "$GITHUB_REPOSITORY" --title "MacSoSo $VERSION" --notes-file RELEASE_NOTES.md
else
  gh release create "$TAG" --repo "$GITHUB_REPOSITORY" --title "MacSoSo $VERSION" \
    --notes-file RELEASE_NOTES.md dist/*.dmg dist/*.sha256
fi
```

- 建标签那一步用 `github-actions[bot]` 身份 `git tag -a` + `git push origin refs/tags/$TAG`；
  若标签已存在但指向别的提交，**拒绝改指**（`exit 1`），提示抬版本号——绝不
  `--force` 移动已发布的标签。
- 幂等发布让 dispatch 重跑同一提交时只换资产、不报错。
- 机器人自己创建的标签不会再触发一次 workflow（GitHub 对 token 产生的事件不递归
  触发），所以不会出现「建标签 → 又跑一遍发布」的循环。

### 发布说明

`RELEASE_NOTES.md` 由 heredoc + `sed` 替换 `__VERSION__ / __TAG__ / __REPO__` 生成，
内容固定包含两个 DMG 的架构区分、SHA256、ad-hoc 签名与未公证的首次启动提示、
Full Disk Access，以及**改编自 [joshua-wu/MacEverything](https://github.com/joshua-wu/MacEverything) (MIT)**
的署名与 `docs/CHANGES_FROM_UPSTREAM.md` 链接。

## 使用方式

```bash
# 1. 抬版本
#    MacSoSo/Info.plist -> CFBundleShortVersionString = 1.1.0
# 2. 推 main —— 完
git commit -am "..." && git push origin main
```

CI 会跑测试、构建两个 DMG、打 `v1.1.0` 标签并发布。
如果版本号没抬，`preflight` 会给出 `No release` 提示而不是报错退出。
想重发同一提交，用 workflow_dispatch 传分支、标签或 SHA。

## 验证

- 在一次性临时仓库 + 本地 bare origin 上回放了 preflight 的判定逻辑，覆盖
  「标签不存在（分支推）」「标签已指向他 commit」「标签已指向本 commit」
  「直接推 `v*` 标签」「plist 版本号非法」「dispatch 传分支 / 标签 / SHA」
  等 8 个 git 场景，输出与 `should_publish` 判定一致。
- 确认 `scripts/package_release.sh` 不读取 git 状态或标签，构建阶段可以早于建标签。
- 修复过程中踩到并改掉的一个坑：`plistlib.load(sys.stdin.buffer)` → 管道不可
  seek 抛 `io.UnsupportedOperation`；改为 `plistlib.loads(sys.stdin.buffer.read())`。

## 相关条目

- [147](147-external-volumes-and-release.md)：首次引入在线编译发布流水线与打包脚本
- [150](150-project-path-and-v1-release.md)：v1.0.0 首次发布（当时仍需手工建标签）
