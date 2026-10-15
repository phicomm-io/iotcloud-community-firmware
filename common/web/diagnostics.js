const diagnosticLabels={wifi_trial_status:'Wi-Fi 试连状态',wifi_trial_active:'正在验证 Wi-Fi',uptime_seconds:'运行时间（秒）',free_memory:'可用内存（字节）',child_lock:'童锁',led_enabled:'启用指示灯',socket_names:'插座名称',socket_states:'插座输出',time_synced:'时钟已校准',utc_time:'设备时间（UTC 秒）',fan_on:'风机开启',speed:'风速',panel_ready:'面板通信正常',panel_errors:'面板通信错误数',version:'固件版本',device_id:'设备 ID',name:'设备名称',ssid:'网络名称',ip:'IP 地址',wifi_connected:'Wi-Fi 已连接',ap_active:'配网热点已开启',ap_name:'热点名称',mqtt_connected:'MQTT 已连接',mqtt_status:'MQTT 状态',mqtt_host:'Broker 地址',mqtt_port:'MQTT 端口',mqtt_user:'MQTT 用户名',mqtt_topic_mode:'主题模式',mqtt_errors:'MQTT 错误次数',mqtt_last_error:'最近 MQTT 错误',mqtt_cleanup_warning:'旧 HA 实体清理异常',sensor_valid:'传感器数据有效',sensor_received:'已收到传感器数据',age_ms:'数据距今',temperature:'温度（℃）',humidity:'湿度（%）',formaldehyde:'甲醛（mg/m³）',pm25:'PM2.5（µg/m³）',uart_ready:'串口已就绪',rx_frames:'接收帧数',unknown_frames:'未知帧数',uart_errors:'串口错误数',discarded_bytes:'丢弃字节数',last_frame:'最近串口帧（HEX）',brightness_requested:'请求亮度档位',ota_active:'正在升级',ota_state:'升级状态',ota_error:'升级错误',ota_progress:'升级进度（%）',scan_busy:'正在扫描 Wi-Fi',web_password_set:'已设置网页密码',mqtt_password_set:'已设置 MQTT 密码',ntp_host:'首选 NTP 服务器',ntp_server:'最近尝试的校时源',ntp_status:'校时状态',ntp_synced:'本次启动已完成网络校时',ntp_screen_queued:'最近屏幕时间已入队',ntp_last_success:'最近成功校时（UTC）',ntp_next_seconds:'下次尝试距今（秒）',timezone_minutes:'时区（UTC 分钟差）',available:'状态可用',peers:'绑定设备',mac:'MAC 地址',ip_address:'IP 地址',pending:'等待远端响应',confirmed:'已确认绑定',remote_result:'远端操作结果',message:'说明',code:'状态码',revision:'配置版本',online:'在线',transport:'通信方式',last_error:'最近错误',lan_ok:'局域网正常',mqtt_ok:'MQTT 正常'};
const diagnosticStates={ready:'已就绪',disconnected:'未连接',connecting:'连接中',synced:'校时成功',syncing:'正在校时',waiting_network:'等待网络',failed:'校时失败',screen_queue_failed:'网络校时成功，屏幕入队失败',idle:'空闲',pending:'等待验证',success:'验证成功并已保存',storage_failure:'保存失败，已回滚',timeout_rollback:'连接超时，已回滚',legacy:'原主题',account:'社区平台'};
function diagnosticValue(key,value){
 if(value===null||value===undefined)return '暂无数据';
 if(typeof value==='boolean')return value?'是':'否';
 if(key==='ntp_last_success')return new Date(value*1000).toISOString().replace('T',' ').replace('.000Z',' UTC');
 if(key==='age_ms')return (value/1000).toFixed(1)+' 秒';
 if(value==='')return '—';
 return diagnosticStates[value]||String(value);
}
function renderDiagnosticTree(value,parent){
 if(value&&typeof value==='object'){
  if(!Object.keys(value).length){parent.textContent=Array.isArray(value)?'暂无记录':'暂无数据';return;}
  const dl=document.createElement('dl');
  Object.entries(value).forEach(([key,v])=>{const dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=Array.isArray(value)?'第 '+(Number(key)+1)+' 项':diagnosticLabels[key]||key;if(v&&typeof v==='object'){dd.className='diagnostic-tree';renderDiagnosticTree(v,dd);}else{dd.textContent=diagnosticValue(key,v);if(typeof v==='boolean')dd.className=v?'good':'';}dl.append(dt,dd);});parent.append(dl);
 }else parent.textContent=diagnosticValue('',value);
}
function renderCommonDiagnostics(s,extraGroups=[],note=''){
 const groups=[['设备',['name','device_id','version','uptime_seconds','free_memory','web_password_set']],['网络连接',['wifi_connected','ssid','ip','ap_active','ap_name','scan_busy','wifi_password_set','wifi_trial_status','wifi_trial_active']],['MQTT',['mqtt_connected','mqtt_status','mqtt_host','mqtt_port','mqtt_user','mqtt_topic_mode','mqtt_password_set','mqtt_errors','mqtt_last_error','mqtt_cleanup_warning']],['固件升级',['ota_active','ota_state','ota_progress','ota_error']],...extraGroups];
 const container=$('diagnosticGroups');container.replaceChildren();const seen=new Set();
 for(const [title,keys] of groups){const values={};for(const key of keys){seen.add(key);if(Object.hasOwn(s,key))values[key]=s[key];}if(!Object.keys(values).length)continue;const box=document.createElement('div'),h=document.createElement('h3');box.className='diagnostic-group';h.textContent=title;box.append(h);renderDiagnosticTree(values,box);container.append(box);}
 const extra=Object.fromEntries(Object.entries(s).filter(([key])=>!seen.has(key)));if(Object.keys(extra).length){const box=document.createElement('div'),h=document.createElement('h3');box.className='diagnostic-group';h.textContent='其他状态';box.append(h);renderDiagnosticTree(extra,box);container.append(box);}
 $('diagnostics').textContent=JSON.stringify(s,null,2);if($('diagnosticUpdated'))$('diagnosticUpdated').textContent='最近读取：'+new Date().toLocaleTimeString()+(note?'；'+note:'');
}
