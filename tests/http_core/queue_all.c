/* Reuse the fault harness I/O; exercise the real bounded two-waiter scheduler. */
#define main original_fault_main
#include "fault.c"
#undef main
static bool blocked;
static unsigned order[16], order_count;
static int fast_send(int fd,const void *p,size_t n){
 if(blocked){error_value=11;return -1;}
 assert(lengths[fd]+n<sizeof(output[fd]));
 memcpy(output[fd]+lengths[fd],p,n);lengths[fd]+=n;return (int)n;
}
static ph_http_action ordered_begin(ph_http_exchange *x){
 assert(order_count<16);order[order_count++]=(unsigned)(x->request->path[1]-'0');
 return begin(x);
}
static ph_http_io queue_io;
static ph_http_app queue_app;
static void setup(uint32_t start){
 reset();ph_http_server_close(&s);memset(closes,0,sizeof(closes));
 tick=start;desired=PH_HTTP_CLIENT_SLOTS;blocked=true;order_count=0;
 queue_io=io;queue_io.send=fast_send;queue_app=app;queue_app.begin=ordered_begin;
 assert(ph_http_server_open(&s,&queue_io,&queue_app,PH_HTTP_CLIENT_SLOTS));
}
static unsigned ready_count(void){unsigned n=0;for(unsigned i=0;i<PH_HTTP_CLIENT_SLOTS;i++)n+=s.slots[i].state==PH_SLOT_READY;return n;}
static void fill(void){
 static const char *requests[]={"GET /1 HTTP/1.1\r\n\r\n","GET /2 HTTP/1.1\r\n\r\n","GET /3 HTTP/1.1\r\n\r\n","GET /4 HTTP/1.1\r\n\r\n","GET /5 HTTP/1.1\r\n\r\n","GET /6 HTTP/1.1\r\n\r\n"};
 for(int fd=1;fd<=3;fd++)connect_one(fd,requests[fd-1]);
 run(1);assert(begins==1&&ready_count()==2&&releases==0);
 for(unsigned i=0;i<PH_HTTP_CLIENT_SLOTS;i++)assert(s.slots[i].lease==(s.active==(int)i));
}
int main(void){
 if(PH_HTTP_CLIENT_SLOTS==4){
  setup(0);fill();connect_one(4,"GET /4 HTTP/1.1\r\n\r\n");
  s.slots[s.active].header_sent=s.response_header_length;
  s.slots[s.active].body_sent=s.exchange.response.length;
  for(unsigned i=0;i<4;i++)if(s.slots[i].state==PH_SLOT_READY)s.slots[i].deadline=tick+25+PH_HTTP_READY_MS;
  s.turn=0;blocked=false;run(1);
  assert(order_count==2&&order[1]==2&&ready_count()==2);
  run(12);assert(order_count==4&&order[2]==3&&order[3]==4&&releases==4);
  ph_http_server_close(&s);assert(live==0);
 }
 for(unsigned wrap=0;wrap<2;wrap++){
  setup(wrap?UINT32_MAX-150u:0);fill();
  /* A fourth connection never becomes a third waiter. */
  if(PH_HTTP_CLIENT_SLOTS==4){
   connect_one(4,"GET /4 HTTP/1.1\r\n\r\n");run(1);
   assert(ready_count()==2&&begins==1&&s.diagnostics.rejected==1);
  }else{
   child=4;input[4]="GET /4 HTTP/1.1\r\n\r\n";run(1);
   assert(child==4&&s.diagnostics.accepted==3);child=-1;
  }
  blocked=false;run(20);assert(begins==3&&releases==3&&sent_ok==3&&s.diagnostics.rejected==(PH_HTTP_CLIENT_SLOTS==4?1u:0u));
  assert(order_count==3);for(unsigned i=0;i<3;i++){assert(order[i]==i+1);assert(closes[i+1]==1);}
  /* Backlog drains and a fresh request can proceed. */
  connect_one(7,"GET /7 HTTP/1.1\r\n\r\n");run(4);assert(begins==4&&sent_ok==4);ph_http_server_close(&s);assert(live==0);
 }
 setup(0);fill();tick+=PH_HTTP_READY_MS;run(1);assert(ready_count()==0&&begins==1&&s.diagnostics.rejected==2);
 blocked=false;run(8);assert(releases==1&&s.diagnostics.busy_sent==2);ph_http_server_close(&s);assert(live==0);
 /* Lower OTA admission must neither admit another client nor lose leases. */
 setup(0);fill();desired=2;child=7;run(1);assert(s.cap==2&&child==7);child=-1;
 blocked=false;run(20);assert(releases==3&&sent_ok==3);ph_http_server_close(&s);assert(live==0);
 /* Stopping a fully occupied server releases exactly the active lease. */
 setup(0);fill();ph_http_server_close(&s);assert(releases==1&&live==0);
 puts("two HTTP waiting slots: bounded queue/FIFO/wrap/timeout/OTA admission/recovery PASS");return 0;
}
