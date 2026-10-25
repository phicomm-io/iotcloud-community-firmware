/*********************************************************************************
 *  MIT License
 *  
 *  Copyright (c) 2020-2024 Gregg E. Berman
 *  
 *  https://github.com/HomeSpan/HomeSpan
 *  
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *  
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *  
 ********************************************************************************/

#include "version.h"
 
#include <ESPmDNS.h>
#include <nvs_flash.h>
#include <sodium.h>
#include <WiFi.h>
#include <driver/ledc.h>
#include <mbedtls/version.h>
#include <mbedtls/sha256.h>
#include <esp_task_wdt.h>
#include <esp_sntp.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>

#include "HomeSpan.h"
#include "HAP.h"
#include "IoTCloudValues.h"
#include <algorithm>

const __attribute__((section(".rodata_custom_desc"))) SpanPartition spanPartition = {HOMESPAN_MAGIC_COOKIE,0};

using namespace Utils;

HapOut hapOut;                      // Specialized output stream that can both print to serial monitor and encrypt/transmit to HAP Clients with minimal memory usage (global-scoped variable)
Span homeSpan;                      // HAP Attributes database and all related control functions for this Accessory (global-scoped variable)
HapCharacteristics hapChars;        // Instantiation of all HAP Characteristics used to create SpanCharacteristics (global-scoped variable)

///////////////////////////////
//         Span              //
///////////////////////////////

// IoTCloud owns Wi-Fi, OTA and recovery. Never touch Flash from a global
// constructor or initialize/erase the application's default NVS partition.
Span::Span(){}

bool Span::iotcloudStorage(const char *partition){
  if(charNVS || srpNVS || hapNVS)return false;
  nvs_handle *handles[]={&charNVS,&srpNVS,&hapNVS};
  const char *names[]={"CHAR","SRP","HAP"};
  for(unsigned i=0;i<3;i++){
    if(nvs_open_from_partition(partition,names[i],NVS_READWRITE,handles[i])!=ESP_OK){
      for(unsigned j=0;j<=i;j++){if(*handles[j])nvs_close(*handles[j]);*handles[j]=0;}
      iotcloudNVS.fail();
      return false;
    }
  }
  iotcloudNVS.opened();
  return true;
}

bool Span::iotcloudInitialize(){
  if(!iotcloudStorageHealthy())return false;
  if(!isInitialized)isInitialized=HAPClient::init();
  if(!isInitialized){iotcloudNVS.fail();iotcloudStop();}
  return iotcloudReady();
}

void Span::iotcloudStop(){
  if(hapServer)hapServer->end();
  for(auto &c:hapList)c.client.stop();
  if(connected)mdns_service_remove("_hap","_tcp");
  connected=0;
}

void Span::begin(Category catID, const char *_displayName, const char *_hostNameBase, const char *_modelName){
  loopTaskHandle=xTaskGetCurrentTaskHandle();
  asprintf(&displayName,"%s",_displayName);
  asprintf(&hostNameBase,"%s",_hostNameBase);
  asprintf(&modelName,"%s",_modelName);
  asprintf(&hostName,"%s%s",_hostNameBase,hostNameSuffix?hostNameSuffix:"");
  sprintf(category,"%d",(int)catID);
  statusLED=new Blinker(nullptr,0);
  hapServer=new WiFiServer(tcpPortNum);
}

void Span::poll() {

  if(pollTaskHandle){
    LOG0("\n** FATAL ERROR: Do not call homeSpan.poll() directly if homeSpan.autoPoll() is used!\n** PROGRAM HALTED **\n\n");
    vTaskDelete(pollTaskHandle);
    while(1);    
  }
  
  pollTask();
}

///////////////////////////////

void Span::pollTask(){
  if(!iotcloudInitialize()){iotcloudStop();return;}
  checkConnect();
  if(WiFi.status()!=WL_CONNECTED)return;
  if(hapServer->hasClient()){
    auto incoming=hapServer->available();
    // Bound sockets and leave memory for authenticated HTTP, MQTT and OTA.
    // INTERNAL alone also counts instruction RAM unsuitable for byte buffers.
    const auto caps=MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const bool paired=HAPClient::nAdminControllers()>0;
    const size_t freeMinimum=paired?32768:40960;
    const size_t blockMinimum=paired?8192:20480;
    if(hapList.size()>=4 || heap_caps_get_free_size(caps)<freeMinimum ||
        heap_caps_get_largest_free_block(caps)<blockMinimum){
      incoming.stop();
    }else{
      auto it=hapList.emplace(hapList.begin());
      it->request.allocate=it->encrypted.allocate=[](size_t n){
        const auto caps=MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        return heap_caps_get_free_size(caps)>n+16384 &&
            heap_caps_get_largest_free_block(caps)>n;
      };
      it->client=incoming;
      it->client.setTimeout(1000);
      it->clientNumber=it->client.fd()-LWIP_SOCKET_OFFSET;
      it->inputWindow.opened(millis());
    }
  }
  snapTime=millis();
  for(auto svc:Loops)svc->loop();
  currentClient=hapList.begin();
  while(currentClient!=hapList.end()){
    if(currentClient->client.connected()){
      currentClient->processRequest();
      if(!iotcloudStorageHealthy()){iotcloudStop();return;}
      ++currentClient;
    }else{
      clearNotify(&*currentClient);
      currentClient=hapList.erase(currentClient);
    }
  }
  HAPClient::checkNotifications();
  HAPClient::checkTimedWrites();
}

void Span::commandMode(){} // Provisioning and reset use authenticated IoTCloud APIs.

void Span::checkConnect(){
  if(WiFi.status()!=WL_CONNECTED){
    if(connected){
      hapServer->end();
      for(auto &c:hapList)c.client.stop();
      connected=0;
    }
    return;
  }
  if(connected)return;
  connected=1;
  // loop() has already started the shared mDNS responder and HTTP service.
  char id[18];                              // create string version of Accessory ID for MDNS broadcast
  memcpy(id,HAPClient::accessory.ID,17);    // copy ID bytes
  id[17]='\0';                              // add terminating null

  MDNS.setInstanceName(displayName);            // set server display name
  MDNS.addService("_hap","_tcp",tcpPortNum);    // advertise HAP service on specified port

  // add MDNS (Bonjour) TXT records for configurable as well as fixed values (HAP Table 6-7)

  char cNum[16];
  sprintf(cNum,"%d",hapConfig.configNumber);
  
  mdns_service_txt_item_set("_hap","_tcp","c#",cNum);            // Accessory Current Configuration Number (updated whenever config of HAP Accessory Attribute Database is updated)
  mdns_service_txt_item_set("_hap","_tcp","md",modelName);       // Accessory Model Name
  mdns_service_txt_item_set("_hap","_tcp","ci",category);        // Accessory Category (HAP Section 13.1)
  mdns_service_txt_item_set("_hap","_tcp","id",id);              // string version of Accessory ID in form XX:XX:XX:XX:XX:XX (HAP Section 5.4)

  mdns_service_txt_item_set("_hap","_tcp","ff","0");             // HAP Pairing Feature flags.  MUST be "0" to specify Pair Setup method (HAP Table 5-3) without MiFi Authentification
  mdns_service_txt_item_set("_hap","_tcp","pv","1.1");           // HAP version - MUST be set to "1.1" (HAP Section 6.6.3)
  mdns_service_txt_item_set("_hap","_tcp","s#","1");             // HAP current state - MUST be set to "1"

  if(!HAPClient::nAdminControllers())                            // Accessory is not yet paired
    mdns_service_txt_item_set("_hap","_tcp","sf","1");           // set Status Flag = 1 (Table 6-8)
  else
    mdns_service_txt_item_set("_hap","_tcp","sf","0");           // set Status Flag = 0

  uint8_t hashInput[22];
  uint8_t hashOutput[64];
  char setupHash[9];
  size_t len;
  
  memcpy(hashInput,qrID,4);                                           // Create the Setup ID for use with optional QR Codes.  This is an undocumented feature of HAP R2!
  memcpy(hashInput+4,id,17);                                          // Step 1: Concatenate 4-character Setup ID and 17-character Accessory ID into hashInput
  mbedtls_sha512_ret(hashInput,21,hashOutput,0);                      // Step 2: Perform SHA-512 hash on combined 21-byte hashInput to create 64-byte hashOutput
  mbedtls_base64_encode((uint8_t *)setupHash,9,&len,hashOutput,4);    // Step 3: Encode the first 4 bytes of hashOutput in base64, which results in an 8-character, null-terminated, setupHash
  mdns_service_txt_item_set("_hap","_tcp","sh",setupHash);            // Step 4: broadcast the resulting Setup Hash

  hapServer->begin();

  LOG0("\n");

  if(!HAPClient::nAdminControllers())
    LOG0("DEVICE NOT YET PAIRED -- PLEASE PAIR WITH HOMEKIT APP\n\n");
  
  if(wifiCallback)
    wifiCallback();

  if(wifiCallbackAll)
    wifiCallbackAll((connected+1)/2);
  
} // initWiFi

