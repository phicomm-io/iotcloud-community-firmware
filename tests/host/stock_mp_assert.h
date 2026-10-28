/* Typed assertion observers for the original UART/storage scenario bodies.
 * This fixture does not parse text or construct/link JSON objects. */
typedef struct { ph_value value; ph_buf *owner; } fixture_value;
static fixture_value fixture_nodes[128];static unsigned fixture_used;
static fixture_value *fixture_node(ph_value v){assert(fixture_used<128);fixture_value*n=&fixture_nodes[fixture_used++];*n=(fixture_value){.value=v};return n;}
static fixture_value *fixture_get(fixture_value*p,const char*k){ph_value v;if(!p||!phv_get(&p->value,k,&v)||v.kind==PHV_NULL)return NULL;return fixture_node(v);}
static bool fixture_bool(fixture_value*p){bool v=false;if(p)assert(phv_bool(&p->value,&v));return v;}
static int fixture_int(fixture_value*p){int64_t v=0;if(p)assert(phv_int(&p->value,&v));return (int)v;}
static const char *fixture_text(fixture_value*p){static char text[256];assert(p&&phv_text(&p->value,text,sizeof(text)));return text;}
static void fixture_dump(ph_value v,char *out,size_t cap){if(v.kind==PHV_TEXT){char text[256];assert(phv_text(&v,text,sizeof(text)));assert(strlen(out)+strlen(text)+2<cap);strcat(out,text);strcat(out," ");}else if(v.kind==PHV_ARRAY){for(unsigned i=0;i<v.count;i++){ph_value c;assert(phv_at(&v,i,&c));fixture_dump(c,out,cap);}}else if(v.kind==PHV_MAP){ph_iterator it;char key[128];ph_value c;assert(phv_iter(&v,&it));while(phv_next(&it,key,&c))fixture_dump(c,out,cap);}}
static const char *fixture_description(fixture_value*p){static char text[8192];assert(p);text[0]=0;fixture_dump(p->value,text,sizeof(text));return text;}
static fixture_value *fixture_finish(ph_writer*w){ph_buf*b=phw_take(w);ph_value v;assert(b&&phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&v));assert(!fixture_used);fixture_value*n=fixture_node(v);n->owner=b;return n;}
static void fixture_release(fixture_value*p){assert(p&&p->owner);ph_buf_free(p->owner);fixture_used=0;}
static fixture_value *service_product_capabilities(void){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));service_product_capabilities_mp(&w,NULL,false);return fixture_finish(&w);}
static fixture_value *m1_alarm_json(void){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));smp_alarm(&w);return fixture_finish(&w);}
static fixture_value *fixture_query(const char*a){static const unsigned char empty[]={0x80};ph_value p;assert(phv_parse(PH_WIRE_MSGPACK,empty,sizeof(empty),&p));ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));const char*c;assert(service_product_mp(a,&p,&w,&c)&&!strcmp(c,"ok"));return fixture_finish(&w);}
static fixture_value *m1_co2_json(void){return fixture_query("co2_display.get");}
static fixture_value *m1_sensor_details_json(void){return fixture_query("sensor_details.get");}
