#!/usr/bin/env python3
"""Audit finite R5/baseline runs: completion, wire closure and actual group token expenditure."""
import argparse
import csv
import hashlib
import importlib.util
import json
from collections import defaultdict
from pathlib import Path


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def verify(path):
    runner=json.loads((path/'runner-metadata.json').read_text())
    assert runner['status']=='ok', 'simulation failed'
    for name,key in (('config.txt','config_sha256'),):
        assert hashlib.sha256((path/name).read_bytes()).hexdigest()==runner[key], 'effective config changed'
    summary=json.loads((path/'metadata.json.summary.json').read_text())
    assert summary['expected_flows']==summary['completed_flows']==len(summary['flows']), 'incomplete flows'
    for f in summary['flows']:
        assert f['unique_sent_bytes']==f['rx_payload_bytes']==f['size_bytes'], 'unique TX/RX mismatch'
    for key in ('admission_drop_packets','remaining_switch_queue_bytes','remaining_mmu_bytes','remaining_mmu_egress_bytes'):
        assert summary[key]==0, key
    result={'run':str(path),'algorithm':runner['algorithm'],'flows':summary['flows'],
            'drops':0,'remaining_queue_bytes':0,'remaining_mmu_bytes':0}
    if runner['algorithm']!='proposed':
        return result
    final=json.loads((path/'r5.final.json').read_text())['proposed_parameters']
    assert final['version']==5
    flows={(g['node'],f['role'],f['id']):f for g in final['gateways'] for f in g['flows']}
    for (node,role,fid),f in flows.items():
        assert f['in_total']==f['tx_total']>0, 'flow queue did not drain'
        if role=='A': assert f['tx_total']==flows[(f['peer'],'B',fid)]['in_total'], 'WAN wire did not close'
    total_sends=0
    groups=[]
    for gateway in final['gateways']:
        node=gateway['node']; assert gateway['rejected']==0, 'unexpected feedback rejection'
        source={g['id']:g for g in gateway['groups'] if g['role']=='A'}
        groups.extend(gateway['groups'])
        if not source: continue
        events=[]
        for r in rows(path/f'r5.gateway-{node}.events.csv'):
            if r['event']=='CONTROL': events.append((int(r['time_ns']),int(r['event_order']),'control',int(r['id']),float(r['value'])))
        for r in rows(path/f'r5.gateway-{node}.group-packets.csv'):
            if r['event']=='TX' and r['role']=='A': events.append((int(r['time_ns']),int(r['event_order']),'tx',int(r['group']),int(r['bytes'])))
        buckets={gid:[0,0.,0.] for gid in source}  # last time, tokens, rate
        for time,ordinal,kind,gid,value in sorted(events):
            last,tokens,rate=buckets[gid]
            tokens=min(source[gid]['frame_bytes'],tokens+rate*(time-last)*1e-9)
            if kind=='control':
                rate=value
                if rate==0: tokens=0.
            else:
                assert tokens+1e-4>=value, f'actual TX exceeds shared budget: {node}/{gid} at {time}'
                tokens=max(0.,tokens-value); total_sends+=1
            buckets[gid]=[time,tokens,rate]
        by_port=defaultdict(lambda:[0.,0.])
        for r in rows(path/f'r5.gateway-{node}.groups.csv'):
            if r['role']!='A': continue
            entry=by_port[(r['time_ns'],r['port'])]; entry[0]+=float(r['target_Bps']); entry[1]=float(r['capacity_Bps'])
        assert all(rate<=capacity+1e-4 for rate,capacity in by_port.values()), 'port over-allocation'
    spec=importlib.util.spec_from_file_location('longhaul_analysis',Path(__file__).with_name('analyze-longhaul.py'))
    analysis=importlib.util.module_from_spec(spec); spec.loader.exec_module(analysis)
    meta=json.loads((path/'metadata.json').read_text())
    evidence=analysis.r5_evidence(path,{},meta)
    assert evidence and all(r['state_sample_matches_events'] is not False for r in evidence), 'snapshot boundary mismatch'
    assert all(r['forward_mapping_mismatches']==0 for r in evidence), 'actual arrival time/bytes mismatch'
    errors=[r['prediction_error_bytes'] for r in evidence if r['prediction_error_bytes'] is not None]
    result.update(wire_closed=True,group_token_audited_packets=total_sends,groups=groups,
                  valid_prediction_samples=len(errors),prediction_abs_error_max_bytes=max(map(abs,errors),default=None),
                  near_cnps=sum(f['near_cnps'] for f in flows.values()),native_cnps=sum(f['native_cnps'] for f in flows.values()))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('--root',type=Path,required=True)
    args=parser.parse_args(); results=[]; failures=[]
    for p in sorted(args.root.rglob('runner-metadata.json')):
        try: results.append(verify(p.parent))
        except (AssertionError,KeyError,ValueError,OSError) as error: failures.append({'run':str(p.parent),'error':str(error)})
    out={'passed_runs':len(results),'failures':failures,'results':results}
    (args.root/'r5-validation.json').write_text(json.dumps(out,indent=2)+'\n')
    print(json.dumps({'passed_runs':len(results),'failures':failures},indent=2))
    return bool(failures) or not results


if __name__=='__main__': raise SystemExit(main())
