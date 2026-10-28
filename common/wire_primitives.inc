/* SPDX-License-Identifier: GPL-3.0-or-later */
PH_WIRE_PRIMITIVE size_t ph_wire_head(ph_wire_format f,unsigned kind,uint64_t n,unsigned char out[9])
{
    if(f!=PH_WIRE_MSGPACK)return 0;
    /* kind: 0 unsigned, 1 negative magnitude (-1-n), 3 text, 4 array, 5 map */
    if(kind==0 && n<128){out[0]=(unsigned char)n;return 1;}
    if(kind==1 && n<32){out[0]=(unsigned char)(0xff-n);return 1;}
    if(kind==3 && n<32){out[0]=(unsigned char)(0xa0+n);return 1;}
    if((kind==4 || kind==5) && n<16){out[0]=(unsigned char)((kind==4?0x90:0x80)+n);return 1;}
    unsigned a;
    if(kind==0 || kind==1) {
        uint64_t v=kind==1?~n:n;
        a=kind==0?(n<=255?1:n<=65535?2:n<=UINT32_MAX?4:8):
          (n<=127?1:n<=32767?2:n<=INT32_MAX?4:8);
        out[0]=(unsigned char)((kind==0?0xcc:0xd0)+(a==1?0:a==2?1:a==4?2:3));n=v;
    } else {
        a=(kind==2 || kind==3) && n<=255?1:n<=65535?2:4;
        out[0]=(unsigned char)(kind==2?(a==1?0xc4:a==2?0xc5:0xc6):kind==3?(a==1?0xd9:a==2?0xda:0xdb):kind==4?(a==2?0xdc:0xdd):(a==2?0xde:0xdf));
    }
    for(unsigned i=0;i<a;i++)out[1+i]=(unsigned char)(n>>(8*(a-i-1)));
    return a+1;
}
PH_WIRE_PRIMITIVE size_t ph_wire_double(ph_wire_format f,double value,unsigned char out[9])
{
    if(f!=PH_WIRE_MSGPACK)return 0;
    uint64_t bits;memcpy(&bits,&value,8);out[0]=0xcb;
    for(unsigned i=0;i<8;i++)out[1+i]=(unsigned char)(bits>>(56-8*i));
    return 9;
}
PH_WIRE_PRIMITIVE bool ph_wire_utf8(const unsigned char *p,size_t n)
{
    for(size_t i=0;i<n;) {
        unsigned c=p[i++],extra;uint32_t v,min;
        if(c<128){if(!c)return false;continue;}
        if(c>=0xc2 && c<=0xdf){extra=1;v=c&31;min=128;}
        else if(c>=0xe0 && c<=0xef){extra=2;v=c&15;min=2048;}
        else if(c>=0xf0 && c<=0xf4){extra=3;v=c&7;min=65536;}
        else return false;
        if(extra>n-i)return false;
        while(extra--){c=p[i++];if((c&192)!=128)return false;v=(v<<6)|(c&63);}
        if(v<min || v>0x10ffff || (v>=0xd800 && v<=0xdfff))return false;
    }
    return true;
}
