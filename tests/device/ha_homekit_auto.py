"""Exercise native HA MQTT/HomeKit against an explicitly selected isolated A1.

Run in the Home Assistant container with PROOF_BROKER, PROOF_PORT and PROOF_A1.
The caller must save and restore device settings; this test changes power, mode,
manual speed and the automatic idle policy. Apple pairing is outside this test.
"""
import asyncio,json,tempfile,logging,pathlib,os
logging.basicConfig(level=logging.ERROR)
from homeassistant.core import HomeAssistant
from homeassistant.bootstrap import async_from_config_dict
from homeassistant import loader
from homeassistant.helpers import entity_registry as er
from homeassistant.components.homekit.accessories import HomeDriver,get_accessory
from homeassistant.components.homekit.iidmanager import AccessoryIIDStorage
from homeassistant.components.homekit.type_air_purifiers import AirPurifier
from homeassistant.components.mqtt import async_publish
from homeassistant.const import __version__

async def main():
    config_dir=tempfile.mkdtemp();pathlib.Path(config_dir,'configuration.yaml').write_text('homeassistant: {}\nmqtt: {}\n')
    hass=HomeAssistant(config_dir);proof=[]
    async def wait(fn):
        for _ in range(150):
            if fn():return
            await asyncio.sleep(.1)
        raise AssertionError('HA state did not converge')
    try:
        loader.async_setup(hass)
        await async_from_config_dict({'homeassistant':{},'mqtt':{}},hass)
        await hass.async_start()
        result=await hass.config_entries.flow.async_init('mqtt',context={'source':'user'})
        result=await hass.config_entries.flow.async_configure(result['flow_id'],{'broker':os.environ['PROOF_BROKER'],'port':int(os.environ['PROOF_PORT']),'username':'','password':'','protocol':'3.1.1','other_settings':{'set_client_cert':False,'set_ca_cert':'off','transport':'tcp'}})
        assert result['result'].state.value=='loaded'
        ident=os.environ['PROOF_A1']
        uid='u_'+b'auto_test'.hex()+'_a1_'+ident+'_fan'
        registry=er.async_get(hass)
        def selected():
            direct=registry.async_get_entity_id('fan','mqtt',uid)
            if direct:return direct
            return next((x.entity_id for x in registry.entities.values() if x.domain=='fan' and x.platform=='mqtt' and ident in x.unique_id),None)
        await wait(selected)
        entity=selected()
        state=lambda:hass.states.get(entity)
        await wait(lambda:state() and state().state not in ('unknown','unavailable'))
        assert state().attributes['preset_modes']==['auto','manual']
        async def call(service,**params):
            await hass.services.async_call('fan',service,{'entity_id':entity,**params},blocking=True)
        await call('set_preset_mode',preset_mode='manual');await call('set_percentage',percentage=47)
        await wait(lambda:state().attributes['preset_mode']=='manual' and state().attributes['percentage']==47)
        proof.append('HA MQTT discovery and service command/state round trip')
        iid=AccessoryIIDStorage(hass,'proof');await iid.async_initialize()
        driver=HomeDriver(hass,'proof','Proof','Proof',iid,loop=hass.loop,persist_file=config_dir+'/hap.state',port=51839,address='127.0.0.1')
        accessory=get_accessory(hass,driver,state(),2,{'type':'air_purifier'})
        assert isinstance(accessory,AirPurifier)
        accessory.run();accessory.async_update_state(state())
        assert accessory.char_target_air_purifier_state.value==0
        accessory.set_chars({'TargetAirPurifierState':1})
        await wait(lambda:state().attributes['preset_mode']=='auto' and accessory.char_target_air_purifier_state.value==1)
        proof.append('HomeKit TargetAirPurifierState auto -> real HA preset -> production MQTT worker -> HomeKit readback')
        await call('set_preset_mode',preset_mode='manual')
        await wait(lambda:accessory.char_target_air_purifier_state.value==0 and state().attributes['percentage']==47)
        await call('set_preset_mode',preset_mode='auto')
        await wait(lambda:accessory.char_target_air_purifier_state.value==1)
        shown=accessory.char_speed.value
        accessory.set_chars({'TargetAirPurifierState':0})
        await wait(lambda:state().attributes['preset_mode']=='manual' and state().attributes['percentage']==shown and accessory.char_target_air_purifier_state.value==0)
        proof.append('HomeKit manual target emits displayed percentage and converges to manual mode')
        await call('turn_off');await wait(lambda:state().state=='off' and accessory.char_active.value==0)
        accessory.set_chars({'TargetAirPurifierState':1})
        await wait(lambda:state().attributes['preset_mode']=='auto' and accessory.char_target_air_purifier_state.value==1)
        assert state().state=='off' and accessory.char_active.value==0
        proof.append('HomeKit auto selection while off preserves off and reports auto')
        base='phicomm/v2/u_'+b'auto_test'.hex()+'/a1/'+ident
        await async_publish(hass,base+'/command',json.dumps({'action':'linkage.set','params':{'min_speed':0,'fallback_speed':0,'mode':1,'metrics':1,'low_pm':4000,'high_pm':5000}}),qos=1)
        await asyncio.sleep(1);await call('turn_on')
        await wait(lambda:state().state=='on' and state().attributes['percentage']==0)
        assert accessory.char_active.value==1 and accessory.char_speed.value==1
        proof.append('HA on/zero-speed idle retained; native HomeKit maps speed to 1 and current state to purifying')
        print(json.dumps({'home_assistant':__version__,'cases':proof,'apple_pairing':'NOT_RUN'},indent=2),flush=True)
        pathlib.Path(os.environ.get('PROOF_OUTPUT','/proof/result.json')).write_text(json.dumps({'home_assistant':__version__,'cases':proof,'apple_pairing':'NOT_RUN'},indent=2))
        await accessory.stop()
    finally:await hass.async_stop()
asyncio.run(main())
