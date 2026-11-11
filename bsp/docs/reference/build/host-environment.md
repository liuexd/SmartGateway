# D01 · Ubuntu BSP 构建环境及 Git 工作区检查

- 记录来源：2026-10-09 开发者在 Ubuntu VMware 虚拟机执行的实际终端命令及输出。
- 状态：**基础构建环境存在，尚未实际编译外部 .ko 模块，也未验证与板端内核的完全 ABI 一致。**

## 1. 宿主机目录与组件

| 路径（相对于 ~/Desktop） | 观测结果 |
| --- | --- |
| `Linux-4.9.88/` | 存在内核源码与先前构建产物 |
| `Linux-4.9.88/.config` | 存在 |
| `Linux-4.9.88/include/generated/utsrelease.h` | `UTS_RELEASE "4.9.88"` |
| `Linux-4.9.88/Module.symvers` | 存在，约 525K |
| `Linux-4.9.88/include/generated/autoconf.h` | 存在，约 47K |
| `Linux-4.9.88/scripts/mod/modpost` | 存在，可执行文件，约 118K |
| `ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/` | 编译器查询为 `Linaro GCC 6.2.1 20161016` |
| `ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/` | 存在，但未验证详细 ABI |
| `Uboot-2017.03/`、`Uboot-2018.03/`、`Buildroot_2020.02.x/` | 目录存在，尚未验证精确补丁/配置对应关系 |

内核 `.config` 实测：

```text
CONFIG_MODULES=y
CONFIG_MODULE_UNLOAD=y
CONFIG_MODVERSIONS=y
```

内核源码的 `.git` 是指向 `../.repo/projects/Linux-4.9.88.git` 的符号链接。宿主 `~/Desktop/.repo` 存在；`git -C ~/Desktop/Linux-4.9.88 status --short` 未报告工作区改动。

## 2. SmartGateway-BSP 本地独立克隆（已验证）

- 本地目录：`~/Desktop/project/SmartGateway-BSP/`
- 远端：`https://github.com/liuexd/SmartGateway.git`
- 开发分支：`feature/bsp-imx6ull`
- 当时检出的提交：`90e2d37`（初次 BSP D01 基线提交）
- `git status`：`working tree clean`；与当时的 `origin/feature/bsp-imx6ull` 一致。
- 同级目录 `~/Desktop/project/SmartGateway/` 为原项目另一份独立本地克隆。

**注意**：两个本地目录是同一个 GitHub 仓库的独立克隆，各自有工作区；BSP 分支也会包含仓库已有的 `common/`、`gateway/`、`server/` 等目录。不要为了“只显示 BSP”而删除 Git 正在跟踪的源文件。

## 3. Git 基础操作学习

```sh
cd ~/Desktop/project/SmartGateway-BSP
git fetch origin
git branch --show-current
git log -1 --oneline
git status
```

在远端继续提交后，如本地干净且没有尚未推送的独立提交，可在该分支上执行 `git pull --ff-only` 同步新增文档。这里记录的 `90e2d37` 是用户克隆时的状态，不代表此后永远不变。

## 4. D02 允许的安全工作

1. 在本地 `bsp/drivers/hello/` 编写最小 Linux 模块源码和 Kbuild Makefile。
2. 用已有内核构建树及 Linaro GCC 在**Ubuntu 宿主机**交叉编译。
3. 使用 `file`、`modinfo`（若已安装）核对产物架构、`vermagic` 等。
4. 恢复方案尚未核对时，**不对当前板上 eMMC 进行任何写入或刷机操作**；首次 `insmod` 留待模块兼容性和安全条件确认后执行。
