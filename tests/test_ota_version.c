#include "../common/ota/version.h"
#include <assert.h>
#include <stdio.h>

static void order(const char *a,const char *b,int expected)
{
    ph_ota_version_t left,right;
    assert(ph_ota_version_parse(a,&left) && ph_ota_version_parse(b,&right));
    assert(ph_ota_version_compare(&left,&right)==expected);
    assert(ph_ota_version_compare(&right,&left)==-expected);
}

int main(void)
{
    const char *valid[]={
        "0.0.0","1.0.0","12.34.56","4294967295.0.0",
        "1.0.0-rc","1.0.0-rc.0","1.0.0-rc.10","1.0.0-rc.4294967295",
        "1.0.0-abcdef","0.9.27-012345-dirty","1.0.0-ABCDEF",
        "1.0.0-0123456789abcdef0123456789abcdef01234567",
        "1.0.0-rc-abcdef","1.0.0-rc.1-abcdef-dirty",
        "1.0.0+build.001","1.0.0+build-x.y","1.0.0-rc+meta.2",
        "1.0.0-rc.2+meta.1","1.0.0-abcdef+build.01"
    };
    const char *invalid[]={
        "","v1.0.0","1.0","1.0.0.0","01.0.0","1.00.0","1.0.00",
        "4294967296.0.0","1.0.0-rc.4294967296","99999999999999999.0.0",
        " 1.0.0","1.0.0 ","1.0.0\n","1.0.0-","1.0.0-rc.",
        "1.0.0-r","1.0.0-rcx","1.0.0-RC","1.0.0-beta","1.0.0-garbage",
        "1.0.0-rc.01","1.0.0-rc.-1","1.0.0-rc.1.2","1.0.0-rc..1",
        "1.0.0-rc.1-beta","1.0.0-rc-dirty","1.0.0-rc-","1.0.0-rc-abcdeg",
        "1.0.0-abcde","1.0.0-abcdefg","1.0.0-abcdef-extra",
        "1.0.0-abcdef-dirty-extra","1.0.0-abcdef-d","1.0.0-abcdef-dirtydirty",
        "1.0.0-0123456789abcdef0123456789abcdef012345678",
        "1.0.0+","1.0.0+.meta","1.0.0+meta.","1.0.0+meta..x",
        "1.0.0+meta_x","1.0.0+meta+x","1.0.0-rc+meta/one"
    };
    ph_ota_version_t value;
    assert(!ph_ota_version_parse(NULL,&value) && !ph_ota_version_parse("1.0.0",NULL));
    for(unsigned i=0;i<sizeof(valid)/sizeof(*valid);i++)assert(ph_ota_version_parse(valid[i],&value));
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
        memset(&value,0x5a,sizeof(value));ph_ota_version_t before=value;
        assert(!ph_ota_version_parse(invalid[i],&value));
        assert(!memcmp(&value,&before,sizeof(value)));
    }
    order("1.0.0-rc-abcdef","1.0.0-123456",-1);
    order("1.0.0-rc","1.0.0-rc.0",-1);
    order("1.0.0-rc.0","1.0.0-rc.1",-1);
    order("1.0.0-rc.2","1.0.0-rc.10",-1);
    order("1.0.0-rc.4294967295","1.0.0",-1);
    order("1.0.0","1.0.1-rc",-1);
    order("0.9.99","1.0.0-rc",-1);
    order("1.9.99","1.10.0-rc",-1);
    order("1.0.0-rc-abcdef","1.0.0-rc-123456-dirty",0);
    order("1.0.0-abcdef","1.0.0-123456-dirty",0);
    order("1.0.0+meta.1","1.0.0+meta.2",0);
    order("1.0.0-rc.1-abcdef+meta","1.0.0-rc.1+other",0);
    puts("OTA versions: strict grammar, overflow, rc ordering, stable promotion and build compatibility PASS");
}
