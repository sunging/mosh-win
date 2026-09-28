# mosh-win

`mosh-win` 是官方 Mosh 1.4.0 的 x64 Windows 原生客户端移植工程。它只构建客户端，不包含 Windows 版 `mosh-server`。

发布包包含两个 Windows PE 程序。第三方库及 MinGW 运行库均静态链接；
Windows 系统 DLL（UCRT API-set、Kernel32、Winsock 和 CNG）由操作系统提供：

- `mosh.exe`：使用 Windows 系统 OpenSSH 登录远端、启动 Linux `mosh-server`，然后启动本地客户端。
- `mosh-client.exe`：执行 Mosh 的加密 UDP 协议、状态同步、漫游恢复和 VT 终端显示。

目标平台为 x64 Windows 10 22H2 或 Windows 11，以及 Windows Terminal/ConPTY 等支持 VT 的现代终端。项目不依赖 Cygwin、MSYS2、OpenSSL 或 ncurses 运行时。

## 前置条件

- CMake 3.28 或更高版本。
- x86_64 MinGW-w64 UCRT 工具链（GCC 15.1.0）；通过 `-DMINGW64_ROOT`、环境变量 `MINGW64_ROOT` 或 `PATH` 上的 `gcc.exe` 定位。CMake 会强制校验 x64、UCRT 和 GCC 15.1.0。
- Git（只用于幂等地应用上游补丁）。
- Windows 自带的 `C:\Windows\System32\OpenSSH\ssh.exe`。
- 第一次配置时能访问 zlib.net 和 GitHub；所有下载都校验 SHA-256。

## 构建

在普通 PowerShell 中运行：

```powershell
Set-Location <path-to-mosh-win>
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Release
```

开发构建允许在移植文件尚未齐全时完成 CMake 配置：

```powershell
cmake --preset mingw64-dev
cmake --build --preset mingw64-dev
```

Release 预设启用 `MOSH_REQUIRE_COMPLETE_PORT=ON`，任何缺失的客户端或平台源文件都会令配置失败。当前构建目录的下载缓存已经存在时可用离线模式：

```powershell
.\scripts\build.ps1 -Configuration Release -Offline
```

对应源码包解压后的 `third_party/source` 可直接用于完全离线构建。在
`mosh-win` 子目录运行：

```powershell
.\scripts\build.ps1 -Configuration Release -Clean -Offline `
  -DependencySourceRoot ..\third_party\source
```

依赖锁定如下：

| 依赖 | 版本 | SHA-256 |
| --- | --- | --- |
| Mosh | 1.4.0 (`bc73a263`) | `ae581fbddf038730af9eee4d319a483288395a0722d0c94c7efb7fdbdbb0dbac` |
| protobuf C++ | 3.21.12 / v21.12 | `4eab9b524aa5913c6fffb20b2a8abf5ef7f95a80bc0701f3a6dbb4c607f73460` |
| zlib | 1.3.1 | `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23` |

protobuf 和 zlib 只以静态库链接。Mosh 自身的归档只作为上游源码输入，不使用其 Autotools 构建，也不构建 `mosh-server`。

终端宽度表固定使用 Unicode 17.0.0。生成后的 C++ 表已签入源码，正常构建不依赖 Python 或联网；维护者可用下列命令校验官方数据哈希并重生成：

```powershell
python .\scripts\generate-unicode-width.py --download
python .\scripts\generate-unicode-width.py --check
```

## 使用

最常见的调用方式：

```powershell
.\mosh.exe user@example.com
.\mosh.exe -p 60001 user@example.com
.\mosh.exe --ssh-option=-i --ssh-option=$env:USERPROFILE\.ssh\id_ed25519 user@example.com
```

启动器复用 `%USERPROFILE%\.ssh\config`、Windows ssh-agent、主机密钥确认和密码提示。远端必须安装兼容的 `mosh-server`，且客户端能够直接访问服务器选择的 UDP 端口（默认范围通常为 60000–61000）。SSH 的 ProxyJump 只负责启动阶段，不会转发 Mosh UDP 流量。

底层接口也可直接使用：

```powershell
$env:MOSH_KEY = '<22-character-key>'
.\mosh-client.exe 203.0.113.10 60001
Remove-Item Env:MOSH_KEY
```

正常使用时不要手工设置密钥；`mosh.exe` 会为子进程构建专用环境并在创建进程后清理内存中的密钥。

## 测试

```powershell
.\scripts\test.ps1 -Configuration Release
```

测试脚本运行 CTest，然后检查两个 EXE 的 PE 导入表。当前 12 项 CTest 覆盖
OCB/RFC 向量和篡改拒绝、随机数/base64、protobuf/zlib 状态同步、Unicode
framebuffer、resize/alternate screen、控制台恢复、IPv4/IPv6，以及真实 loopback
UDP 上的丢包、重复、乱序、源端口漫游和虚拟 15 秒断网恢复。启动器测试使用伪
`ssh.exe` 检查 Windows 参数引用、远端 shell 引用、启动信息解析和失败路径。
PE 审计会拒绝 Cygwin/MSYS、MinGW C++ 运行时、OpenSSL、protobuf、zlib 和
ncurses DLL。

仓库中的 `tests/wsl/run-e2e.ps1` 可针对 WSL2 Ubuntu 临时启动高端口 `sshd` 和 Linux `mosh-server`，进行 Windows OpenSSH → Linux 服务端 → 原生 UDP 客户端互操作测试。测试夹具不安装 Windows 服务、不保存凭据，完成后应停止临时进程。

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\wsl\run-e2e.ps1 `
  -Distro Ubuntu -InstallOpenSshServer
