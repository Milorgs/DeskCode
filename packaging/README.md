# DeskCode Package Manager Manifests (`packaging/`)

[简体中文](#简体中文) · [English](#english)

---

## 简体中文

本目录包含 DeskCode 在四大主流包管理器中的官方发布清单，完整覆盖全部 5 个跨平台发行包：

- `desk-agent-macos-universal.tar.gz`（macOS Apple Silicon + Intel Universal 2）
- `desk-agent-linux-x64.tar.gz`（Linux `x86_64`）
- `desk-agent-linux-arm64.tar.gz`（Linux `aarch64` / `arm64`）
- `desk-agent-windows-x64.zip`（Windows `x64`）
- `desk-agent-windows-arm64.zip`（Windows `ARM64`）

### 用户安装指南

#### 1. Homebrew（macOS & Linux `x86_64` / `arm64`）

```sh
# 直接从本地或远程仓库 Formula 安装
brew install --formula packaging/homebrew/deskcode.rb

# 如已配置独立 Tap（Milorgs/tap）：
brew tap milorgs/tap
brew install deskcode
```

#### 2. Scoop（Windows `x64` / `arm64`）

```powershell
# 直接通过清单安装
scoop install .\packaging\scoop\deskcode.json

# 或从 Bucket 安装：
scoop bucket add milorgs https://github.com/Milorgs/scoop-bucket
scoop install milorgs/deskcode
```

#### 3. WinGet（Windows 10/11 `x64` / `arm64`）

```powershell
# 已收录至 winget-pkgs 后直接安装：
winget install Milorgs.DeskCode

# 本地验证并安装单文件清单：
winget validate --manifest .\packaging\winget\Milorgs.DeskCode.yaml
winget install --manifest .\packaging\winget\Milorgs.DeskCode.yaml
```

#### 4. Arch Linux AUR（`deskcode-bin`，支持 `x86_64` & `aarch64`）

```sh
# 使用 AUR 助手（yay / paru）：
yay -S deskcode-bin

# 或本地使用 makepkg 构建安装：
cd packaging/aur
makepkg -si
```

### 发布维护：一键同步版本号与 SHA256 校验和

每次发布新版本并汇总生成包含全部 5 个归档文件的 `SHA256SUMS.txt` 后，运行纯 Python 3 标准库脚本即可确定性地同步更新全部清单：

```sh
python3 packaging/update_checksums.py 1.17.0 dist/SHA256SUMS.txt
```

该命令会同步更新：

1. `packaging/homebrew/deskcode.rb`
2. `packaging/scoop/deskcode.json`
3. `packaging/aur/PKGBUILD`
4. `packaging/aur/.SRCINFO`
5. `packaging/winget/Milorgs.DeskCode.yaml`

### 运行清单自检测试

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B packaging/tests/test_packaging.py
```

---

## English

This directory houses official DeskCode manifests for the 4 major package managers across all 5 release archives (`desk-agent-macos-universal.tar.gz`, `desk-agent-linux-x64.tar.gz`, `desk-agent-linux-arm64.tar.gz`, `desk-agent-windows-x64.zip`, `desk-agent-windows-arm64.zip`).

### Installation

#### 1. Homebrew (macOS Universal & Linux `x86_64` / `arm64`)

```sh
# Install directly from the formula file
brew install --formula packaging/homebrew/deskcode.rb

# Or via Tap (Milorgs/tap):
brew tap milorgs/tap
brew install deskcode
```

#### 2. Scoop (Windows `x64` / `arm64`)

```powershell
# Install directly from the manifest
scoop install .\packaging\scoop\deskcode.json

# Or via Bucket:
scoop bucket add milorgs https://github.com/Milorgs/scoop-bucket
scoop install milorgs/deskcode
```

#### 3. WinGet (Windows 10/11 `x64` / `arm64`)

```powershell
# Install from winget-pkgs:
winget install Milorgs.DeskCode

# Validate & install local singleton manifest:
winget validate --manifest .\packaging\winget\Milorgs.DeskCode.yaml
winget install --manifest .\packaging\winget\Milorgs.DeskCode.yaml
```

#### 4. Arch Linux AUR (`deskcode-bin`, `x86_64` & `aarch64`)

```sh
# Via AUR helper:
yay -S deskcode-bin

# Or build locally with makepkg:
cd packaging/aur
makepkg -si
```

### Release Maintenance: Updating Version & SHA256 Checksums

After publishing or staging all 5 release archives and generating `SHA256SUMS.txt`, run the zero-dependency Python 3 updater:

```sh
python3 packaging/update_checksums.py 1.17.0 dist/SHA256SUMS.txt
```

This deterministically updates `homebrew/deskcode.rb`, `scoop/deskcode.json`, `aur/PKGBUILD`, `aur/.SRCINFO`, and `winget/Milorgs.DeskCode.yaml`.

### Running Verification Tests

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B packaging/tests/test_packaging.py
```
