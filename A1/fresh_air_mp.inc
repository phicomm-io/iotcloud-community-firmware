/* SPDX-License-Identifier: GPL-3.0-or-later */
static void fresh_encode_mp(ph_writer *w,const fresh_config_t *c,bool linkage){
 phw_begin(w,NULL,false);phw_uint(w,"revision",c->revision);
 if(linkage)feature_fresh_fields_mp(c,w);
 else{
  phw_uint(w,"pulses_per_rev",c->mapping.pulses_per_rev);phw_uint(w,"coupling",c->mapping.coupling);phw_uint(w,"cap_ratio",c->mapping.cap_ratio);phw_uint(w,"boost_ratio",c->mapping.boost_ratio);
  phw_bool(w,"active_low",c->mapping.active_low);phw_number(w,"a",c->mapping.a);phw_number(w,"b",c->mapping.b);phw_number(w,"pwm_min",c->mapping.pwm_min);phw_number(w,"pwm_max",c->mapping.pwm_max);
  phw_bool(w,"on",c->on);phw_uint(w,"speed",c->speed);phw_text(w,"mode",c->automatic?"auto":"manual");
 }phw_end(w);
}
bool fresh_mp(const char *action,const ph_value *p,ph_writer *w,const char **code){
 if(strncmp(action,"fresh_air.",10))return false;
 fresh_config_t c;fresh_config_get(&c);*code="invalid_params";
 bool linkage=!strncmp(action,"fresh_air.linkage.",18);
 bool get=!strcmp(action,"fresh_air.config.get")||!strcmp(action,"fresh_air.linkage.get");
 bool config_set=!strcmp(action,"fresh_air.config.set"),control=!strcmp(action,"fresh_air.set");
 if(!p||p->kind!=PHV_MAP)goto invalid;
 if(get){if(!phv_keys(p,""))goto invalid;*code="ok";fresh_encode_mp(w,&c,linkage);return true;}
 if(!config_set&&!control&&strcmp(action,"fresh_air.linkage.set")){*code="unknown_action";goto invalid;}
 ph_value rv;bool has_revision=phv_get(p,"revision",&rv);int64_t revision;
 if(!control&&!has_revision)goto invalid;
 if(has_revision){if(!phv_int(&rv,&revision)||revision<0||revision>UINT32_MAX)goto invalid;if((uint32_t)revision!=c.revision){*code="conflict";goto invalid;}}
 if(linkage){if(!feature_fresh_decode_mp(&c,p))goto invalid;}
 else{
  ph_iterator it;if(!phv_iter(p,&it))goto invalid;char key[128];ph_value v;
  while(phv_next(&it,key,&v)){
   if(!strcmp(key,"revision"))continue;
   if(control&&!strcmp(key,"mode")){if(phv_equal_text(&v,"auto"))c.automatic=1;else if(phv_equal_text(&v,"manual"))c.automatic=0;else goto invalid;continue;}
   if((control&&!strcmp(key,"on"))||(config_set&&!strcmp(key,"active_low"))){bool flag;if(!phv_bool(&v,&flag))goto invalid;if(control)c.on=flag;else c.mapping.active_low=flag;continue;}
   double n;if(!phv_number(&v,&n)||!isfinite(n))goto invalid;
   if(control&&!strcmp(key,"speed")){if(n<0||n>100||floor(n)!=n)goto invalid;c.speed=n;}
   else if(config_set&&!strcmp(key,"a"))c.mapping.a=n;
   else if(config_set&&!strcmp(key,"b"))c.mapping.b=n;
   else if(config_set&&!strcmp(key,"pwm_min"))c.mapping.pwm_min=n;
   else if(config_set&&!strcmp(key,"pwm_max"))c.mapping.pwm_max=n;
   else if(config_set&&(!strcmp(key,"coupling")||!strcmp(key,"cap_ratio")||!strcmp(key,"boost_ratio")||!strcmp(key,"pulses_per_rev"))){
    if(n<0||n>500||floor(n)!=n)goto invalid;
    if(!strcmp(key,"coupling")){if(n>2)goto invalid;c.mapping.coupling=n;}
    else if(!strcmp(key,"cap_ratio"))c.mapping.cap_ratio=n;else if(!strcmp(key,"boost_ratio"))c.mapping.boost_ratio=n;else c.mapping.pulses_per_rev=n;
   }else goto invalid;
  }
 }
 if(!fresh_mapping_valid(&c.mapping))goto invalid;
 *code=fresh_config_save(&c);if(strcmp(*code,"accepted"))goto invalid;
 service_cache_changed(SERVICE_CACHE_ALL);fresh_config_get(&c);fresh_encode_mp(w,&c,linkage);return true;
 invalid:phw_begin(w,NULL,false);phw_end(w);return true;
}