```

`-InstallOpenSshServer` 只在 WSL 发行版缺少 `sshd` 时安装软件包；夹具始终以
临时配置、高端口和临时密钥运行，不启用常驻服务。部分 Windows OpenSSH 9.5
版本在远端命令成功后仍可能触发
[Win32-OpenSSH #1899](https://github.com/PowerShell/Win32-OpenSSH/issues/1899)
并返回异常退出码。启动器只在已经严格解析到有效 `MOSH CONNECT` 后容忍该
退出，同时发出警告；连接信息缺失或格式错误仍然失败。

可复现性验收会在两个不同的干净 Release 目录构建并逐一比较两个 EXE 的
SHA-256（不宣称 ZIP 容器本身逐字节可复现）：

```powershell
.\scripts\verify-reproducible.ps1
```

## 打包

```powershell
.\scripts\package.ps1
```

如果 Release 是用外置的离线依赖源码构建的，可显式把同一源码目录交给打包器：

```powershell
.\scripts\package.ps1 -SkipBuild `
  -DependencySourceRoot ..\third_party\source
```

命令在 `dist/` 中生成：

- `mosh-win-1.4.0-win1-x64.zip`：两个 EXE、README、许可证、第三方声明、静态运行库/OCB 许可文本和 SHA-256 清单。
- `mosh-win-1.4.0-win1-source.zip`：当前工程、补丁以及 CMake 实际展开的 Mosh/protobuf/zlib 对应源码。
- `SHA256SUMS.txt`：两个 ZIP 自身的 SHA-256。

可以单独执行 `scripts/audit-pe-imports.ps1` 审计任意产物：

```powershell
.\scripts\audit-pe-imports.ps1 -Binary .\out\build\mingw64-release\bin\mosh.exe
```

## 移植结构

- `src/platform/`：Win32 控制台、Winsock、CNG 和 UTF-8 平台层。
- `src/client/`：Windows 客户端入口和事件循环。
- `src/launcher/`：OpenSSH 启动器、参数引用和子进程环境管理。
- `patches/series`：按顺序应用到固定 Mosh 1.4.0 源码的补丁列表。
- `cmake/`：固定工具链、依赖、target 和补丁应用逻辑。

FetchContent 的上游目录属于构建产物。不要直接编辑 `out/build/*/_deps/mosh_upstream-src`；应把修改维护为 `patches/` 中的补丁，以便 clean build 可重现。

## 许可证

本工程和 Mosh 派生代码按 GPL-3.0-or-later 分发。二进制分发必须同时满足 GPL 对应源码要求；默认打包流程因此会生成 source bundle。protobuf 使用 BSD-3-Clause，zlib 使用 zlib License，详情见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
