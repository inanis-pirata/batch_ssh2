# batchssh2

基于 [libssh2](https://libssh2.org/) 的跨平台（Windows / Linux）命令行**批量 SSH 运维工具**：对大量设备并发执行命令、上传、下载，并把结果以纯文本 / CSV / JSON 输出。面向工业现场（AGV、物联网设备等）"只开 22 端口、IP 与凭证固定"的批量运维场景。

## 功能一览

| 功能 | 说明 |
|---|---|
| 批量执行命令 `-c` | 单行命令，或 `@script.sh` 本地脚本经 `bash -s` 远端执行 |
| 批量上传 `--upload` | 本地文件 / 整目录递归推送到远端路径 |
| 批量下载 `--download` | 远端文件 / 整目录拉取，自动按 IP 建子目录防覆盖 |
| `@file` 语法糖 | `-H @iplist.txt`、`-c @script.sh`、`-p @pwd.txt` |
| 认证兼容 | password 失败自动回退 keyboard-interactive（兼容 PAM） |
| 公钥认证 | `-i <keyfile>` 使用 SSH 私钥认证 |
| 主机密钥验证 | `--known-hosts <file>` 校验主机密钥 |
| 配置文件 | `--config <file>` 从文件读取连接类默认参数；`-H` / 动作参数必须命令行给 |
| 调度控制 | `-F` 并发线程数、`-T` 单设备总超时 |
| 预演模式 | `--dry-run` 只打印操作不实际连接 |
| 静默模式 | `--quiet` 抑制所有 stderr 输出 |
| 优雅退出 | Ctrl+C 完成当前任务后输出已完成结果 |
| 输出 | 默认文本 / `--csv`（RFC 4180，Windows 下带 BOM）/ `--json` |

## 目录结构

```
batchssh2/
├── CMakeLists.txt       # 构建定义（含 libssh2 FetchContent 兜底）
├── CMakePresets.json    # windows / linux 两套 preset
├── tests/               # 单元测试 / 端到端脚本
└── src/
    ├── main.c           # CLI 解析、任务编排、退出码
    ├── ssh_worker.c/.h  # libssh2 连接、认证回退、exec、SFTP 传输
    ├── pool.c/.h        # 线程池（Win32 线程 / pthreads 自适应）
    ├── output.c/.h      # 文本 / CSV / JSON 输出
    └── util.c/.h        # 动态缓冲、@file 读取、CRLF 转换、目录遍历等
```

## 编译

只依赖 CMake，没有任何构建脚本。libssh2 的获取顺序：系统 CMake config → pkg-config → `LIBSSH2_ROOT` 指向的预编译库 → 都没有时由 CMake 自动下载 1.11.1 源码并静态编译（Windows 用 WinCNG，无外部加密依赖；Linux 用 OpenSSL）。

### Windows

```powershell
winget install Kitware.CMake          # 只需一次，装完重开终端让 PATH 生效

cmake --preset windows                # 首次会下载并编译 libssh2
cmake --build --preset windows
# 产物: build\windows\Release\batchssh2.exe
```

### Linux

```bash
cmake --preset linux
cmake --build --preset linux -j$(nproc)
# 产物: build/linux/batchssh2
```

WSL 里同样用 `cmake --preset linux`。源码树放在 Windows 盘上时（如 `/mnt/d/...`）两个平台共用同一份源码，所以 preset 把产物分别放进 `build/windows` 和 `build/linux`，互不覆盖，切换平台不用删目录。

不装系统 libssh2（`apt install libssh2-1-dev`）时，需要 OpenSSL 头文件才能让 CMake 自行编译 libssh2：
`apt install libssl-dev zlib1g-dev`（Debian/Ubuntu）。

### 不用 preset 的等价裸命令

```bash
# Linux / WSL
cmake -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j$(nproc)

# Windows
cmake -B build/windows -G "Visual Studio 17 2022" -A x64
cmake --build build/windows --config Release
```

已有预编译 libssh2 时用 `-DLIBSSH2_ROOT=<含 include/ 与 lib/ 的前缀>` 指过去；想清理就删掉 `build/`。


## 使用

```bash
# 批量执行命令
batchssh2 -H 192.168.1.10,192.168.1.11 -c "uptime"

# 主机列表文件 + 脚本文件 + CSV 报表（重定向到文件用 Excel 打开）
batchssh2 -H @iplist.txt -p @pwd.txt -c @check.sh --csv > report.csv

# 密码走环境变量，避免 ps 抓到明文
export BATCHSSH2_PWD='xxx'
batchssh2 -H @iplist.txt -c "df -h"

# 上传固件到所有设备
batchssh2 -H @iplist.txt --upload ./app.bin:/opt/app.bin

# 下载所有设备的日志（自动存到 ./logs/<ip>/syslog，互不覆盖）
batchssh2 -H @iplist.txt --download /var/log/syslog:./logs/

# JSON 输出对接其它系统（此模式自动关闭进度条）
batchssh2 -H @iplist.txt -c "hostname" --json | jq .

# 公钥认证 + 主机密钥验证
batchssh2 -H @iplist.txt -i ~/.ssh/id_rsa --known-hosts ~/.ssh/known_hosts -c "uptime"

# 预演模式（不实际连接，只打印将要执行的操作）
batchssh2 -H @iplist.txt -c "reboot" --dry-run

# 静默模式（适合 cron，所有进度/警告都抑制）
batchssh2 -H @iplist.txt -c "df -h" --quiet

# 从配置文件读取连接类默认参数（命令行可覆盖）
# 可配置 key: user / password / port / identity / known_hosts / forks / timeout
# 注意：-H 与动作（-c / --upload / --download）不能缺省，必须命令行给
batchssh2 --config batchssh2.conf -H @iplist.txt -c "uptime"
```

`iplist.txt` 格式（空行 / `#` 注释自动过滤，自动去重）：

```
# AGV 车队
192.168.1.10
192.168.1.11   # 支持行内注释
```

## 输出与流分离约定

- **STDOUT**：只有业务数据（文本 / CSV / JSON），可放心 `>` 重定向。
- **STDERR**：进度（`[3/50] 192.168.1.12: success ...`）、报错、`-v` 调试日志。
- CSV 遵循 RFC 4180 转义；Windows 构建自动加 UTF-8 BOM，Excel 双击中文不乱码，Linux 构建不加 BOM，不影响 `grep/cut`。
- JSON 字段：`host / status / exit_code / time_cost_ms / stdout / stderr / error_msg`，`status ∈ success|failed|timeout`。

## 退出码

| 码 | 含义 |
|---|---|
| 0 | 全部成功 |
| 1 | 部分成功 |
| 2 | 全部失败 |
| 3 | 参数错误 / 本地文件不存在 |

## 设计要点（踩坑记录）

1. **双重认证回退**：先 `libssh2_userauth_password`，失败立即改走 `libssh2_userauth_keyboard_interactive`，回调中自动应答密码——兼容开了 PAM 的现代 Linux（只允许 kbd-interactive 的服务器）。
2. **CRLF 归一**：`-c @script.sh` 在发送前把 `\r\n` 全部转成 `\n`，否则远端 bash 会报 `$'\r': command not found`。
3. **并发模型**：固定线程池（`-F`，默认 10）+ 阻塞 socket，避免瞬间 TCP 握手打爆现场 Wi-Fi/AP。
4. **三重超时**：TCP 非阻塞 connect + select 计时、`libssh2_session_set_timeout` 兜底、业务层 deadline——单台假死设备只会自己超时，绝不拖垮线程池，更不会让进程崩溃。
5. **exec 防死锁**：脚本通过 stdin 灌入时，写 stdin 与读 stdout/stderr 交替进行（非阻塞 channel + `wait_socket`），大脚本 + 高输出不会互相顶死通道窗口。
6. **配置文件的边界**：`--config` 只接受连接/运行类默认值（`user`/`password`/`port`/`identity`/`known_hosts`/`forks`/`timeout`）。目标（`-H`）与动作（`-c`/`--upload`/`--download`）刻意**不做**默认值——它们定义的是「这一次要干什么」，写成默认值会让下次调用的行为变得不可预测，所以必须每次命令行给。

## 安全说明

- 推荐使用终端交互式输入密码（默认行为，类似 `ssh`），安全且不留痕迹。
- 也支持 `-p @pwd.txt` 从文件读取或通过环境变量 `BATCHSSH2_PWD` 传入，适合脚本场景。
- `-p <明文>` 直接在命令行传密码会被 `ps` 看到，不推荐。
- 主机密钥验证通过 `--known-hosts <file>` 启用。

## 测试

本项目带多类测试：

```bash
# 1. 单元测试（纯函数，无需 SSH 服务器）
cd tests/unit
cc -I../../src -o test_util test_util.c ../../src/util.c && ./test_util
#    覆盖：trim、CRLF 转换、sbuf 缓冲、strlist 排序去重、路径操作、文件读取

# 2. Linux 端到端：需要本机 sshd 与一对测试账号
sh tests/run_e2e.sh build/linux/batchssh2 myuser mypass
#    覆盖：命令执行、CRLF 脚本、JSON/CSV、文件/目录上传下载、按 IP 分目录、
#          超时检测、错误密码、混合成败退出码、环境变量/@file 密码

# 3. Windows 端到端：需要本机 OpenSSH Server 服务
powershell -ExecutionPolicy Bypass -File tests\run_e2e_windows.ps1 -Binary build\windows\Release\batchssh2.exe -User myuser -Password mypass

# 4. keyboard-interactive 回退专项：起临时 sshd(2222 端口, 禁用 password 认证)
sh tests/kbd_test.sh build/linux/batchssh2         # 需要 sudo
```
