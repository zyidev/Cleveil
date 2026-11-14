# GitHub 仓库信息填写参考（About 栏）

GitHub 仓库首页右上角 **About** 只有三段可填：Description、Topics、以及 Website。其中 Description 是**单行纯文本**，
不支持按语言自动切换；README 才有语言切换（`README.md` ↔ `README.en.md` 顶部的链接）。

## 1. Description（推荐这一行，中英并列，两种语言下都成立）

```
Windows 程序卸载与残留清理工具（C++20 + Qt 6）｜A Windows uninstaller & leftover cleaner built with C++20 + Qt 6
```

字数 138 字符，符合 GitHub 简短要求。

若希望 Description 与 README 语言完全一致，就按下表替换（中文主页时用中文版，英文主页时换成英文版）：

| 场景 | 文案 |
| --- | --- |
| README.md（中文主页） | `Windows 程序卸载与残留清理工具：批量卸载向导、实时安装监控、残留深度清理，C++20 + Qt 6 实现` |
| README.en.md（英文主页） | `A Windows uninstaller & leftover cleaner with a batch wizard, live install monitor and deep cleanup — C++20 + Qt 6` |

## 2. Topics（建议，GitHub 上限 20 个）

```
windows uninstaller qt6 cpp20 leftover-cleanup registry batch-uninstall desktop-app windows-10 windows-11
```

## 3. Website

留空，或以后接 Releases 页面地址。

## 4. 其它首页设置建议

- **README 语言切换**：两份文档顶部都已有互链（`[中文](README.md) ｜ [English](README.en.md)`），
  GitHub 不会自动跳转，靠这个链接切换；想做成"按浏览器语言自动选中"需要 GitHub Pages 或
  `README` 顶部用 `<details>` 折叠双语，静态 README 做不到。
- **Releases**：`build.ps1` 产物是 `build\Cleveil.exe` + Qt DLL 目录，建议打包成 `Cleveil-1.0-win64.zip`
  挂到 Release，而不是把 exe 单独提交进仓库。
- **.gitignore**：至少忽略 `build/`、`*.pdb`、`docs/_t.csv` 这类临时导出。
