/* TC1 HTTP small controls. The product lease supplies the body and scalar;
 * the only retained allocation is prepared before any request body arrives.
 * No allocation or retry occurs after entering the product control. */
void *service_http_control_credit(void)
{return ph_wire_malloc(sizeof(mp_receipt_frame)+SERVICE_HTTP_CONTROL_REPLY);}
void service_http_control_credit_free(void *credit){ph_wire_free(credit);}

static size_t mp_control_reply(void *bytes,size_t capacity,const char *id,const char *code)
{
 ph_writer w;ph_buf storage;
 if(!phw_init_fixed(&w,PH_WIRE_MSGPACK,&storage,bytes,capacity))return 0;
 phw_begin(&w,NULL,false);if(*id)phw_text(&w,"request_id",id);
 phw_text(&w,"code",code);phw_text(&w,"message",mp_message(code));
 phw_begin(&w,"data",false);phw_end(&w);phw_end(&w);
 return phw_finish_fixed(&w);
}
bool service_http_control_mp(const void *raw,size_t length,const ph_auth_context *http,
                             void **credit,void *reply,size_t capacity,size_t *reply_length,
                             unsigned *status)
{
 mp_request r;
 if(length>SERVICE_HTTP_CONTROL_INPUT || !mp_decode(raw,length,&r) ||
    (strcmp(r.action,"indicator.set") && strcmp(r.action,"outlet.set") && strcmp(r.action,"child_lock.set")))return false;
 /* Check every possible control/error envelope BEFORE mutation, including
  * longest request_id and persistence failure. Future vocabulary changes
  * cannot turn an executed control into a failed output allocation. */
 static const char *const codes[]={"accepted","storage_error","invalid_params","busy",
     "unsupported","conflict","unauthorized","migration_required"};
 size_t bound=capacity<SERVICE_HTTP_CONTROL_REPLY?capacity:SERVICE_HTTP_CONTROL_REPLY;
 *reply_length=0;*status=200;
 for(unsigned i=0;i<sizeof(codes)/sizeof(codes[0]);i++)
  if(!mp_control_reply(reply,bound,r.id,codes[i])){
   *status=503;*reply_length=mp_control_reply(reply,capacity,r.id,"busy");return true;
  }
 const char *code="busy";bool entered=false,record=false,dispatched=false;
 unsigned char digest[32];
 if(r.id[0]){
  SHA256Context sha;
  if(SHA256Reset(&sha)||SHA256Input(&sha,raw,length)||SHA256Result(&sha,digest))goto respond;
 }
 if(!service_command_enter(true))goto respond;
 entered=true;
 if(!http || !ph_auth_context_valid ||
    (http->source!=PH_AUTH_HTTP && http->source!=PH_AUTH_HTTP_OPEN && http->source!=PH_AUTH_HTTP_BASIC) ||
    !ph_auth_context_valid(http,PH_AUTH_SCOPE_ADMIN)){code="unauthorized";*status=401;goto respond;}
 if(device_config_recovery()){code="migration_required";goto respond;}
 if(r.id[0])for(unsigned i=0;i<8;i++)if(!mp_receipts[i].lan && !strcmp(mp_receipts[i].id,r.id)){
  mp_receipt_frame *saved=mp_receipts[i].response;
  if(memcmp(mp_receipts[i].digest,digest,32)){code="conflict";goto respond;}
  if(!saved)goto respond; /* An older dispatcher tombstone still forbids replay. */
  if(saved->length>capacity)goto respond;
  memcpy(reply,saved->base?saved->base->bytes:saved->bytes,saved->length);
  if(saved->base)mp_receipt_apply(reply,saved);
  *reply_length=saved->length;service_command_leave();return true;
 }
 if(ota_job_active() || (r.id[0] && (!credit || !*credit)))goto respond;
 /* The allowlist's TC1 handlers return an empty data map and allocate no
  * business writer. The SDK may fail persistence; that is a real result. */
 {
  unsigned char empty[4];ph_buf storage;ph_writer w;
  phw_init_fixed(&w,PH_WIRE_MSGPACK,&storage,empty,sizeof(empty));
  code="invalid_params";
  if(!service_product_mp(r.action,&r.params,&w,&code))code="unsupported";
  dispatched=true;
  record=r.id[0]!=0;
 }
respond:
 *reply_length=mp_control_reply(reply,bound,r.id,code);
 if(record){
  mp_receipt *saved=&mp_receipts[mp_receipt_next++%8];
  mp_receipt_frame *frame=*credit,*previous=saved->response;
  saved->response=NULL;mp_receipt_release(previous);memset(saved,0,sizeof(*saved));
  frame->base=NULL;frame->references=1;frame->length=(uint16_t)*reply_length;frame->encoded=(uint16_t)*reply_length;
  memcpy(frame->bytes,reply,*reply_length);strcpy(saved->id,r.id);memcpy(saved->digest,digest,32);
  saved->response=frame;*credit=NULL;
 }
 if(entered)service_command_leave();
 if(dispatched)service_event(!strcmp(code,"accepted")?"info":"warn","command",!strcmp(code,"accepted")?"accepted":"rejected");
 return true;
}