///////////////////////////////

Span& Span::setQRID(const char *id){
  
  char tBuf[5]={};
  sscanf(id,"%4[0-9A-Za-z]",tBuf);
  
  if(strlen(id)==4 && strlen(tBuf)==4){
    sprintf(qrID,"%s",id);
  }

  return(*this);
} // setQRID

///////////////////////////////

void Span::processSerialCommand(const char *c){
  if(c && c[0]=='S')setPairingCode(c+1,false);
}

void Span::getWebLog(void (*f)(const char *, void *), void *user_data){
  HAPClient::getStatusURL(NULL,f,user_data);
}

///////////////////////////////

void Span::resetStatus(){
  if(strlen(network.wifiData.ssid)==0)
    STATUS_UPDATE(start(LED_WIFI_NEEDED),HS_WIFI_NEEDED)
  else if(WiFi.status()!=WL_CONNECTED)
    STATUS_UPDATE(start(LED_WIFI_CONNECTING),HS_WIFI_CONNECTING)
  else if(!HAPClient::nAdminControllers())
    STATUS_UPDATE(start(LED_PAIRING_NEEDED),HS_PAIRING_NEEDED)
  else
    STATUS_UPDATE(on(),HS_PAIRED)
}

///////////////////////////////

void Span::reboot(){
  STATUS_UPDATE(off(),HS_REBOOTING)
  delay(1000);
  ESP.restart();  
}

///////////////////////////////

const char* Span::statusString(HS_STATUS s){
  switch(s){
    case HS_WIFI_NEEDED: return("WiFi Credentials Needed");
    case HS_WIFI_CONNECTING: return("WiFi Connecting");
    case HS_PAIRING_NEEDED: return("Device not yet Paired");
    case HS_PAIRED: return("Device Paired");
    case HS_ENTERING_CONFIG_MODE: return("Entering Command Mode");
    case HS_CONFIG_MODE_EXIT: return("1. Exit Command Mode"); 
    case HS_CONFIG_MODE_REBOOT: return("2. Reboot Device");
    case HS_CONFIG_MODE_LAUNCH_AP: return("3. Launch Access Point");
    case HS_CONFIG_MODE_UNPAIR: return("4. Unpair Device");
    case HS_CONFIG_MODE_ERASE_WIFI: return("5. Erase WiFi Credentials");
    case HS_CONFIG_MODE_EXIT_SELECTED: return("Exiting Command Mode...");
    case HS_CONFIG_MODE_REBOOT_SELECTED: return("Rebooting Device...");
    case HS_CONFIG_MODE_LAUNCH_AP_SELECTED: return("Launching Access Point...");
    case HS_CONFIG_MODE_UNPAIR_SELECTED: return("Unpairing Device...");
    case HS_CONFIG_MODE_ERASE_WIFI_SELECTED: return("Erasing WiFi Credentials...");
    case HS_REBOOTING: return("REBOOTING!");
    case HS_FACTORY_RESET: return("Performing Factory Reset...");
    case HS_AP_STARTED: return("Access Point Started");
    case HS_AP_CONNECTED: return("Access Point Connected");
    case HS_AP_TERMINATED: return("Access Point Terminated");
    case HS_OTA_STARTED: return("OTA Update Started");
    default: return("Unknown");
  }
}

///////////////////////////////

Span& Span::setPairingCode(const char *s, boolean progCall){
   
  char setupCode[10]={};

  sscanf(s," %9[0-9]",setupCode);

  if(strlen(setupCode)!=8){
    LOG0("\n*** Invalid request to change Setup Code to '%s'.  Code must be exactly 8 digits.\n\n",s);
    if(progCall){
      LOG0("=== PROGRAM HALTED ===");
      while(1);
    }
    return(*this);
  }   

  if(!network.allowedCode(setupCode)){
    LOG0("\n*** Invalid request to change Setup Code to '%s'.  Code too simple.\n\n",s);
    if(progCall){
      LOG0("=== PROGRAM HALTED ===");
      while(1);
    }
    return(*this);
  }

  TempBuffer<Verification> verifyData;       // temporary storage for verification data
  SRP6A *srp=new SRP6A;                      // create temporary instance of SRP

  if(!progCall)
    LOG0("\nGenerating new SRP verification data for Setup Code: %.3s-%.2s-%.3s ... ",setupCode,setupCode+3,setupCode+5);

  if(!srp->createVerifyCode(setupCode,verifyData)){iotcloudNVS.fail();delete srp;return *this;}
  const bool saved=iotcloudNVS.writeBlob(srpNVS,"VERIFYDATA",verifyData,verifyData.len());
  
  if(!progCall && saved)
    LOG0("New Code Saved!\nSetup Payload for Optional QR Code: %s\n\n",qrCode.get(atoi(setupCode),qrID,atoi(category)));

  delete srp;
  return(*this);
}

///////////////////////////////

Span& Span::setWifiCredentials(const char *ssid, const char *pwd){
  sprintf(network.wifiData.ssid,"%.*s",MAX_SSID,ssid);
  sprintf(network.wifiData.pwd,"%.*s",MAX_PWD,pwd);


  return(*this);
}

///////////////////////////////

void Span::printfAttributes(int flags){

  hapOut << "{\"accessories\":[";

  for(int i=0;i<Accessories.size();i++){
    Accessories[i]->printfAttributes(flags);    
    if(i+1<Accessories.size())
      hapOut << "," ;
  }
    
  hapOut << "]}";
}

///////////////////////////////

boolean Span::deleteAccessory(uint32_t n){
  
  auto it=homeSpan.Accessories.begin();
  for(;it!=homeSpan.Accessories.end() && (*it)->aid!=n; it++);
  
  if(it==homeSpan.Accessories.end())
    return(false);

  delete *it;
  return(true);
}

///////////////////////////////

SpanCharacteristic *Span::find(uint32_t aid, uint32_t iid){

  int index=-1;
  for(int i=0;i<Accessories.size();i++){   // loop over all Accessories to find aid
    if(Accessories[i]->aid==aid){          // if match, save index into Accessories array
      index=i;
      break;
    }
  }

  if(index<0)                  // fail if no match on aid
    return(NULL);
    
  for(int i=0;i<Accessories[index]->Services.size();i++){                           // loop over all Services in this Accessory
    for(int j=0;j<Accessories[index]->Services[i]->Characteristics.size();j++){     // loop over all Characteristics in this Service
      
      if(iid == Accessories[index]->Services[i]->Characteristics[j]->iid)           // if matching iid
        return(Accessories[index]->Services[i]->Characteristics[j]);                // return pointer to Characteristic
    }
  }

  return(NULL);                // fail if no match on iid
}

///////////////////////////////

int Span::countCharacteristics(char *buf){

  int nObj=0;
  
  const char tag[]="\"aid\"";
  while((buf=strstr(buf,tag))){         // count number of characteristic objects in PUT JSON request
    nObj++;
    buf+=strlen(tag);
  }

  return(nObj);
}

///////////////////////////////

