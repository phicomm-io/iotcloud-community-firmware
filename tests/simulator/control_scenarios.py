#!/usr/bin/env python3
import math
import time
from scenarios import call,wait,record
A='020000010001'
M=[f'02000002000{i}' for i in range(1,8)]
def check_average(status,count,pm25,context):
    assert status.get('valid') is True and status.get('sample_count')==count,(context,status)
    expected={'temperature':22,'humidity':45,'pm25':pm25,'formaldehyde':.05}
    for key,value in expected.items():
        actual=status.get(key)
        assert isinstance(actual,(int,float)) and math.isfinite(actual) and abs(actual-value)<.01,(context,key,actual,value)
def main():
    for m,value in zip(M,[10,20,30,40,50,60,700]):call(m,raw={'sim_sensor':{'values':[22,45,value,.05]}})
    wait(A,'sensors.average.get',lambda s:s['sample_count']==7 and abs(s['pm25']-130)<.01)
    initial=call(A,'sensors.average.get');check_average(initial,7,130,'initial-seven-sensor-mean')
    record('mean-seven-values',status=initial)
    call(A,'linkage.set',{'enabled':1,'mode':0,'fixed_speed':63,'on_enabled':0,'off_enabled':0,'debounce_seconds':0,'min_switch_seconds':0})
    s=wait(A,'status.get',lambda s:s['on'] and s['speed']==63);record('fixed-control',status=s)
    # Linear 10..700 maps to 20..100. Statistics exercise the actual worker,
    # rather than calling a second copy of the aggregation algorithm.
    for statistic,expected in [(0,34),(1,100),(2,20),(3,23),(4,23),(5,20)]:
        call(A,'linkage.set',{'mode':1,'statistic':statistic,'metrics':1,'low_pm':10,'high_pm':700,'min_speed':20,'max_speed':100})
        s=wait(A,'status.get',lambda s:s['on'] and s['speed']==expected,seconds=25)
        record('linear-statistic',statistic=statistic,expected=expected,status=s)
    call(A,'linkage.set',{'mode':2,'statistic':0,'target_pm':35,'kp':20,'ki':.02,'kd':0})
    for _ in range(4):
        time.sleep(5);s=call(A,'status.get');assert s['on'] and 20<=s['speed']<=100;record('pid-bounded',status=s)
    # Invalid humidity is excluded, never silently converted to a zero sensor.
    call(M[-1],raw={'sim_sensor':{'values':[22,101,700,.05]}})
    s=wait(A,'sensors.average.get',lambda s:s['sample_count']==6 and abs(s['pm25']-35)<.01)
    check_average(s,6,35,'invalid-humidity-excluded')
    record('invalid-sensor-excluded',status=s)
    call(M[-1],raw={'sim_sensor':{'values':[22,45,700,.05]}})
    wait(A,'sensors.average.get',lambda s:s['sample_count']==7)
    # Frozen source timestamp exercises 15s freshness expiry while network is live.
    call(M[-1],raw={'sim_sensor':{'enabled':False}})
    s=wait(A,'sensors.average.get',lambda s:s['sample_count']==6 and abs(s['pm25']-35)<.01,seconds=35)
    check_average(s,6,35,'stale-sensor-excluded');record('stale-sensor-excluded',status=s)
    call(M[-1],raw={'sim_sensor':{'enabled':True}})
    wait(A,'sensors.average.get',lambda s:s['sample_count']==7)
    record('control-scenarios-pass');print('control scenarios pass',flush=True)
if __name__=='__main__':
    try:main()
    except Exception as e:record('control-failure',error=str(e));raise
