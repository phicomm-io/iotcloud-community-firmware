#define main legacy_auth_main
#include "../local_auth/test_local_auth.c"
#undef main
int main(void){
 fixture f;ph_auth_state s;ph_auth_context c,old;char key[33],token[33],again[33],prior[33];setup(&f,&s);
 assert(ph_auth_pair(&s,"","aabbccddeeff",PH_AUTH_SCOPE_CONTROL,key));unsigned saves=f.saves;
 assert(!ph_auth_pair_resume_boot(&s,"aabbccddeeff",key,0,token));
 assert(ph_auth_pair_resume_boot(&s,"aabbccddeeff",key,101,token));assert(ph_auth_udp_check(&s,"aabbccddeeff",token,&c));
 uint32_t born=s.local[c.slot].born,last=s.local[c.slot].last;unsigned sequence=f.sequence;
 f.now+=1000;f.fail_random=true;
 assert(ph_auth_pair_resume_boot(&s,"aabbccddeeff",key,101,again)&&!strcmp(token,again));
 assert(s.local[c.slot].born==born&&s.local[c.slot].last==last&&f.sequence==sequence&&f.saves==saves);
 assert(!ph_auth_pair_resume_boot(&s,"aabbccddeeff",key,102,again));assert(ph_auth_context_valid(&c,PH_AUTH_SCOPE_CONTROL));
 f.fail_random=false;old=c;strcpy(prior,token);
 assert(ph_auth_pair_resume_boot(&s,"aabbccddeeff",key,102,token)&&strcmp(prior,token));
 assert(!ph_auth_udp_check(&s,"aabbccddeeff",prior,&c)&&!ph_auth_context_valid(&old,PH_AUTH_SCOPE_CONTROL));
 assert(ph_auth_udp_check(&s,"aabbccddeeff",token,&c));
 /* Replaying an old resume with the durable key never resurrects its token.
  * Possession of that key can issue a fresh resume, as on the legacy API. */
 assert(ph_auth_pair_resume_boot(&s,"aabbccddeeff",key,101,again)&&strcmp(prior,again)&&strcmp(token,again));
 assert(!ph_auth_udp_check(&s,"aabbccddeeff",token,&c));assert(f.saves==saves);
 assert(ph_auth_pair_resume(&s,"aabbccddeeff",key,token)&&!strcmp(token,again));
 assert(ph_auth_udp_check(&s,"aabbccddeeff",token,&c));born=s.local[c.slot].born;last=s.local[c.slot].last;
 f.now+=PH_AUTH_IDLE_MS;assert(ph_auth_pair_resume_boot(&s,"aabbccddeeff",key,101,again)&&strcmp(token,again));
 assert(!ph_auth_context_valid(&c,PH_AUTH_SCOPE_CONTROL)&&f.saves==saves);
 assert(ph_auth_pair_remove(&s,"aabbccddeeff"));assert(!ph_auth_pair_resume_boot(&s,"aabbccddeeff",key,101,token));
 (void)born;(void)last;puts("boot-bound resume: TTL/retry/rotation/replay/legacy/revocation PASS");return 0;
}