int Span::updateCharacteristics(char *buf, SpanBuf *pObj, int capacity){

  if(capacity<1 || capacity>32)return 0;

  int nObj=0;
  char *p1;
  int cFound=0;
  boolean twFail=false;
  
  while(char *t1=strtok_r(buf,"{",&p1)){           // parse 'buf' and extract objects into 'pObj' unless NULL
   buf=NULL;
    char *p2;
    int okay=0;
    
    while(char *t2=strtok_r(t1,"}[]:, \"\t\n\r",&p2)){

      if(!cFound){                                 // first token found
        if(strcmp(t2,"characteristics")){
          LOG0("\n*** ERROR:  Problems parsing JSON - initial \"characteristics\" tag not found\n\n");
          return(0);
        }
        cFound=1;
        break;
      }
      
      t1=NULL;
      if(nObj>=capacity)return 0;
      char *t3;
      if(!strcmp(t2,"aid") && (t3=strtok_r(t1,"}[]:, \"\t\n\r",&p2))){
        uint64_t id;
        if((okay&1) || !iotcloud::homeParseUnsigned(t3,1,UINT32_MAX,id))return 0;
        pObj[nObj].aid=uint32_t(id);
        okay|=1;
      } else 
      if(!strcmp(t2,"iid") && (t3=strtok_r(t1,"}[]:, \"\t\n\r",&p2))){
        uint64_t id;
        if((okay&2) || !iotcloud::homeParseUnsigned(t3,1,UINT32_MAX,id))return 0;
        pObj[nObj].iid=uint32_t(id);
        okay|=2;
      } else 
      if(!strcmp(t2,"value") && (t3=strtok_r(t1,"}[]:,",&p2))){
        if(okay&4)return 0;
        // Preserve quotes so a JSON string cannot be coerced into a number.
        pObj[nObj].val=t3;
        okay|=4;
      } else 
      if(!strcmp(t2,"ev") && (t3=strtok_r(t1,"}[]:, \"\t\n\r",&p2))){
        if(okay&8)return 0;
        pObj[nObj].ev=t3;
        okay|=8;
      } else 
      if(!strcmp(t2,"r") && (t3=strtok_r(t1,"}[]:, \"\t\n\r",&p2))){
        if((okay&16) || !iotcloud::homeParseBool(t3,pObj[nObj].wr))return 0;
        okay|=16;
      } else 
      if(!strcmp(t2,"pid") && (t3=strtok_r(t1,"}[]:, \"\t\n\r",&p2))){        
        uint64_t pid;
        if((okay&32) || !iotcloud::homeParseUnsigned(t3,1,UINT64_MAX,pid))return 0;
        okay|=32;
        if(!TimedWrites.count(pid)){
          LOG0("\n*** ERROR:  Timed Write PID not found\n\n");
          twFail=true;
        } else        
        if(millis()>TimedWrites[pid]){
          LOG0("\n*** ERROR:  Timed Write Expired\n\n");
          twFail=true;
        }        
      } else {
        LOG0("\n*** ERROR:  Problems parsing JSON characteristics object - unexpected property \"%s\"\n\n",t2);
        return(0);
      }
    } // parse property tokens

    if(!t1){                                                                  // at least one token was found that was not initial "characteristics"
      if((okay&15)==7 || (okay&15)==11 || (okay&15)==15){                      // all required properties found
        if(!pObj[nObj].val)                                                   // if value is NOT being updated
          pObj[nObj].wr=false;                                                // ignore any request for write-response
        nObj++;                                                               // increment number of characteristic objects found        
      } else {
        LOG0("\n*** ERROR:  Problems parsing JSON characteristics object - missing required properties\n\n");
        return(0);
      }
    }
      
  } // parse objects

  if(nObj!=capacity)return 0;
  // Each characteristic owns one pending value. Reject aliases before PASS 1
  // so a second row cannot replace or revive a previously rejected write.
  for(int i=0;i<nObj;++i)
    for(int j=0;j<i;++j)
      if(pObj[i].aid==pObj[j].aid && pObj[i].iid==pObj[j].iid)return 0;

  snapTime=millis();                                           // timestamp for this series of updates, assigned to each characteristic in loadUpdate()

  for(int i=0;i<nObj;i++){                                     // PASS 1: loop over all objects, identify characteristics, and initialize update for those found

    if(twFail){                                                // this is a timed-write request that has either expired or for which there was no PID
      pObj[i].status=StatusCode::InvalidValue;                 // set error for all characteristics      
      
    } else {
      pObj[i].characteristic = find(pObj[i].aid,pObj[i].iid);  // find characteristic with matching aid/iid and store pointer          

      if(pObj[i].characteristic)                                                                 // if found, initialize characterstic update with new val/ev
        pObj[i].status=pObj[i].characteristic->loadUpdate(pObj[i].val,pObj[i].ev,pObj[i].wr);    // save status code, which is either an error, or TBD (in which case updateFlag for the characteristic has been set to either 1 or 2) 
      else
        pObj[i].status=StatusCode::UnknownResource;                                              // if not found, set HAP error            
    }
      
  } // first pass
      
  for(int i=0;i<nObj;i++){                                     // PASS 2: loop again over all objects       
    if(pObj[i].status==StatusCode::TBD){                       // if object status still TBD

      StatusCode status=pObj[i].characteristic->service->update()?StatusCode::OK:StatusCode::Unable;        // update service and save statusCode as OK or Unable depending on whether return is true or false

      for(int j=i;j<nObj;j++){                                                                              // loop over this object plus any remaining objects to update values and save status for any other characteristics in this service
        
        if(pObj[j].status==StatusCode::TBD && pObj[j].characteristic && pObj[j].characteristic->service==pObj[i].characteristic->service){ // only validated pending values participate in this service update
          pObj[j].status=status;                                                                            // save statusCode for this object
          LOG1("Updating aid=%u iid=%u",pObj[j].characteristic->aid,pObj[j].characteristic->iid);
          if(status==StatusCode::OK){                                                                       // if status is okay
            pObj[j].characteristic->uvSet(pObj[j].characteristic->value,pObj[j].characteristic->newValue);  // update characteristic value with new value
            if(pObj[j].characteristic->nvsKey){                                                             // if storage key found
              if(pObj[j].characteristic->format<FORMAT::STRING)
                nvs_set_u64(charNVS,pObj[j].characteristic->nvsKey,pObj[j].characteristic->value.UINT64);   // store data as uint64_t regardless of actual type (it will be read correctly when access through uvGet())         
              else
                nvs_set_str(charNVS,pObj[j].characteristic->nvsKey,pObj[j].characteristic->value.STRING);   // store data
              nvs_commit(charNVS);
            }
            LOG1(" (okay)\n");
          } else {                                                                                          // if status not okay
            pObj[j].characteristic->uvSet(pObj[j].characteristic->newValue,pObj[j].characteristic->value);  // replace characteristic new value with original value
            LOG1(" (failed)\n");
          }
          pObj[j].characteristic->updateFlag=0;                                                             // reset updateFlag for characteristic
        }
      }

    } // object had TBD status
  } // loop over all objects
      
  return(1);
}

///////////////////////////////

void Span::clearNotify(HAPClient *hc){

  for(auto const &acc : Accessories)
    for(auto const &svc : acc->Services)
      for(auto const &chr : svc->Characteristics)
        chr->evList.remove(hc);  
} 

///////////////////////////////

void Span::printfNotify(SpanBuf *pObj, int nObj, HAPClient *hc){

  boolean notifyFlag=false;
  
  for(int i=0;i<nObj;i++){                                       // loop over all objects
    
    if(pObj[i].status==StatusCode::OK && pObj[i].val){           // characteristic was successfully updated with a new value (i.e. not just an EV request)
      if(pObj[i].characteristic->evList.has(hc)){                // if connection hc is subscribed to EV notifications for this characteristic
      
        if(!notifyFlag)                                          // this is first notification for any characteristic
          hapOut << "{\"characteristics\":[";                    // print start of JSON array
        else                                                     // else already printed at least one other characteristic
          hapOut << ",";                                         // add preceeding comma before printing this characteristic
        
        pObj[i].characteristic->printfAttributes(GET_VALUE|GET_AID|GET_NV);    // print JSON attributes for this characteristic
        notifyFlag=true;        
      }
    }
  }

  if(notifyFlag)
    hapOut << "]}";
}

///////////////////////////////

void Span::printfAttributes(SpanBuf *pObj, int nObj){

  hapOut << "{\"characteristics\":[";

  for(int i=0;i<nObj;i++){
    hapOut << "{\"aid\":" << pObj[i].aid << ",\"iid\":" << pObj[i].iid << ",\"status\":" << (int)pObj[i].status;
    if(pObj[i].status==StatusCode::OK && pObj[i].wr && pObj[i].characteristic)
      hapOut << ",\"value\":" << pObj[i].characteristic->uvPrint(pObj[i].characteristic->value).c_str();
    hapOut << "}";
    if(i+1<nObj)
      hapOut << ",";
  }

  hapOut << "]}";
}

///////////////////////////////

