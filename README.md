# zquant-system

这是一个低延迟交易系统项目，包含交易所方和市场参与方，并提供构建、运行和性能分析的相关脚本。
项目学习实践来源：https://github.com/PacktPublishing/Building-Low-Latency-Applications-with-CPP

## 目录结构

- `quant-system/scripts`: 包含构建、运行相关的脚本
- `quant-system/notebooks`: 包含性能分析的脚本
- `quant-system/common`: 公共组件代码
- `quant-system/exchange`: 交易所相关代码
- `quant-system/trading`: 交易相关代码
- `quant-system/benchmarks`: 性能基准测试代码

## 脚本使用说明

### 1. 构建脚本

#### `build.sh`
- 功能：构建Release和Debug版本的项目，会先清理之前的构建产物
- 使用方法：
```bash
cd quant-system
bash scripts/build.sh
```
- 说明：该脚本会创建`cmake-build-release`和`cmake-build-debug`目录，分别用于存放Release和Debug版本的构建结果，使用CMake或Ninja作为构建工具，并行使用4个进程进行构建。

#### `no_clean_build.sh`
- 功能：构建Release和Debug版本的项目，不清理之前的构建产物，构建速度更快
- 使用方法：
```bash
cd quant-system
bash scripts/no_clean_build.sh
```

### 2. 运行脚本

#### `run_benchmarks.sh`
- 功能：运行性能基准测试，包括日志器、内存池和哈希表的性能测试
- 使用方法：
```bash
cd quant-system
bash scripts/run_benchmarks.sh
```
- 输出：会分别显示原始和优化后的日志器、内存池的时钟周期数，以及数组哈希表和无序映射哈希表的时钟周期数。

#### `run_clients.sh`
- 功能：启动交易客户端，支持不同类型的交易算法（MAKER、TAKER、RANDOM）
- 使用方法：
```bash
cd quant-system
bash scripts/run_clients.sh
```
- 说明：默认启动ID为1的MAKER客户端和ID为5的RANDOM客户端，其他客户端处于注释状态。可根据需要修改脚本中的参数或取消注释来启动更多客户端。客户端参数包括成交量阈值、价格阈值、最大订单量、最大持仓和最大亏损等。

#### `run_exchange_and_clients.sh`
- 功能：依次启动交易所和交易客户端，最后停止交易所
- 使用方法：
```bash
cd quant-system
bash scripts/run_exchange_and_clients.sh
```
- 说明：该脚本会先构建项目，然后启动交易所，等待10秒后运行客户端脚本，客户端运行完成后停止交易所。

## 性能分析脚本使用说明

### `perf_analysis.py`
- 功能：处理和分析性能日志文件，可视化系统延迟，支持RDTSC（特定局部操作的持续时间）和TTT（系统关键路径中两个不同事件之间的延迟）两种类型的延迟数据。
- 依赖：需要安装`pandas`、`plotly`等Python库
- 使用方法：
```bash
cd quant-system/notebooks
python3 perf_analysis.py ../exchange*.log "../*_1.log" --cpu-freq 2.60
```
- 参数说明：
  - `log_patterns`: 一个或多个日志文件模式（如`../exchange*.log`）
  - `--cpu-freq`: CPU频率（单位为GHz），用于将RDTSC周期转换为纳秒，默认值为2.60
- 输出：会为每个RDTSC标签和预定义的TTT路径生成并显示延迟图表，图表中包含原始数据和滚动平均值，同时在控制台输出每个标签或路径的观测值数量和平均延迟。

## 备注

- 脚本中的路径和配置可能需要根据实际系统环境进行调整
- 运行前请确保已安装必要的依赖库和工具（如CMake、Ninja、g++、Python相关库等）
- 性能测试结果会受到硬件环境、系统负载等因素的影响