/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../typed_wire.h"
#include "version.h"
#include <ctype.h>
/* Metadata is copied out before freeing the borrowed transfer buffer. Signed
 * firmware still performs its independent signature, hash and rollback checks. */
typedef struct {char version[32],path[200],release_log[200];uint32_t size;} ph_release_manifest;
static bool ph_release_path(const char *s)
{
 if(strncmp(s,"/releases/v",11)||strlen(s)>=200||strstr(s,".."))return false;
 for(;*s;s++)if(!isalnum((unsigned char)*s)&&!strchr("/._-",*s))return false;
 return true;
}
static bool ph_release_text(const ph_value *v,const char *key,char *out,size_t capacity)
{ph_value field;return phv_get(v,key,&field)&&phv_text(&field,out,capacity);}
static bool ph_release_hex(const char *text,size_t length)
{if(strlen(text)!=length)return false;for(size_t i=0;i<length;i++)if(!isxdigit((unsigned char)text[i]))return false;return true;}
static bool ph_release_manifest_parse(const void *data,size_t length,const char *model,const char *variant,ph_release_manifest *out)
{
 ph_value root,v;int64_t n;char text[80],manifest_model[8],manifest_variant[48];
 memset(out,0,sizeof(*out));
 if(!phv_parse(PH_WIRE_MSGPACK,data,length,&root)||root.kind!=PHV_MAP||
    !phv_keys(&root,"|schema||tag||version||commit||model||firmware_variant||path||size||sha256||release_log|")||
    !phv_get(&root,"schema",&v)||!phv_int(&v,&n)||n!=1||
    !phv_get(&root,"size",&v)||!phv_int(&v,&n)||n<=0||n>UINT32_MAX)return false;
 out->size=(uint32_t)n;
 if(!ph_release_text(&root,"model",manifest_model,sizeof(manifest_model))||strcmp(manifest_model,model)||
    !ph_release_text(&root,"firmware_variant",manifest_variant,sizeof(manifest_variant))||strcmp(manifest_variant,variant)||
    !ph_release_text(&root,"version",out->version,sizeof(out->version))||
    !ph_release_text(&root,"path",out->path,sizeof(out->path))||!ph_release_path(out->path)||
    strlen(out->path)<=7||strcmp(out->path+strlen(out->path)-7,".signed")||
    !ph_release_text(&root,"release_log",out->release_log,sizeof(out->release_log))||!ph_release_path(out->release_log)||
    !ph_release_text(&root,"sha256",text,sizeof(text))||!ph_release_hex(text,64)||
    !ph_release_text(&root,"commit",text,sizeof(text))||!ph_release_hex(text,40)||
    !ph_release_text(&root,"tag",text,sizeof(text))||text[0]!='v'||strcmp(text+1,out->version))return false;
 ph_ota_version_t version;return ph_ota_version_parse(out->version,&version);
}
