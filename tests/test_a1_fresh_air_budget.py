"""Measure fresh-air status admission at two soft floors using ARM/newlib.

The allocator ledger is synthetic: it cannot prove closed Wi-Fi SDK headroom.
"""
import unittest
from tests.test_status_direct import fixture
from tests import test_http_json_budget as budget_tests


class FreshAirBudget(unittest.TestCase):
    def test_status_floor_comparison(self):
        for floor in (16384, 12288):
            with self.subTest(floor=floor):
                text = fixture()
                text = text.replace('16384', str(floor))
                # Preserve the existing graph/wire oracle for ordinary fields;
                # include the additive accessory fields from its current graph.
                text = text.replace('!strcmp(key,"sensors")?value:',
                    '(!strcmp(key,"sensors")||!strncmp(key,"fresh_air_",10))?value:')
                text = text.replace('void a1_status_capture(',
                    'void fresh_snapshot(fresh_snapshot_t *p){memset(p,0,sizeof(*p));p->requested_on=true;p->requested_speed=100;p->automatic=true;p->ready=true;p->target=100;p->applied=100;p->primary=100;p->duty=100;p->pulses=123456;p->frequency_millihz=123456;}\nvoid a1_status_capture(', 1)
                text = text.replace('budget=23000;budget<=29000;budget+=2000',
                    'budget=17000;budget<=25000;budget+=1000')
                text = text.replace('assert(x.response.status==200&&p.wire);if(post)',
                    'if(budget>=23000)assert(x.response.status==200);if(x.response.status!=200){ph_http_product_release(&p);healthy_stats();printf("fresh floor='+str(floor)+' capacity=%u post=%u BUSY\\n",budget,post);continue;}if(post)')
                text = text.replace('states();http_and_memory();', 'http_and_memory();')
                budget_tests.JsonBudget().compile_arm('A1', '#define A1_FRESH_AIR 1\n'+text, budget=True)
