"""Native HA MQTT CO2 integration proof on an isolated broker and M1 simulator."""
import asyncio,json,tempfile,logging,pathlib,os
logging.basicConfig(level=logging.ERROR)
from homeassistant.core import HomeAssistant
from homeassistant.bootstrap import async_from_config_dict
from homeassistant import loader
from homeassistant.helpers import entity_registry as er
from homeassistant.components.mqtt import async_publish
from homeassistant.const import __version__

async def main():
    config_dir=tempfile.mkdtemp();pathlib.Path(config_dir,'configuration.yaml').write_text('homeassistant: {}\nmqtt: {}\n')
    hass=HomeAssistant(config_dir)
    async def wait(fn,seconds=20):
        for _ in range(seconds*10):
            if fn():return
            await asyncio.sleep(.1)
        raise AssertionError('HA CO2 state did not converge')
    try:
        loader.async_setup(hass)
        await async_from_config_dict({'homeassistant':{},'mqtt':{}},hass);await hass.async_start()
        result=await hass.config_entries.flow.async_init('mqtt',context={'source':'user'})
        result=await hass.config_entries.flow.async_configure(result['flow_id'],{'broker':'127.0.0.1','port':int(os.environ['PROOF_PORT']),'username':'','password':'','protocol':'3.1.1','other_settings':{'set_client_cert':False,'set_ca_cert':'off','transport':'tcp'}})
        assert result['result'].state.value=='loaded'
        uid='u_'+b'co2_test'.hex()+'_m1_aabbccddeeff_co2';registry=er.async_get(hass)
        def entity():return registry.async_get_entity_id('sensor','mqtt',uid)
        await wait(entity)
        def state():return hass.states.get(entity())
        await wait(lambda:state() and state().state=='723')
        assert state().attributes['unit_of_measurement']=='ppm'
        assert state().attributes['device_class']=='carbon_dioxide'
        assert state().attributes['state_class']=='measurement'
        pathlib.Path('/proof/disable').touch()
        await wait(lambda:state().state=='unavailable',30)
        pathlib.Path('/proof/enable').touch()
        await wait(lambda:state().state=='723')
        await async_publish(hass,'homeassistant/status','online',qos=1)
        await asyncio.sleep(1)
        assert state().state=='723'
        proof={'home_assistant':__version__,'entity_id':entity(),'state':state().state,'attributes':dict(state().attributes),'cases':['native discovery','numeric ppm state','carbon_dioxide device class','measurement state class','stale unavailable','fresh recovery','HA birth rediscovery'],'hardware':'synthetic sensor, production MQTT worker'}
        pathlib.Path('/proof/result.json').write_text(json.dumps(proof,indent=2));print(json.dumps(proof),flush=True)
    finally:await hass.async_stop()
asyncio.run(main())
