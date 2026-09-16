# PowerTCP Simulations

## Long-haul baseline

The independent `longhaul-convergence` example compares DCQCN (`CC_MODE=1`),
HPCC (`CC_MODE=3`) and TIMELY (`CC_MODE=7`) on the fixed 5 ms one-way DCI
topology. Validate and run the smoke matrix from the `ns-3.39` tree with:

```bash
python3 examples/PowerTCP/validate_longhaul_topology.py
python3 examples/PowerTCP/run-longhaul-baseline.py
python3 examples/PowerTCP/analyze-longhaul.py
python3 examples/PowerTCP/plot-longhaul.py
```

The runner defaults to S0 and S2 with one `RngRun` per algorithm. Use
`--full` for S0--S5 and five runs, or pass `--scenarios`, `--algorithms`, and
`--runs` explicitly. Each run directory contains the config snapshot,
`sender-rate.csv`, `receiver-goodput.csv`, `dci-link.csv`, `fct.csv`,
`pfc.csv`, metadata, and the stdout diagnostic log.

To avoid any directory conflicts, first open `config.sh` and set the value of `NS3` to the FULL path to the current directory. Both Incast and Fairness simulations take only about a few minutes. So you can play around easily. Documentation on workload simulations coming soon.

## Incast

- **Simulations:** Run `./script-burst.sh yes no` in your terminal to launch one-shot simulations for PowerTCP, Theta-PowerTCP, HPCC, TIMELY and DCQCN in a 10:1 incast scenario. The simulation data is written to `dump_burst/` folder in this directory. 
- **Parse results:** Just run `./results-burst.sh` and the results are written to `results_burst/` folder. 
- **Generate plots:** Run `python3 plot-burst.py`. You will need to open `plot-burst.py` and change some paths at the top of the script. Note: script, results, plot. This is the order to run the scripts.

## Fairness

- **Simulations:** Run `./script-fairness.sh` in your terminal to launch fairness test for PowerTCP, Theta-PowerTCP, HPCC, TIMELY and DCQCN. The simulation data is written to `dump_fairness/`. 
- **Parse results:** Run `./results-fairness.sh` and the results can be found in `results_fairness/`. 
- **Generate plots:** Run `python3 plot-fairness.py` to generate figures (pdf and png).


## Workload

- **Simulations:** Run `./script-workload.sh` in your terminal to launch workload tests for PowerTCP, Theta-PowerTCP, HPCC, TIMELY and DCQCN. The simulation data is written to `dump_workload/`. The script launches simulations for the following cases:
	- varying loads; no incast traffic
	- 80% load, 2MB request size and varying request rates
	- 80% load, 4/second request rate and varying request sizes 
- **Parse results:** Run `./results-workload.sh` and the results can be found in `results_workload/`. 
- **Generate plots:** Run `python3 plot-workload.py` to generate figures (pdf and png).

**IMPORTANT to note:** These simulations take a very long time. The scripts are written to launch **38** simulations in parallel. Please adjust this number based on your cpu. Below are the lines to change (they appear multiple times in the script. Pay attention!).

```bash
while [[ $(ps aux|grep "powertcp-evaluation-workload-optimized"|wc -l) -gt 38 ]];do
	echo "Waiting for cpu cores.... $N-th experiment "
	sleep 60
done
```