boolean Span::printfAttributes(char **ids, int numIDs, int flags){

  uint32_t aid;
  uint32_t iid;
  
  SpanCharacteristic *Characteristics[numIDs];
  StatusCode status[numIDs];

  for(int i=0;i<numIDs;i++){              // PASS 1: loop over all ids requested to check status codes - only errors are if characteristic not found, or not readable
    aid=iid=0;
    sscanf(ids[i],"%u.%u",&aid,&iid);     // parse aid and iid
    Characteristics[i]=find(aid,iid);     // find matching chararacteristic
    
    if(Characteristics[i]){                                         // if found
      if(Characteristics[i]->perms&PERMS::PR){                      // if permissions allow reading
        status[i]=Characteristics[i]->iotcloudAvailable?StatusCode::OK:StatusCode::Unable;
        if(status[i]!=StatusCode::OK){Characteristics[i]=NULL;flags|=GET_STATUS;}
      } else {
        Characteristics[i]=NULL;                                    // set to NULL to trigger not-found in Pass 2 below                                     
        status[i]=StatusCode::WriteOnly;
        flags|=GET_STATUS;                                          // update flags to require status attribute for all characteristics
      }
    } else {
      status[i]=StatusCode::UnknownResource;
      flags|=GET_STATUS;                                            // update flags to require status attribute for all characteristics
    }
  }

  hapOut << "{\"characteristics\":[";

  for(int i=0;i<numIDs;i++){              // PASS 2: loop over all ids requested and create JSON for each (either all with, or all without, a status attribute based on final flags setting)
    
    if(Characteristics[i])                                          // if found
      Characteristics[i]->printfAttributes(flags);                  // get JSON attributes for characteristic (may or may not include status=0 attribute)
    else{                                                           // else create JSON status attribute based on requested aid/iid
      aid=iid=0;
    sscanf(ids[i],"%u.%u",&aid,&iid);                             
      hapOut << "{\"iid\":" << iid << ",\"aid\":" << aid << ",\"status\":" << (int)status[i] << "}";     
    }
      
    if(i+1<numIDs)
      hapOut << ",";    
  }

  hapOut << "]}";

  return(flags&GET_STATUS);    
}

///////////////////////////////

Span& Span::resetIID(uint32_t newIID){

  if(Accessories.empty()){
    LOG0("\nFATAL ERROR!  Can't reset the Accessory IID count without a defined Accessory ***\n");
    LOG0("\n=== PROGRAM HALTED ===");
    while(1);
  }
  
  if(newIID<1){
    LOG0("\nFATAL ERROR!  Request to reset the Accessory IID count to 0 not allowed (IID must be 1 or greater) ***\n");
    LOG0("\n=== PROGRAM HALTED ===");
    while(1);    
  }
  
  Accessories.back()->iidCount=newIID-1;
  return(*this);
}

///////////////////////////////

boolean Span::updateDatabase(boolean updateMDNS){

  printfAttributes(GET_META|GET_PERMS|GET_TYPE|GET_DESC);   // stream attributes database, which automtically produces a SHA-384 hash
  hapOut.flush();  

  boolean changed=false;

  if(memcmp(hapOut.getHash(),hapConfig.hashCode,48)){       // if hash code of current HAP database does not match stored hash code
    memcpy(hapConfig.hashCode,hapOut.getHash(),48);         // update stored hash code
    hapConfig.configNumber++;                               // increment configuration number
    if(hapConfig.configNumber==65536)                       // reached max value
      hapConfig.configNumber=1;                             // reset to 1
                   
    if(!iotcloudNVS.writeBlob(hapNVS,"HAPHASH",&hapConfig,sizeof(hapConfig)))return false;
    changed=true;

    if(updateMDNS){
      char cNum[16];
      sprintf(cNum,"%d",hapConfig.configNumber);
      mdns_service_txt_item_set("_hap","_tcp","c#",cNum);      
    }
  }

  Loops.clear();

  for(auto acc=Accessories.begin(); acc!=Accessories.end(); acc++){                        // identify all services with over-ridden loop() methods
    for(auto svc=(*acc)->Services.begin(); svc!=(*acc)->Services.end(); svc++){
      if((void(*)())((*svc)->*(&SpanService::loop)) != (void(*)())(&SpanService::loop))    // save pointers to services in Loops vector
        homeSpan.Loops.push_back((*svc));
    }
  }    

  return(changed);
}

///////////////////////////////

list<Controller, Mallocator<Controller>>::const_iterator Span::controllerListBegin(){
  return(HAPClient::controllerList.cbegin());
}

///////////////////////////////

list<Controller, Mallocator<Controller>>::const_iterator Span::controllerListEnd(){
  return(HAPClient::controllerList.cend());
}

///////////////////////////////
//      SpanAccessory        //
///////////////////////////////

SpanAccessory::SpanAccessory(uint32_t aid){

  if(!homeSpan.Accessories.empty()){

    if(homeSpan.Accessories.size()==HAPClient::MAX_ACCESSORIES){
      LOG0("\n\n*** FATAL ERROR: Can't create more than %d Accessories.  Program Halting.\n\n",HAPClient::MAX_ACCESSORIES);
      while(1);      
    }
    
    this->aid=homeSpan.Accessories.back()->aid+1;
                
  } else {
    this->aid=1;
  }
  
  homeSpan.Accessories.push_back(this);

  if(aid>0){                 // override with user-specified aid
    this->aid=aid;
  }

}

///////////////////////////////

SpanAccessory::~SpanAccessory(){

  while(Services.rbegin()!=Services.rend())           // delete all Services in this Accessory
    delete *Services.rbegin();                       
  
  auto acc=homeSpan.Accessories.begin();              // find Accessory in homeSpan vector and erase entry
  while((*acc)!=this)
    acc++;
  homeSpan.Accessories.erase(acc);
  LOG1("Deleted Accessory AID=%d\n",aid);
}

///////////////////////////////

void SpanAccessory::printfAttributes(int flags){

  hapOut << "{\"aid\":" << aid << ",\"services\":[";

  for(int i=0;i<Services.size();i++){
    Services[i]->printfAttributes(flags);    
    if(i+1<Services.size())
      hapOut << ",";
    }
    
  hapOut << "]}";
}

///////////////////////////////
//       SpanService         //
///////////////////////////////

SpanService::SpanService(const char *type, const char *hapName, boolean isCustom){

  if(homeSpan.Accessories.empty()){
    LOG0("\nFATAL ERROR!  Can't create new Service '%s' without a defined Accessory ***\n",hapName);
    LOG0("\n=== PROGRAM HALTED ===");
    while(1);
  }

  this->type=type;
  this->hapName=hapName;
  this->isCustom=isCustom;

  homeSpan.Accessories.back()->Services.push_back(this);  
  accessory=homeSpan.Accessories.back();
  iid=++(homeSpan.Accessories.back()->iidCount);
}

///////////////////////////////

SpanService::~SpanService(){
  
  while(Characteristics.rbegin()!=Characteristics.rend())           // delete all Characteristics in this Service
    delete *Characteristics.rbegin();
      
  auto svc=accessory->Services.begin();              // find Service in containing Accessory vector and erase entry
  while((*svc)!=this)
    svc++;
  accessory->Services.erase(svc);

  for(svc=homeSpan.Loops.begin(); svc!=homeSpan.Loops.end() && (*svc)!=this; svc++);    // search for entry in Loop vector...
  if(svc!=homeSpan.Loops.end()){                                                        // ...if it exists, erase it
    homeSpan.Loops.erase(svc);
    LOG1("Deleted Loop Entry\n");
  }

  auto pb=homeSpan.PushButtons.begin();         // loop through PushButton vector and delete ALL PushButtons associated with this Service
  while(pb!=homeSpan.PushButtons.end()){
    if((*pb)->service==this){
      pb=homeSpan.PushButtons.erase(pb);
      LOG1("Deleted PushButton on Pin=%d\n",(*pb)->pin);
    }
    else {
      pb++;
    }
  }
  
  LOG1("Deleted Service AID=%u IID=%u\n",accessory->aid,iid); 
}

///////////////////////////////

SpanService *SpanService::setPrimary(){
  primary=true;
  return(this);
}

///////////////////////////////

SpanService *SpanService::setHidden(){
  hidden=true;
  return(this);
}

///////////////////////////////

SpanService *SpanService::addLink(SpanService *svc){
  linkedServices.push_back(svc);
  return(this);
}

