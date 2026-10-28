/* SPDX-License-Identifier: GPL-3.0-or-later */
void feature_fresh_fields_mp(const fresh_config_t *f,ph_writer *w) {
 linkage_encode_mp(w,&f->linkage,&f->co2,f->fallback_speed);
}
bool feature_fresh_decode_mp(fresh_config_t *f,const ph_value *o) {
 ph_iterator it;if(!phv_iter(o,&it)||o->kind!=PHV_MAP)return false;char key[128];ph_value value;
 while(phv_next(&it,key,&value)){
  bool known=!strcmp(key,"revision")||!strcmp(key,"fallback_speed");
  for(unsigned i=0;i<sizeof(mp_linkage_fields)/sizeof(mp_linkage_fields[0]);i++)if(!strcmp(key,mp_linkage_fields[i].key))known=true;
  for(unsigned i=0;i<sizeof(mp_co2_fields)/sizeof(mp_co2_fields[0]);i++)if(!strcmp(key,mp_co2_fields[i].key))known=true;
  if(!known)return false;
 }
 bool ok=mp_linkage_decode(&f->linkage,&f->co2,&f->fallback_speed,*o);if(ok)f->linkage.enabled=1;return ok;
}
