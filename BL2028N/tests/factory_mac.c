#include "factory_mac.h"
#include <assert.h>
#include <stdio.h>
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);unsigned char sector[PH_S30_RF_SIZE],mac[6],copy[PH_S30_RF_SIZE];
 assert(!fseek(f,PH_S30_RF_ADDRESS,SEEK_SET));assert(fread(sector,1,sizeof(sector),f)==sizeof(sector));fclose(f);
 assert(ph_factory_mac(sector,sizeof(sector),mac));assert(ph_mac_valid(mac));
 char ap[24],host[32];assert(ph_mac_ap_name(ap,mac)&&ph_mac_host_name(host,mac));
 assert(strncmp(ap,"S30M_V1B_",9)==0&&strncmp(host,"iotcloud-s30-v1b-",16)==0);
 const unsigned char other[6]={0x02,0x12,0x34,0xab,0xcd,0xef};
 assert(ph_mac_ap_name(ap,other)&&!strcmp(ap,"S30M_V1B_ABCDEF"));
 assert(ph_mac_host_name(host,other)&&!strcmp(host,"iotcloud-s30-v1b-abcdef"));
 const unsigned char second[6]={0x02,0x12,0x34,0x12,0x34,0x56};
 assert(ph_mac_ap_name(ap,second)&&!strcmp(ap,"S30M_V1B_123456"));
 assert(ph_mac_host_name(host,second)&&!strcmp(host,"iotcloud-s30-v1b-123456"));
 for(size_t n=0;n<34;++n)assert(!ph_factory_mac(sector,n,mac));
 memcpy(copy,sector,sizeof(copy));memset(copy,255,sizeof(copy));assert(!ph_factory_mac(copy,sizeof(copy),mac));
 memcpy(copy,sector,sizeof(copy));copy[4]=copy[5]=copy[6]=copy[7]=255;assert(!ph_factory_mac(copy,sizeof(copy),mac));
 const unsigned char invalid[][6]={{0},{255,255,255,255,255,255},{1,2,3,4,5,6},{0xc8,0x47,0x8c,0,0,0}};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i){assert(!ph_mac_valid(invalid[i]));assert(!ph_mac_ap_name(ap,invalid[i])&&!ap[0]);assert(!ph_mac_host_name(host,invalid[i])&&!host[0]);}
 puts("PASS: stock RF TLV, bounded MAC validation and product AP/DHCP names; default MACs rejected");
}