///////////////////////////////

void SpanService::printfAttributes(int flags){

  hapOut << "{\"iid\":" << iid << ",\"type\":\"" << type << "\",";
  
  if(hidden)
    hapOut << "\"hidden\":true,";
    
  if(primary)
    hapOut << "\"primary\":true,";

  if(!linkedServices.empty()){
    hapOut << "\"linked\":[";
    for(int i=0;i<linkedServices.size();i++){
      hapOut << linkedServices[i]->iid;
      if(i+1<linkedServices.size())
        hapOut << ",";
    }
     hapOut << "],";
  }
    
  hapOut << "\"characteristics\":[";
  
  for(int i=0;i<Characteristics.size();i++){
    Characteristics[i]->printfAttributes(flags);    
    if(i+1<Characteristics.size())
      hapOut << ",";
  }
    
  hapOut << "]}";
}

///////////////////////////////
//    SpanCharacteristic     //
///////////////////////////////

SpanCharacteristic::SpanCharacteristic(HapChar *hapChar, boolean isCustom){

  type=hapChar->type;
  perms=hapChar->perms;
  hapName=hapChar->hapName;
  format=hapChar->format;
  staticRange=hapChar->staticRange;
  this->isCustom=isCustom;
  this->hapChar=hapChar;

  if(homeSpan.Accessories.empty() || homeSpan.Accessories.back()->Services.empty()){
    LOG0("\nFATAL ERROR!  Can't create new Characteristic '%s' without a defined Service ***\n",hapName);
    LOG0("\n=== PROGRAM HALTED ===");
    while(1);
  }

  homeSpan.Accessories.back()->Services.back()->Characteristics.push_back(this);  
  iid=++(homeSpan.Accessories.back()->iidCount);
  service=homeSpan.Accessories.back()->Services.back();
  aid=homeSpan.Accessories.back()->aid;
}

///////////////////////////////

SpanCharacteristic::~SpanCharacteristic(){

  auto chr=service->Characteristics.begin();              // find Characteristic in containing Service vector and erase entry
  while((*chr)!=this)
    chr++;
  service->Characteristics.erase(chr);

  free(desc);
  free(unit);
  free(validValues);
  free(nvsKey);

  if(format>=FORMAT::STRING){
    free(value.STRING);
    free(newValue.STRING);
  }
  
  LOG1("Deleted Characteristic AID=%u IID=%u\n",aid,iid);  
}

///////////////////////////////

String SpanCharacteristic::uvPrint(UVal &u){
  char c[64];
  switch(format){
    case FORMAT::BOOL:
      return(String(u.BOOL));      
    case FORMAT::INT:
      return(String(u.INT));
    case FORMAT::UINT8:
      return(String(u.UINT8));        
    case FORMAT::UINT16:
      return(String(u.UINT16));        
    case FORMAT::UINT32:
      return(String(u.UINT32));        
    case FORMAT::UINT64:
      sprintf(c,"%llu",u.UINT64);
      return(String(c));        
    case FORMAT::FLOAT:
      sprintf(c,"%g",u.FLOAT);
      return(String(c));        
    case FORMAT::STRING:
    case FORMAT::DATA:
    case FORMAT::TLV_ENC:
      return(String("\"") + String(u.STRING) + String("\""));        
  } // switch
  return(String());       // included to prevent compiler warnings
}

///////////////////////////////

void SpanCharacteristic::uvSet(UVal &dest, UVal &src){
  if(format>=FORMAT::STRING)
    uvSet(dest,(const char *)src.STRING);
  else
    dest=src;
}

///////////////////////////////

void SpanCharacteristic::uvSet(UVal &u, STRING_t val){
  u.STRING = (char *)HS_REALLOC(u.STRING, strlen(val) + 1);
  strcpy(u.STRING, val);
}

///////////////////////////////

void SpanCharacteristic::uvSet(UVal &u, DATA_t data){
  
  if(data.second>0){
    size_t olen;
    mbedtls_base64_encode(NULL,0,&olen,NULL,data.second);                              // get length of string buffer needed (mbedtls includes the trailing null in this size)
    value.STRING = (char *)HS_REALLOC(value.STRING,olen);                              // allocate sufficient size for storing value
    mbedtls_base64_encode((uint8_t*)value.STRING,olen,&olen,data.first,data.second );  // encode data into string buf
  } else {
    value.STRING = (char *)HS_REALLOC(value.STRING,1);                                 // allocate sufficient size for just trailing null character
    *value.STRING ='\0';
  }  
}

///////////////////////////////

void SpanCharacteristic::uvSet(UVal &u, TLV_ENC_t tlv){

  const size_t bufSize=36;                                  // maximum size of buffer to store packed TLV bytes before encoding directly into value; must be multiple of 3
  size_t nBytes=tlv.pack_size();                            // total size of packed TLV in bytes

  if(nBytes>0){
    size_t nChars;
    mbedtls_base64_encode(NULL,0,&nChars,NULL,nBytes);      // get length of string buffer needed (mbedtls includes the trailing null in this size)
    u.STRING = (char *)HS_REALLOC(u.STRING,nChars);         // allocate sufficient size for storing value
    TempBuffer<uint8_t> tBuf(bufSize);                      // create fixed-size buffer to store packed TLV bytes
    tlv.pack_init();                                        // initialize TLV packing
    uint8_t *p=(uint8_t *)u.STRING;                         // set pointer to beginning of value
    while((nBytes=tlv.pack(tBuf,bufSize))>0){               // pack the next set of TLV bytes, up to a maximum of bufSize, into tBuf
      size_t olen;                                          // number of characters written (excludes null character)
      mbedtls_base64_encode(p,nChars,&olen,tBuf,nBytes);    // encode data directly into value
      p+=olen;                                              // advance pointer to null character
      nChars-=olen;                                         // subtract number of characters remaining
    }
  } else {
    u.STRING = (char *)HS_REALLOC(u.STRING,1);              // allocate sufficient size for just trailing null character
    *u.STRING ='\0';
  }  
}

///////////////////////////////

char *SpanCharacteristic::getStringGeneric(UVal &val){
  if(format>=FORMAT::STRING)
      return val.STRING;

  return NULL;
}

///////////////////////////////

void SpanCharacteristic::setString(const char *val, boolean notify){ 

  setValCheck();
  uvSet(value,val);
  setValFinish(notify);    
}

///////////////////////////////

