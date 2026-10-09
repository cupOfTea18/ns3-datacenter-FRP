#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location("analysis",Path(__file__).with_name("analyze-longhaul.py"))
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)

class AnalysisTests(unittest.TestCase):
    def flow(self,i,edges):
        return {"id":i,"data_path":edges,"ack_path":[[v,u,c] for u,v,c in edges],"ack_wire_bytes":0,"ack_interval_bytes":1}
    def test_r4_event_boundaries_and_generations(self):
        def event(t,ordinal,kind,amount,admitted,sent,generation=1):
            return dict(time_ns=t,node=81,event_order=ordinal,event=kind,role="B",flow=1,
                        generation=generation,bytes=amount,in_total=admitted,tx_total=sent,queue_bytes=admitted-sent)
        index=a.r4_packet_index([event(10,1,"ADMIT",100,100,0),event(10,2,"TX",40,100,40),
                                event(20,3,"ADMIT",20,120,40),event(10,4,"ADMIT",7,7,0,2)])
        key=(81,"B",1,1)
        self.assertEqual(a.r4_endpoint(index,key,9)["queue_bytes"],0)
        self.assertEqual(a.r4_endpoint(index,key,10,1)["queue_bytes"],100)
        self.assertEqual(a.r4_endpoint(index,key,10)["queue_bytes"],60)
        self.assertEqual(a.r4_endpoint(index,key,19)["queue_bytes"],60)
        self.assertEqual(a.r4_endpoint(index,key,20)["queue_bytes"],80)
        self.assertEqual(a.r4_endpoint(index,(81,"B",1,2),10)["queue_bytes"],7)
        with self.assertRaises(ValueError):
            a.r4_packet_index([event(10,1,"TX",100,0,100)])
    def test_shared_path(self):
        flows=[self.flow(1,[[0,1,100],[1,2,20]]),self.flow(2,[[0,1,100],[1,3,100]])]
        self.assertEqual(a.path_reference(flows,1000,0),{1:20,2:80})
    def test_directions_independent(self):
        flows=[self.flow(1,[[0,1,100]]),self.flow(2,[[1,0,100]])]
        self.assertEqual(a.path_reference(flows,1000,0),{1:100,2:100})
    def test_wire_overhead(self):
        self.assertAlmostEqual(a.path_reference([self.flow(1,[[0,1,100]])],1000,100)[1],100/1.1)
    def test_missing_path_is_not_equal_share(self):
        self.assertIsNone(a.path_reference([{"id":1}],1000,48))
    def test_statuses(self):
        self.assertEqual(a.settling([],0,.01,100,.03,.001)[0],"insufficient_window")
        self.assertEqual(a.settling([(0,100),(.1,100)],0,.1,100,.03,.001)[0],"insufficient_samples")
        samples=[(i*.001,20) for i in range(101)]
        self.assertEqual(a.settling(samples,0,.1,100,.03,.001)[0],"not_converged")
        self.assertEqual(a.settling([(t,100) for t,_ in samples],0,.1,100,.03,.001)[0],"converged")
    def test_summary_keeps_failures(self):
        rows=[{"sender_status":"converged","sender_settling_10_ms":2},
              {"sender_status":"not_converged","sender_settling_10_ms":None},
              {"sender_status":"insufficient_window","sender_settling_10_ms":None}]
        result=a.convergence_summary(rows,"sender")
        self.assertEqual(result["sender_not_converged_fraction"],.5)
        self.assertEqual(result["sender_insufficient_window_stages"],1)
    def test_peak_not_tail(self):
        rows=[{"time_ns":1000000,"tx_bps":100,"queue_bytes":900},{"time_ns":100000000,"tx_bps":0,"queue_bytes":0}]
        self.assertEqual(a.window_metrics(rows,0,.1,100)["queue_sample_max_bytes"],900)
        self.assertEqual(a.window_metrics(rows,.09,.1,100)["queue_sample_max_bytes"],0)

if __name__=="__main__":unittest.main()
