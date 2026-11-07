import Cocoa
import CoreBluetooth
final class Provision: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
 var manager: CBCentralManager!
 var target: CBPeripheral?
 var tx: CBCharacteristic?
 var seq: UInt8 = 0
 var configured = false
 let configPath = CommandLine.arguments.dropFirst().first ?? NSHomeDirectory()+"/.config/iotcloud-g3/wifi.plist"
 var config: NSDictionary? { NSDictionary(contentsOfFile: configPath) }
 let service = CBUUID(string:"FFFF")
 func log(_ s:String) {
  let u=URL(fileURLWithPath:NSTemporaryDirectory()+"iotcloud-g3-provision.log"), d=(s+"\n").data(using:.utf8)!
  if let f=try? FileHandle(forWritingTo:u){f.seekToEndOfFile();f.write(d);f.closeFile()}else{try? d.write(to:u)}
 }
 func start(){log("starting");manager=CBCentralManager(delegate:self,queue:nil);DispatchQueue.main.asyncAfter(deadline:.now()+180){self.log("finished");exit(0)}}
 func centralManagerDidUpdateState(_ c:CBCentralManager){
  log("bluetooth state \(c.state.rawValue) auth \(CBManager.authorization.rawValue)")
  if c.state == .poweredOn {c.scanForPeripherals(withServices:nil,options:nil)}
 }
 func centralManager(_ c:CBCentralManager,didDiscover p:CBPeripheral,advertisementData a:[String:Any],rssi:NSNumber){
  let name=(a[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
  if name.uppercased().hasPrefix("SENSOR3_") { log("candidate \(name) \(p.identifier)") }
  guard target == nil, name.uppercased() == (config?["device_name"] as? String)?.uppercased() else{return}
  target=p;p.delegate=self;c.stopScan();c.connect(p,options:nil)
 }
 func centralManager(_ c:CBCentralManager,didConnect p:CBPeripheral){log("connected \(p.name ?? "")");p.discoverServices([service])}
 func centralManager(_ c:CBCentralManager,didFailToConnect p:CBPeripheral,error:Error?){log("connect failed \(String(describing:error))")}
 func peripheral(_ p:CBPeripheral,didDiscoverServices error:Error?){for s in p.services ?? []{p.discoverCharacteristics(nil,for:s)}}
 func peripheral(_ p:CBPeripheral,didDiscoverCharacteristicsFor s:CBService,error:Error?){
  for ch in s.characteristics ?? [] {
   log("characteristic \(ch.uuid) props \(ch.properties.rawValue)")
   if ch.uuid == CBUUID(string:"FF01"){tx=ch}
   if ch.uuid == CBUUID(string:"FF02"){p.setNotifyValue(true,for:ch)}
  }
 }
 func send(_ type:UInt8,_ payload:[UInt8]=[],_ control:UInt8=0){
  guard let p=target,let ch=tx else{return}
  let frame=Data([type,control,seq,UInt8(payload.count)]+payload);let sentSequence=seq;seq &+= 1
  p.writeValue(frame,for:ch,type:.withResponse)
  log("sent type \(type) seq \(sentSequence) length \(payload.count)")
 }
 func peripheral(_ p:CBPeripheral,didUpdateNotificationStateFor ch:CBCharacteristic,error:Error?){
  if let e=error{log("notify failed \(e)");return}
  if ch.isNotifying {send(0x1c)}
 }
 func peripheral(_ p:CBPeripheral,didWriteValueFor ch:CBCharacteristic,error:Error?){if let e=error{log("write failed \(e)")}}
 func peripheral(_ p:CBPeripheral,didUpdateValueFor ch:CBCharacteristic,error:Error?){
  guard let d=ch.value else{return};let b=[UInt8](d)
  log("notification type \(b.first ?? 0) length \(b.count)")
  guard b.count>=4,b[0] == 0x41,!configured else{return}
  configured=true
  guard let cfg=config, let macText=cfg["gateway_mac"] as? String else{log("missing config/gateway_mac");return}
  let compact=macText.replacingOccurrences(of:":",with:"")
  guard compact.count == 12 else{log("invalid gateway MAC");return}
  var mac:[UInt8]=[]
  for i in stride(from:0,to:12,by:2) {
   let start=compact.index(compact.startIndex,offsetBy:i), end=compact.index(start,offsetBy:2)
   guard let v=UInt8(compact[start..<end],radix:16) else{log("invalid gateway MAC");return}
   mac.append(v)
  }
  guard mac != Array(repeating:0,count:6),mac != Array(repeating:255,count:6),mac[0]&1 == 0 else{log("reserved gateway MAC");return}
  var body:[UInt8]
  if cfg["operation"] as? String == "gateway" {
   body=[0x42,3,0,0,6]+mac+[3]
  } else {
   guard let ssid=cfg["ssid"] as? String,let password=cfg["password"] as? String,let address=cfg["gateway_ip"] as? String else{log("missing Wi-Fi config");return}
   let ss=Array(ssid.utf8), pw=Array(password.utf8), ip=Array(address.utf8)
   guard ss.count <= 32,pw.count <= 64,ip.count <= 15 else{log("invalid config lengths");return}
   body=[0x42,1,0,0,UInt8(ss.count)]
   body += ss;body += [UInt8(pw.count)];body += pw
   body += [UInt8(ip.count)];body += ip;body += [6]+mac+[3]
  }
  var chunks:[([UInt8],UInt8)]=[]
  var left=body
  while left.count>16 {
   let n=left.count
   let part=[UInt8(n & 255),UInt8(n >> 8)]+Array(left.prefix(14))
   chunks.append((part,0x10));left.removeFirst(14)
  }
  chunks.append((left,0))
  log("sending factory custom config fragments \(chunks.count)")
  for(i,f) in chunks.enumerated(){DispatchQueue.main.asyncAfter(deadline:.now()+Double(i+1)*0.5){self.send(0x4d,f.0,f.1)}}

 }
}
let app=NSApplication.shared
app.setActivationPolicy(.accessory)
let provision=Provision();provision.start();app.run()