size_t SpanCharacteristic::getDataGeneric(uint8_t *data, size_t len, UVal &val){    
  if(format<FORMAT::DATA)
    return(0);

  size_t olen;
  int ret=mbedtls_base64_decode(data,len,&olen,(uint8_t *)val.STRING,strlen(val.STRING));
  
  if(data==NULL)
    return(olen);
    
  if(ret==MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
    LOG0("\n*** WARNING:  Can't decode Characteristic::%s with getData().  Destination buffer is too small (%d out of %d bytes needed)!\n\n",hapName,len,olen);
  else if(ret==MBEDTLS_ERR_BASE64_INVALID_CHARACTER)
    LOG0("\n*** WARNING:  Can't decode Characteristic::%s with getData().  Data is not in base-64 format!\n\n",hapName);
    
  return(olen);
}

///////////////////////////////

void SpanCharacteristic::setData(const uint8_t *data, size_t len, boolean notify){

  setValCheck();
  uvSet(value,{data,len});
  setValFinish(notify);
} 
  
///////////////////////////////

size_t SpanCharacteristic::getTLVGeneric(TLV8 &tlv, UVal &val){
   
  if(format<FORMAT::TLV_ENC)
    return(0);

  const size_t bufSize=36;                    // maximum size of buffer to store decoded bytes before unpacking into TLV; must be multiple of 3
  TempBuffer<uint8_t> tBuf(bufSize);          // create fixed-size buffer to store decoded bytes
  tlv.wipe();                                 // clear TLV completely

  size_t nChars=strlen(val.STRING);         // total characters to decode
  uint8_t *p=(uint8_t *)val.STRING;         // set pointer to beginning of value
  const size_t decodeSize=bufSize/3*4;      // number of characters to decode in each pass
  int status=0;
  
  while(nChars>0){
    size_t olen;
    size_t n=nChars<decodeSize?nChars:decodeSize;
    
    int ret=mbedtls_base64_decode(tBuf,tBuf.len(),&olen,p,n);
    if(ret==MBEDTLS_ERR_BASE64_INVALID_CHARACTER){
      LOG0("\n*** WARNING:  Can't decode Characteristic::%s with getTLV().  Data is not in base-64 format!\n\n",hapName);
      tlv.wipe();
      return(0);
    }
    status=tlv.unpack(tBuf,olen);
    p+=n;
    nChars-=n;
  }
  if(status>0){
    LOG0("\n*** WARNING:  Can't unpack Characteristic::%s with getTLV().  TLV record is incomplete or corrupted!\n\n",hapName);
    tlv.wipe();
    return(0);      
  }
return(tlv.pack_size());
}

///////////////////////////////

void SpanCharacteristic::setTLV(const TLV8 &tlv, boolean notify){

  setValCheck();
  uvSet(value,tlv);
  setValFinish(notify);
}

///////////////////////////////

void SpanCharacteristic::setValCheck(){
  if(updateFlag==1)
    LOG0("\n*** WARNING:  Attempt to set value of Characteristic::%s within update() while it is being simultaneously updated by Home App.  This may cause device to become non-responsive!\n\n",hapName);
}

///////////////////////////////

void SpanCharacteristic::setValFinish(boolean notify){

  uvSet(newValue,value);     
  updateTime=homeSpan.snapTime;

  if(notify){
    if((perms&EV) && (updateFlag!=2)){        // only broadcast notification if EV permission is set AND update is NOT being done in context of write-response    
      SpanBuf sb;                             // create SpanBuf object
      sb.characteristic=this;                 // set characteristic          
      sb.status=StatusCode::OK;               // set status
      char dummy[]="";
      sb.val=dummy;                           // set dummy "val" so that printfNotify knows to consider this "update"
      homeSpan.Notifications.push_back(sb);   // store SpanBuf in Notifications vector
    }

    if(nvsKey){
      nvs_set_str(homeSpan.charNVS,nvsKey,value.STRING);    // store data
      nvs_commit(homeSpan.charNVS);
    }
  }      
}

///////////////////////////////

void SpanCharacteristic::printfAttributes(int flags){

  const char permCodes[][7]={"pr","pw","ev","aa","tw","hd","wr"};
  const char formatCodes[][9]={"bool","uint8","uint16","uint32","uint64","int","float","string","data","tlv8"};

  hapOut << "{\"iid\":" << iid;

  if(flags&GET_TYPE)
    hapOut << ",\"type\":\"" << type << "\"";

  if((perms&PR) && (flags&GET_VALUE)){    
    if(perms&NV && !(flags&GET_NV))
      hapOut << ",\"value\":null";
    else
      hapOut << ",\"value\":" << uvPrint(value).c_str();
  }

  if(flags&GET_META){
    hapOut << ",\"format\":\"" << formatCodes[format] << "\"";
    
    if(customRange && (flags&GET_META)){
      hapOut << ",\"minValue\":" << uvPrint(minValue).c_str() << ",\"maxValue\":" << uvPrint(maxValue).c_str();
        
      if(uvGet<float>(stepValue)>0)
        hapOut << ",\"minStep\":" << uvPrint(stepValue).c_str();
    }

    if(unit){
      if(strlen(unit)>0)
        hapOut << ",\"unit\":\"" << unit << "\"";
     else
        hapOut << ",\"unit\":null";
    }

    if(validValues){
      hapOut << ",\"valid-values\":" << validValues;
    }
  }
    
  if(desc && (flags&GET_DESC)){
    hapOut << ",\"description\":\"" << desc << "\"";
  }

  if(flags&GET_PERMS){
    hapOut << ",\"perms\":[";
    for(int i=0;i<7;i++){
      if(perms&(1<<i)){
        hapOut << "\"" << permCodes[i] <<"\"";
        if(perms>=(1<<(i+1)))
          hapOut << ",";
      }
    }
    hapOut << "]";
  }

  if(flags&GET_AID)
    hapOut << ",\"aid\":" << aid;

  HAPClient *hc=&(*(homeSpan.currentClient));
  
  if(flags&GET_EV)
    hapOut << ",\"ev\":" << (evList.has(hc)?"true":"false");

  if(flags&GET_STATUS)
    hapOut << ",\"status\":0";    

  hapOut << "}";
}

///////////////////////////////

StatusCode SpanCharacteristic::loadUpdate(char *val, char *ev, boolean wr){

  boolean evFlag=false;
  if(ev){                // request for notification
    if(!iotcloud::homeParseBool(ev,evFlag))
      return(StatusCode::InvalidValue);
    
    if(evFlag && !(perms&EV))         // notification is not supported for characteristic
      return(StatusCode::NotifyNotAllowed);
      
  }

  if(val){
    if(!(perms&PW))         // cannot write to read only characteristic
      return(StatusCode::ReadOnly);

    UVal candidate;
    uint64_t unsignedValue;
    switch(format){
    
    case BOOL:
      if(!iotcloud::homeParseBool(val,candidate.BOOL))
        return(StatusCode::InvalidValue);
      break;

    case INT:
      if(!iotcloud::homeParseInt(val,minValue.INT,maxValue.INT,candidate.INT) ||
         !iotcloud::homeValueInList(validValues,candidate.INT))
        return(StatusCode::InvalidValue);
      break;

    case UINT8:
    case UINT16:
    case UINT32:
    case UINT64:
      if(!iotcloud::homeParseUnsigned(val,uvGet<uint64_t>(minValue),uvGet<uint64_t>(maxValue),unsignedValue) ||
         (format!=UINT64 && !iotcloud::homeValueInList(validValues,int64_t(unsignedValue))))
        return(StatusCode::InvalidValue);
      uvSet(candidate,unsignedValue);
      break;

    case FLOAT:
      if(!iotcloud::homeParseFloat(val,minValue.FLOAT,maxValue.FLOAT,candidate.FLOAT))
        return(StatusCode::InvalidValue);
      break;

    case STRING:
    case DATA:
    case TLV_ENC:{
      const char *begin,*end;
      if(!iotcloud::homeValueBounds(val,begin,end) || end-begin<2 || *begin!='"' || end[-1]!='"')
        return(StatusCode::InvalidValue);
      *const_cast<char *>(end-1)='\0';
      uvSet(newValue,(const char *)stripBackslash(const_cast<char *>(begin+1)));
      break;
    }

    default:
      return(StatusCode::InvalidValue);

    } // switch
    if(format<STRING)newValue=candidate;
  }

  // No subscription or pending value changes are made for a rejected row.
  if(ev){
    LOG1("Notification Request for aid=%u iid=%u: %s\n",aid,iid,evFlag?"true":"false");
    HAPClient *hc=&(*(homeSpan.currentClient));
    if(evFlag)evList.add(hc);
    else evList.remove(hc);
  }
  if(!val)return(StatusCode::OK);

  updateFlag=1+wr;                // set flag to 1 if successful update or 2 if successful AND write-response flag is set
  updateTime=homeSpan.snapTime;
  return(StatusCode::TBD);
}

///////////////////////////////

unsigned long SpanCharacteristic::timeVal(){
  
  return(homeSpan.snapTime-updateTime);
}

///////////////////////////////

boolean SpanCharacteristic::updated(){
  
  return(updateFlag>0);
}

///////////////////////////////

uint32_t SpanCharacteristic::getIID(){
  
  return(iid);
}

///////////////////////////////

SpanCharacteristic *SpanCharacteristic::setPerms(uint8_t perms){
  perms&=0x7F;
  if(perms>0)
    this->perms=perms;
  return(this);
}

///////////////////////////////

SpanCharacteristic *SpanCharacteristic::addPerms(uint8_t dPerms){
  return(setPerms(perms|dPerms));
}

///////////////////////////////

SpanCharacteristic *SpanCharacteristic::removePerms(uint8_t dPerms){
  return(setPerms(perms&(~dPerms)));
}

///////////////////////////////

SpanCharacteristic *SpanCharacteristic::setDescription(const char *c){
  desc = (char *)HS_REALLOC(desc, strlen(c) + 1);
  strcpy(desc, c);
  return(this);
}  

///////////////////////////////

SpanCharacteristic *SpanCharacteristic::setUnit(const char *c){
  unit = (char *)HS_REALLOC(unit, strlen(c) + 1);
  strcpy(unit, c);
  return(this);
}  

///////////////////////////////

SpanCharacteristic *SpanCharacteristic::setValidValues(int n, ...){
 
  String s="[";
  va_list vl;
  va_start(vl,n);
  for(int i=0;i<n;i++){
    switch(format){
      case FORMAT::UINT8:
        s+=(uint8_t)va_arg(vl,uint32_t);
        break;
      case FORMAT::UINT16:
        s+=(uint16_t)va_arg(vl,uint32_t);
        break;
      case FORMAT::UINT32:
        s+=(uint32_t)va_arg(vl,uint32_t);
        break;
      case FORMAT::INT:
        s+=(int)va_arg(vl,uint32_t);
        break;
      default:
        setValidValuesError=true;
        return(this);      
    }
    if(i!=n-1)
      s+=",";
  }
  va_end(vl);
  s+="]";

  validValues=(char *)HS_REALLOC(validValues, strlen(s.c_str()) + 1);
  strcpy(validValues,s.c_str());

  return(this);
}

///////////////////////////////

boolean SpanCharacteristic::EVLIST::has(HAPClient *hc){
  return(find_if(begin(), end(), [hc](const HAPClient *hcTemp){return(hc==hcTemp);}) != end());  
}

///////////////////////////////

void SpanCharacteristic::EVLIST::add(HAPClient *hc){
  if(!has(hc))
    push_back(hc);
}

///////////////////////////////

void SpanCharacteristic::EVLIST::remove(HAPClient *hc){
  auto it=remove_if(begin(), end(), [hc](const HAPClient *hcTemp){return(hc==hcTemp);});
  erase(it,end());
}

///////////////////////////////
//        SpanButton         //
///////////////////////////////

SpanButton::SpanButton(int pin, uint16_t longTime, uint16_t singleTime, uint16_t doubleTime, triggerType_t triggerType) : PushButton(pin, triggerType){

  if(homeSpan.Accessories.empty() || homeSpan.Accessories.back()->Services.empty()){
    if(buttonType==HS_BUTTON)
      LOG0("\nFATAL ERROR!  Can't create new SpanButton(%d,%u,%u,%u) without a defined Service ***\n",pin,longTime,singleTime,doubleTime);
    else
      LOG0("\nFATAL ERROR!  Can't create new SpanToggle(%d,%u) without a defined Service ***\n",pin,longTime);
      
    LOG0("\n=== PROGRAM HALTED ===");
    while(1);
  }

  this->longTime=longTime;
  this->singleTime=singleTime;
  this->doubleTime=doubleTime;
  service=homeSpan.Accessories.back()->Services.back();

  homeSpan.PushButtons.push_back(this);
}

///////////////////////////////

void SpanButton::check(){

  if( (buttonType==HS_BUTTON && triggered(singleTime,longTime,doubleTime)) ||
      (buttonType==HS_TOGGLE && toggled(longTime)) )                             // if the underlying PushButton is triggered/toggled
    service->button(pin,type());                                              // call the Service's button() routine with pin and type as parameters    
}

///////////////////////////////
//     SpanUserCommand       //
///////////////////////////////

SpanUserCommand::SpanUserCommand(char c, const char *s, void (*f)(const char *)){
  this->s=s;
  userFunction1=f;
   
  homeSpan.UserCommands[c]=this;
}

///////////////////////////////

SpanUserCommand::SpanUserCommand(char c, const char *s, void (*f)(const char *, void *), void *arg){
  this->s=s;
  userFunction2=f;
  userArg=arg;
   
  homeSpan.UserCommands[c]=this;
}

///////////////////////////////
//        SpanWebLog         //
///////////////////////////////

void SpanWebLog::init(uint16_t maxEntries, const char *serv, const char *tz, const char *url){
  this->maxEntries=maxEntries;
  timeServer=serv;
  timeZone=tz;
  if(url){
    statusURL="GET /" + String(url) + " ";
    isEnabled=true;
  }
  log = (log_t *)HS_CALLOC(maxEntries,sizeof(log_t));
}

///////////////////////////////

void SpanWebLog::initTime(void *args){
  SpanWebLog *wLog = (SpanWebLog *)args;
  
  WEBLOG("Acquiring Time from %s (%s)",wLog->timeServer,wLog->timeZone,wLog->waitTime/1000);
  configTzTime(wLog->timeZone,wLog->timeServer);
  struct tm timeinfo;
  if(getLocalTime(&timeinfo,wLog->waitTime)){
    strftime(wLog->bootTime,sizeof(wLog->bootTime),"%c",&timeinfo);
    wLog->timeInit=true;
    WEBLOG("Time Acquired: %s",wLog->bootTime);
  } else {
    WEBLOG("Can't access Time Server after %d seconds",wLog->waitTime/1000);
  }

  vTaskDelete(NULL);
  
}

///////////////////////////////

void SpanWebLog::vLog(boolean sysMsg, const char *fmt, va_list ap){

  char *buf;
  vasprintf(&buf,fmt,ap);

  if(sysMsg)
    LOG0("%s\n",buf);
  else
    LOG1("WEBLOG: %s\n",buf);
  
  if(maxEntries>0){
    int index=nEntries%maxEntries;
  
    log[index].upTime=esp_timer_get_time();
    if(timeInit)
      getLocalTime(&log[index].clockTime,10);
    else
      log[index].clockTime.tm_year=0;
  
    log[index].message=(char *)HS_REALLOC(log[index].message, strlen(buf) + 1);
    strcpy(log[index].message, buf);
    
    log[index].clientIP=homeSpan.lastClientIP;  
    nEntries++;
  }

  free(buf);
}

///////////////////////////////
//         SpanOTA           //
///////////////////////////////

int SpanOTA::init(boolean _auth, boolean _safeLoad, const char *pwd){
  if(esp_ota_get_running_partition()==esp_ota_get_next_update_partition(NULL)){
    LOG0("\n*** WARNING: Can't start OTA Server - Partition table used to compile this sketch is not configured for OTA.\n\n");
    return(-1);     
  }
  
  enabled=true;
  safeLoad=_safeLoad;
  auth=_auth;
  if(pwd==NULL)
    return(0);
  return(setPassword(pwd));
}

///////////////////////////////

int SpanOTA::setPassword(const char *pwd){
  if(strlen(pwd)<1 || strlen(pwd)>32){
    LOG0("\n*** WARNING: Cannot change OTA password to '%s'. Password length must be between 1 and 32 characters.\n\n",pwd);
    return(-1);
  }

  MD5Builder otaPwdHash;
  otaPwdHash.begin();
  otaPwdHash.add(pwd);
  otaPwdHash.calculate();
  otaPwdHash.getChars(homeSpan.spanOTA.otaPwd);  
  return(0);
}

///////////////////////////////

void SpanOTA::start(){
  LOG0("\n*** Current Partition: %s\n*** New Partition: %s\n*** OTA Starting..",
    esp_ota_get_running_partition()->label,esp_ota_get_next_update_partition(NULL)->label);
  otaPercent=0;
  STATUS_UPDATE(start(LED_OTA_STARTED),HS_OTA_STARTED)
}

///////////////////////////////

void SpanOTA::end(){
  nvs_set_u8(homeSpan.otaNVS,"OTA_REQUIRED",safeLoad);
  nvs_commit(homeSpan.otaNVS);
  LOG0(" DONE!  Rebooting...\n");
  homeSpan.reboot();
}

///////////////////////////////

void SpanOTA::progress(uint32_t progress, uint32_t total){
  int percent=progress*100/total;
  if(percent/10 != otaPercent/10){
    otaPercent=percent;
    LOG0("%d%%..",progress*100/total);
  }

  if(safeLoad && progress==total){
    SpanPartition newSpanPartition;   
    esp_partition_read(esp_ota_get_next_update_partition(NULL), sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t), &newSpanPartition, sizeof(newSpanPartition));
    LOG0("Checking for HomeSpan Magic Cookie: %s..",newSpanPartition.magicCookie);
    if(strcmp(newSpanPartition.magicCookie,spanPartition.magicCookie))
      Update.abort();
  }
}

