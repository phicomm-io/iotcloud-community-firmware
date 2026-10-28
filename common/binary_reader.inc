/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Normalized tokens: uint=0, neg=1, string=3, array=4, map=5,
 * false=20, true=21, null=22, real=27. */
PH_BINARY_PRIMITIVE bool binary_token(binary_reader *r,binary_item *v)
{
    if(r->p==r->end || ++r->nodes>PH_BINARY_NODES)return false;
    unsigned c=*r->p++,width=0;v->n=0;v->text=NULL;
    if(r->f!=PH_WIRE_MSGPACK)return false;
        if(c<128){v->kind=0;v->n=c;}
        else if(c>=0xe0){v->kind=1;v->n=0xff-c;}
        else if((c&0xe0)==0xa0){v->kind=3;v->n=c&31;}
        else if((c&0xf0)==0x90){v->kind=4;v->n=c&15;}
        else if((c&0xf0)==0x80){v->kind=5;v->n=c&15;}
        else if(c==0xc0)v->kind=22;
        else if(c==0xc2 || c==0xc3)v->kind=20+(c&1);
        else {
            if(c>=0xcc && c<=0xcf){v->kind=0;width=1u<<(c-0xcc);}
            else if(c>=0xd0 && c<=0xd3){v->kind=0;width=1u<<(c-0xd0);}
            else if(c>=0xc4 && c<=0xc6){v->kind=2;width=1u<<(c-0xc4);}
            else if(c>=0xd9 && c<=0xdb){v->kind=3;width=1u<<(c-0xd9);}
            else if(c==0xdc || c==0xdd){v->kind=4;width=c==0xdc?2:4;}
            else if(c==0xde || c==0xdf){v->kind=5;width=c==0xde?2:4;}
            else if(c==0xca || c==0xcb){v->kind=27;width=c==0xca?4:8;}
            else return false;
            if(!binary_take(r,width,&v->n))return false;
            if(c>=0xd0 && c<=0xd3 && (v->n&(UINT64_C(1)<<(width*8-1)))) {
                v->kind=1;v->n=width==8?~v->n:((UINT64_C(1)<<(width*8))-1)^v->n;
            }
            if(c==0xca || c==0xcb)goto floating;
        }
    if(v->kind==2 || v->kind==3) {
        if(v->n>(uint64_t)(r->end-r->p) || (v->kind==3 && !ph_wire_utf8(r->p,(size_t)v->n)))return false;
        v->text=r->p;r->p+=(size_t)v->n;
    }
    if((v->kind<=1 && v->n>PH_BINARY_SAFE-(v->kind==1)) ||
       ((v->kind==4 || v->kind==5) && v->n>PH_BINARY_NODES))return false;
    return true;
floating:
    if(width==8){uint64_t bits=v->n;memcpy(&v->real,&bits,8);}
    else if(width==4){uint32_t bits=(uint32_t)v->n;float f;memcpy(&f,&bits,4);v->real=f;}
    v->kind=27;return isfinite(v->real);
}

