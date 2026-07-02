#!/bin/bash
# 批量运行5种算法的 workload 仿真
# 用法:
#   ./run_all_algos_workload.sh                     # 使用默认参数 (load=0.2, duration=0.02)
#   ./run_all_algos_workload.sh --load 0.3 --duration 0.05
#   ./run_all_algos_workload.sh --enableFlowFileBackground 0

echo "=========================================="
echo "批量 workload 仿真: DCQCN, HPCC, TIMELY, FRP, ROCC"
echo "=========================================="

declare -A ALGOS
ALGOS=( [1]="DCQCN" [3]="HPCC" [7]="TIMELY" [13]="FRP" [14]="ROCC" )

# 透传给 run_single_workload_algo.py 的额外参数
EXTRA_ARGS="$@"

for ccMode in "${!ALGOS[@]}"; do
    algo=${ALGOS[$ccMode]}
    echo ""
    echo ">>> 运行 $algo (ccMode=$ccMode)..."
    python3 run_single_workload_algo.py $ccMode $algo $EXTRA_ARGS
done

echo ""
echo "=========================================="
echo "全部完成！"
echo "=========================================="
ls -lh /home/shemuping/newCode/ns3-FRP/results/workload/workload_*.png