///////////////////////////////

void SpanOTA::error(ota_error_t err){
  LOG0("*** OTA Error[%u]: ", err);
  if (err == OTA_AUTH_ERROR) LOG0("Auth Failed\n\n");
    else if (err == OTA_BEGIN_ERROR) LOG0("Begin Failed\n\n");
    else if (err == OTA_CONNECT_ERROR) LOG0("Connect Failed\n\n");
    else if (err == OTA_RECEIVE_ERROR) LOG0("Receive Failed\n\n");
    else if (err == OTA_END_ERROR) LOG0("End Failed\n\n");
}

///////////////////////////////

int SpanOTA::otaPercent;
boolean SpanOTA::safeLoad;
boolean SpanOTA::enabled=false;
boolean SpanOTA::auth;

///////////////////////////////
//        SpanPoint          //
///////////////////////////////

SpanPoint::SpanPoint(const char *macAddress, int sendSize, int receiveSize, int queueDepth, boolean useAPaddress){

  if(sscanf(macAddress,"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",peerInfo.peer_addr,peerInfo.peer_addr+1,peerInfo.peer_addr+2,peerInfo.peer_addr+3,peerInfo.peer_addr+4,peerInfo.peer_addr+5)!=6){
    LOG0("\nFATAL ERROR!  Can't create new SpanPoint(\"%s\") - Invalid MAC Address ***\n",macAddress);
    LOG0("\n=== PROGRAM HALTED ===");
    while(1);
  }

  if(sendSize<0 || sendSize>200 || receiveSize<0 || receiveSize>200 || queueDepth<1 || (sendSize==0 && receiveSize==0)){
    LOG0("\nFATAL ERROR!  Can't create new SpanPoint(\"%s\",%d,%d,%d) - one or more invalid parameters ***\n",macAddress,sendSize,receiveSize,queueDepth);
    LOG0("\n=== PROGRAM HALTED ===");
    while(1);
  }

  this->sendSize=sendSize;
  this->receiveSize=receiveSize;
  
  if(receiveSize>0)
    WiFi.mode(WIFI_AP_STA);
  else if(WiFi.getMode()==WIFI_OFF)
    WiFi.mode(WIFI_STA);    

  init();                             // initialize SpanPoint
  peerInfo.channel=0;                 // 0 = matches current WiFi channel
  
  peerInfo.ifidx=useAPaddress?WIFI_IF_AP:WIFI_IF_STA;         // specify interface as either STA or AP
  
  peerInfo.encrypt=useEncryption;     // set encryption for this peer
  memcpy(peerInfo.lmk, lmk, 16);      // set local key
  esp_now_add_peer(&peerInfo);        // add peer to ESP-NOW

  if(receiveSize>0)
    receiveQueue = xQueueCreate(queueDepth,receiveSize);  

  SpanPoints.push_back(this);             
}

///////////////////////////////

void SpanPoint::init(const char *password){

  if(initialized)
    return;

  if(WiFi.getMode()==WIFI_OFF)
    WiFi.mode(WIFI_STA);  
  
  wifi_config_t conf;                       // make sure AP is hidden (if WIFI_AP_STA is used), since it is just a "dummy" AP to keep WiFi alive for ESP-NOW
  esp_wifi_get_config(WIFI_IF_AP,&conf);
  conf.ap.ssid_hidden=1;
  esp_wifi_set_config(WIFI_IF_AP,&conf);
    
  uint8_t hash[32];
  mbedtls_sha256_ret((const unsigned char *)password,strlen(password),hash,0);      // produce 256-bit bit hash from password

  esp_now_init();                           // initialize ESP-NOW
  memcpy(lmk, hash, 16);                    // store first 16 bytes of hash for later use as local key
  esp_now_set_pmk(hash+16);                 // set hash for primary key using last 16 bytes of hash
  esp_now_register_recv_cb(dataReceived);   // set callback for receiving data
  esp_now_register_send_cb(dataSent);       // set callback for sending data
  
  statusQueue = xQueueCreate(1,sizeof(esp_now_send_status_t));    // create statusQueue even if not needed
  setChannelMask(channelMask);                                    // default channel mask at start-up uses channels 1-13  

  uint8_t channel;
  if(!isHub){                                                   // this is not a hub
    nvs_flash_init();                                           // initialize NVS
    nvs_open("POINT",NVS_READWRITE,&pointNVS);                  // open SpanPoint data namespace in NVS
    if(!nvs_get_u8(pointNVS,"CHANNEL",&channel)){               // if channel found in NVS...
      if(channelMask & (1<<channel))                            // ... and if channel is allowed by channel mask
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);   // set the WiFi channel
    }
  }
  
  initialized=true;
}

///////////////////////////////

void SpanPoint::setChannelMask(uint16_t mask){
  channelMask = mask & 0x3FFE;

  if(isHub)
    return;

  uint8_t channel=0;

  for(int i=1;i<=13 && channel==0;i++)          // find first "allowed" channel based on mask
    channel=(channelMask & (1<<i))?i:0;

  if(channel==0){
    LOG0("\nFATAL ERROR!  SpanPoint::setChannelMask(0x%04X) - mask must allow for at least one channel ***\n",mask);
    LOG0("\n=== PROGRAM HALTED ===");
    while(1);
  }

  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

///////////////////////////////

uint8_t SpanPoint::nextChannel(){

  uint8_t channel;
  wifi_second_chan_t channel2; 
  esp_wifi_get_channel(&channel,&channel2);     // get current channel

  if(isHub || channelMask==(1<<channel))        // do not change channel if device is either a hub, or channel mask does not allow for any other channels
    return(channel);

  do {
    channel=(channel<13)?channel+1:1;       // advance to next channel
  } while(!(channelMask & (1<<channel)));   // until we find next valid one

  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);         // set the WiFi channel
  
  nvs_set_u8(pointNVS,"CHANNEL",channel);
  nvs_commit(pointNVS);  
     
  return(channel);  
}

///////////////////////////////

boolean SpanPoint::get(void *dataBuf){

  if(receiveSize==0)
    return(false);

  return(xQueueReceive(receiveQueue, dataBuf, 0));
}

///////////////////////////////

boolean SpanPoint::send(const void *data){

  if(sendSize==0)
    return(false);
  
  uint8_t channel;
  wifi_second_chan_t channel2; 
  esp_wifi_get_channel(&channel,&channel2);     // get current channel
  uint8_t startingChannel=channel;              // set starting channel to current channel

  esp_now_send_status_t status = ESP_NOW_SEND_FAIL;

  do {
    for(int i=1;i<=3;i++){
      
      LOG1("SpanPoint: Sending %d bytes to MAC Address %02X:%02X:%02X:%02X:%02X:%02X using channel %hhu...\n",
        sendSize,peerInfo.peer_addr[0],peerInfo.peer_addr[1],peerInfo.peer_addr[2],peerInfo.peer_addr[3],peerInfo.peer_addr[4],peerInfo.peer_addr[5],channel);
        
      esp_now_send(peerInfo.peer_addr, (uint8_t *) data, sendSize);
      xQueueReceive(statusQueue, &status, pdMS_TO_TICKS(2000));
      if(status==ESP_NOW_SEND_SUCCESS)
        return(true);
      delay(10);
    }    
    channel=nextChannel();
  } while(channel!=startingChannel);

  return(false);
} 

///////////////////////////////

void SpanPoint::dataReceived(const uint8_t *mac, const uint8_t *incomingData, int len){
  
  auto it=SpanPoints.begin();
  for(;it!=SpanPoints.end() && memcmp((*it)->peerInfo.peer_addr,mac,6)!=0; it++);
  
  if(it==SpanPoints.end())
    return;

  if((*it)->receiveSize==0)
    return;

  if(len!=(*it)->receiveSize){
    LOG0("SpanPoint Warning! %d bytes received from %02X:%02X:%02X:%02X:%02X:%02X does not match %d-byte queue size\n",len,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],(*it)->receiveSize);
    return;
  }

  (*it)->receiveTime=millis();                             // set time of receive
  xQueueSend((*it)->receiveQueue, incomingData, 0);        // send to queue - do not wait if queue is full and instead fail immediately since we need to return from this function ASAP
}

///////////////////////////////

uint8_t SpanPoint::lmk[16];
boolean SpanPoint::initialized=false;
boolean SpanPoint::isHub=false;
boolean SpanPoint::useEncryption=true;
vector<SpanPoint *, Mallocator<SpanPoint *>> SpanPoint::SpanPoints;
uint16_t SpanPoint::channelMask=0x3FFE;
QueueHandle_t SpanPoint::statusQueue;
nvs_handle SpanPoint::pointNVS;


///////////////////////////////
//          MISC             //
///////////////////////////////

void __attribute__((weak)) loop(){
}

///////////////////////////////



 
