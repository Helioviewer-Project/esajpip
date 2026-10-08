#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jpeg2000/hv_reader.h"
#include "jpeg2000/hv_render.h"

/* Exercise both allocation failures in the inventory and its cleanup. */
static int allocations_before_failure = -1;
static void *test_calloc(size_t count, size_t size) {
    if (allocations_before_failure == 0) return NULL;
    if (allocations_before_failure > 0) allocations_before_failure--;
    return calloc(count, size);
}
#define calloc test_calloc
#include "jpeg2000/hv_reader.c"
#undef calloc

static int failures;
static void check(int ok, const char *name) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", name); failures++; }
}
typedef struct { uint8_t data[4096]; size_t size; } bytes;
static void u32(bytes *b, uint32_t n) {
    for (int i = 24; i >= 0; i -= 8) b->data[b->size++] = (uint8_t)(n >> i);
}
static size_t start(bytes *b, uint32_t type) {
    size_t at = b->size; u32(b, 0); u32(b, type); return at;
}
static void end(bytes *b, size_t at) {
    uint32_t n = (uint32_t)(b->size - at);
    for (int i = 0; i < 4; i++) b->data[at+i] = (uint8_t)(n >> (24-8*i));
}
static void box(bytes *b, uint32_t type) { size_t at = start(b, type); end(b, at); }
static bytes file(int jpx, size_t codestreams) {
    bytes b = {{0}, 0}; size_t at;
    at = start(&b, HV_BOX_JP); u32(&b, 0x0d0a870a); end(&b, at);
    at = start(&b, HV_BOX_FTYP);
    u32(&b, jpx ? 0x6a707820 : 0x6a703220); u32(&b, 0);
    u32(&b, jpx ? 0x6a707820 : 0x6a703220); end(&b, at);
    at = start(&b, HV_BOX_JP2H); box(&b, HV_BOX_COLR); end(&b, at);
    for (size_t i = 0; i < codestreams; i++) box(&b, i % 2 ? HV_BOX_FTBL : HV_BOX_JP2C);
    return b;
}
static size_t registration(bytes *b, unsigned denominator, unsigned cs, unsigned sample) {
    size_t at = start(b, HV_BOX_CREG);
    uint8_t body[] = {0,(uint8_t)denominator,0,(uint8_t)denominator,
                     (uint8_t)(cs >> 8),(uint8_t)cs,(uint8_t)sample,(uint8_t)sample,0,0};
    memcpy(b->data+b->size, body, sizeof body); b->size += sizeof body;
    end(b, at); return at;
}
static void expect_error(bytes *b, const char *expected) {
    hv_presentation p = {0}, saved; size_t at = 777;
    p.layers = 123; saved = p;
    const char *error = hv_presentation_open(b->data, b->size, &p, &at);
    check(error && strcmp(error, expected) == 0, expected);
    check(memcmp(&p, &saved, sizeof p) == 0, "failed inventory unchanged");
    check(at != 777, "failure offset reported");
}
static void inventory(void) {
    bytes b = file(1, 3); hv_presentation p = {0}; size_t at = 777, h;
    hv_registration_entry e;
    check(!hv_presentation_open(b.data,b.size,&p,&at), "implicit JPX");
    check(p.codestreams == 3 && p.layers == 3 && at == 777, "implicit counts/offset");
    check(p.defaults.type == HV_BOX_JP2H && p.codestream_headers[2].type == 0,
          "default and absent codestream header");
    check(!hv_registration_read(&p.layer[2].registration,0,&e) && e.codestream == 2 &&
          e.sampling_x == 1 && e.alignment_x == 0, "implicit registration");
    hv_presentation_free(&p); hv_presentation_free(&p);
    check(!p.layer && !p.codestream_headers && !p.layers, "free clears inventory");
    h = start(&b, HV_BOX_JPCH); box(&b, HV_BOX_CMAP); end(&b,h);
    h = start(&b, HV_BOX_JPLH); box(&b, HV_BOX_COLR); registration(&b,2,2,2); end(&b,h);
    h = start(&b, HV_BOX_JPLH); end(&b,h);
    /* Unconsumed composition body is opaque, including invalid child framing. */
    h = start(&b, 0x636f6d70); b.data[b.size++] = 255; end(&b,h);
    check(!hv_presentation_open(b.data,b.size,&p,&at), "explicit reordered/equivalent");
    check(p.layers == 2 && p.codestreams == 3 && p.codestream_headers[0].type == HV_BOX_JPCH,
          "explicit layers exclude unused codestreams; retain headers");
    check(p.layer[0].header.start > p.codestream_headers[0].start &&
          p.layer[0].registration.entries >= b.data && p.layer[0].registration.entries < b.data+b.size,
          "ordered borrowed descriptors");
    check(!hv_registration_read(&p.layer[0].registration,0,&e) && e.codestream == 2 &&
          e.sampling_x == 2 && p.layer[0].registration.denominator_x == 2,
          "equivalent identity preserved");
    check(!hv_registration_read(&p.layer[1].registration,0,&e) && e.codestream == 1,
          "default registration in explicit layer");
    hv_presentation_free(&p);
    b = file(0,1);
    check(!hv_presentation_open(b.data,b.size,&p,&at) && p.layers == 1, "JP2 implicit layer");
    hv_presentation_free(&p);
    b = file(1,2); h = start(&b,HV_BOX_JPLH);
    size_t r = registration(&b,3,1,2);
    uint8_t second[] = {0,0,4,5,1,2};
    memcpy(b.data+b.size,second,sizeof second); b.size += sizeof second; end(&b,r); end(&b,h);
    check(!hv_presentation_open(b.data,b.size,&p,&at), "scaled multi-source registration");
    check(p.layer[0].registration.count == 2 &&
          !hv_registration_read(&p.layer[0].registration,1,&e) && e.codestream == 0 &&
          e.sampling_x == 4 && e.sampling_y == 5 && e.alignment_x == 1 && e.alignment_y == 2,
          "all registration fields retained");
    hv_registration_entry saved = e;
    check(hv_registration_read(&p.layer[0].registration,2,&e) && !memcmp(&saved,&e,sizeof e),
          "entry bounds/output unchanged");
    hv_presentation_free(&p);
}
static void malformed(void) {
    bytes b; size_t h,r;
    b=file(1,1); h=start(&b,HV_BOX_JPLH); registration(&b,1,1,1); end(&b,h);
    expect_error(&b,"layer codestream reference out of range");
    b=file(1,1); box(&b,HV_BOX_JPLH); box(&b,HV_BOX_JPLH);
    expect_error(&b,"layer codestream reference out of range");
    b=file(1,1); h=start(&b,HV_BOX_JPLH); registration(&b,1,0,1); registration(&b,1,0,1); end(&b,h);
    expect_error(&b,"duplicate layer registration");
    b=file(1,1); h=start(&b,HV_BOX_JPLH); registration(&b,0,0,1); end(&b,h);
    expect_error(&b,"creg.denominator");
    b=file(1,1); h=start(&b,HV_BOX_JPLH); registration(&b,1,0,0); end(&b,h);
    expect_error(&b,"creg.sampling");
    b=file(1,1); h=start(&b,HV_BOX_JPLH); r=registration(&b,1,0,1);
    b.data[r+16]=1; end(&b,h); expect_error(&b,"creg.alignment");
    b=file(1,1); h=start(&b,HV_BOX_JPLH); r=registration(&b,1,0,1);
    b.size--; end(&b,r); end(&b,h); expect_error(&b,"creg.extent");
    b=file(1,1); h=start(&b,HV_BOX_JPLH); b.data[b.size++]=0; end(&b,h);
    /* The exact framing error belongs to hv_boxes_next. */
    hv_presentation p={0}; size_t at=0;
    check(hv_presentation_open(b.data,b.size,&p,&at)!=NULL, "bounded layer children");
    b=file(1,2);
    for (int i=0; i<2; i++) {
        allocations_before_failure=i;
        expect_error(&b,"out of memory");
    }
    allocations_before_failure=-1;
    b=file(1,1); box(&b,HV_BOX_JP2H); expect_error(&b,"duplicate presentation defaults");
    b=file(1,0); expect_error(&b,"presentation header or codestream count");
    b=file(1,1); box(&b,HV_BOX_JPCH); box(&b,HV_BOX_JPCH);
    expect_error(&b,"presentation header or codestream count");
    b=file(0,2); expect_error(&b,"presentation header or codestream count");
    b=file(1,1); box(&b,HV_BOX_JCLX);
    expect_error(&b,"unsupported JPX codestream or repeated container");
    b=file(1,1); box(&b,HV_BOX_J2CX);
    expect_error(&b,"unsupported JPX codestream or repeated container");
    b=file(1,1); b.size--; check(hv_presentation_open(b.data,b.size,&p,&at)!=NULL,"truncated top-level box");
    /* Standalone registration failure also preserves output and success offset. */
    b=(bytes){{0},0}; r=registration(&b,1,0,1);
    hv_box reg={0}; hv_boxes it; const char *error;
    hv_boxes_file(&it,b.data,b.size); check(hv_boxes_next(&it,&reg,&error,&at)==1,"registration box");
    hv_registration registration_value={0}; at=777;
    check(!hv_registration_open(b.data,&reg,&registration_value,&at) && at==777,"registration success offset");
    hv_registration saved=registration_value;
    b.data[r+8]=0; b.data[r+9]=0;
    check(hv_registration_open(b.data,&reg,&registration_value,&at)!=NULL &&
          !memcmp(&saved,&registration_value,sizeof saved),"registration failure unchanged");
}
static void inherited_headers(void) {
    bytes b=file(1,0); size_t defaults=32, h, group, at=777;
    hv_presentation p={0}; hv_presentation_headers headers={0}, saved;
    size_t default_palette=b.size; box(&b,HV_BOX_PCLR);
    size_t default_mapping=b.size; box(&b,HV_BOX_CMAP);
    size_t default_channels=b.size; box(&b,HV_BOX_CDEF);
    size_t default_format=b.size; box(&b,HV_BOX_PXFM); end(&b,defaults);
    for (int i=0; i<3; i++) box(&b,HV_BOX_JP2C);
    h=start(&b,HV_BOX_JPCH); size_t own_palette=b.size; box(&b,HV_BOX_PCLR); end(&b,h);
    h=start(&b,HV_BOX_JPCH); size_t own_mapping=b.size; box(&b,HV_BOX_CMAP); end(&b,h);
    box(&b,HV_BOX_JPCH);
    h=start(&b,HV_BOX_JPLH); group=start(&b,HV_BOX_CGRP);
    box(&b,HV_BOX_COLR); box(&b,HV_BOX_COLR); end(&b,group);
    size_t own_channels=b.size; box(&b,HV_BOX_CDEF); registration(&b,1,1,1); end(&b,h);
    h=start(&b,HV_BOX_JPLH); size_t opacity=b.size; box(&b,HV_BOX_OPCT); end(&b,h);
    h=start(&b,HV_BOX_JPLH); size_t own_format=b.size; box(&b,HV_BOX_PXFM); end(&b,h);
    check(!hv_presentation_open(b.data,b.size,&p,&at),"inheritance inventory");
    check(!hv_presentation_read_headers(b.data,&p,0,0,&headers,&at) && at==777,
          "inherited headers success offset");
    check(headers.codestream==1 && headers.mapping.start==own_mapping &&
          headers.palette.start==default_palette,"registered source overrides mapping independently");
    check(headers.colour.start==group && headers.channels.start==own_channels &&
          !headers.opacity.type && headers.pixel_format.start==default_format,
          "layer colour group/channel override/default pixel format");
    hv_boxes colours; hv_box colour; const char *error; int count=0;
    hv_boxes_children(&colours,b.data,&headers.colour);
    while(hv_boxes_next(&colours,&colour,&error,&at)==1) if(colour.type==HV_BOX_COLR) count++;
    check(count==2,"retain all colour alternatives");
    check(!hv_presentation_read_headers(b.data,&p,1,0,&headers,&at) &&
          headers.opacity.start==opacity && !headers.channels.type &&
          headers.colour.start==defaults,"local opacity suppresses default channel definition");
    check(!hv_presentation_read_headers(b.data,&p,2,0,&headers,&at) &&
          headers.channels.start==default_channels && headers.pixel_format.start==own_format &&
          headers.mapping.start==default_mapping,"independent default header properties");
    /* Read source 0 through a different layer to exercise palette-only override. */
    p.layer[0].registration=(hv_registration){NULL,1,0,1,1};
    check(!hv_presentation_read_headers(b.data,&p,0,0,&headers,&at) &&
          headers.palette.start==own_palette && headers.mapping.start==default_mapping,
          "palette-only override inherits mapping");
    saved=headers;
    check(hv_presentation_read_headers(b.data,&p,3,0,&headers,&at)!=NULL &&
          !memcmp(&headers,&saved,sizeof headers),"invalid layer preserves headers");
    check(hv_presentation_read_headers(b.data,&p,0,1,&headers,&at)!=NULL &&
          !memcmp(&headers,&saved,sizeof headers),"invalid registration source preserves headers");
    hv_presentation_free(&p);
    /* CREF only matters when it supplies an interpreted presentation property. */
    b=file(1,1); h=start(&b,HV_BOX_JPCH); size_t reference=start(&b,HV_BOX_CREF);
    u32(&b,HV_BOX_XML); end(&b,reference); end(&b,h);
    check(!hv_presentation_open(b.data,b.size,&p,&at) &&
          !hv_presentation_read_headers(b.data,&p,0,0,&headers,&at),"unconsumed metadata reference opaque");
    b.data[reference+8]='p'; b.data[reference+9]='c'; b.data[reference+10]='l'; b.data[reference+11]='r';
    saved=headers;
    error=hv_presentation_read_headers(b.data,&p,0,0,&headers,&at);
    check(error && !strcmp(error,"unsupported presentation cross-reference") &&
          at==reference && !memcmp(&headers,&saved,sizeof headers),"palette reference cannot silently inherit");
    hv_presentation_free(&p);
    b=file(1,1); h=start(&b,HV_BOX_JPLH); group=start(&b,HV_BOX_CGRP);
    reference=start(&b,HV_BOX_CREF); u32(&b,HV_BOX_COLR); end(&b,reference); end(&b,group); end(&b,h);
    check(!hv_presentation_open(b.data,b.size,&p,&at),"referenced colour inventory remains readable");
    error=hv_presentation_read_headers(b.data,&p,0,0,&headers,&at);
    check(error && !strcmp(error,"unsupported presentation cross-reference") && at==reference,
          "colour reference cannot silently disappear");
    hv_presentation_free(&p);
    b=file(1,1); h=start(&b,HV_BOX_JPLH); box(&b,HV_BOX_CDEF); box(&b,HV_BOX_OPCT); end(&b,h);
    check(!hv_presentation_open(b.data,b.size,&p,&at),"conflicting channel inventory");
    error=hv_presentation_read_headers(b.data,&p,0,0,&headers,&at);
    check(error && !strcmp(error,"presentation has both channel definition and opacity"),
          "channel and opacity declarations mutually exclusive");
    hv_presentation_free(&p);
    b=file(1,1); h=start(&b,HV_BOX_JPLH); group=start(&b,HV_BOX_CGRP);
    b.data[b.size++]=0; end(&b,group); end(&b,h);
    check(!hv_presentation_open(b.data,b.size,&p,&at),"uninterpreted colour group inventory");
    check(hv_presentation_read_headers(b.data,&p,0,0,&headers,&at)!=NULL,"bound interpreted colour group children");
    hv_presentation_free(&p);
}
static void channel_fields(void) {
    const uint8_t map_bytes[] = {0,2,1,5, 0xff,0xff,0,0, 0,3,7,255};
    const uint8_t def_bytes[] = {0,4, 0,2,0,0,0,1, 0,0,0,1,0,0,
                                0,1,0,2,0,3, 0xff,0xff,0xff,0xff,0xff,0xff};
    hv_component_mapping map = {0}, saved_map;
    hv_channel_definition def = {0}, saved_def;
    hv_component_mapping_entry m = {0}, saved_m;
    hv_channel_definition_entry d = {0}, saved_d;
    hv_box b = {0}; size_t at = (size_t)-1;
    b.type=HV_BOX_CMAP; b.end=sizeof map_bytes;
    check(!hv_component_mapping_open(map_bytes,&b,&map,&at) && map.count==3 &&
          map.entries==map_bytes && at==(size_t)-1,"borrowed component mapping");
    check(!hv_component_mapping_read(&map,0,&m) && m.component==2 && m.type==1 && m.column==5,
          "palette component and column");
    check(!hv_component_mapping_read(&map,1,&m) && m.component==65535 && m.type==0,
          "component reference stays numeric");
    check(!hv_component_mapping_read(&map,2,&m) && m.type==7 && m.column==255,
          "mapping access leaves unsupported method to consumer");
    saved_m=m;
    check(hv_component_mapping_read(&map,map.count,&m) && !memcmp(&m,&saved_m,sizeof m),
          "mapping invalid index preserves output");
    saved_map=map;
    for (size_t n=0;n<sizeof map_bytes;n++) {
        b.end=n; at=(size_t)-1;
        uint8_t *prefix=malloc(n?n:1); memcpy(prefix,map_bytes,n);
        const char *error=hv_component_mapping_open(prefix,&b,&map,&at);
        if (n && n%4==0)
            check(!error && map.count==n/4,"whole mapping prefix is an independent list");
        else
            check(error && at==0 && !memcmp(&map,&saved_map,sizeof map),"mapping truncated entry");
        free(prefix); map=saved_map;
    }
    b.type=HV_BOX_CDEF; b.end=sizeof def_bytes; at=(size_t)-1;
    check(!hv_channel_definition_open(def_bytes,&b,&def,&at) && def.count==4 &&
          def.entries==def_bytes+2 && at==(size_t)-1,"borrowed channel definitions");
    for (size_t i=0;i<3;i++)
        check(!hv_channel_definition_read(&def,i,&d) && d.type==i &&
              d.channel==(i==0?2:i-1) && d.association==(i==0?1:i==1?0:3),
              "colour, opacity and premultiplied fields");
    check(!hv_channel_definition_read(&def,3,&d) && d.channel==65535 &&
          d.type==65535 && d.association==65535,"unspecified channel fields retained");
    saved_d=d;
    check(hv_channel_definition_read(&def,def.count,&d) && !memcmp(&d,&saved_d,sizeof d),
          "definition invalid index preserves output");
    saved_def=def;
    for(size_t n=0;n<sizeof def_bytes;n++) {
        b.end=n; at=(size_t)-1;
        uint8_t *prefix=malloc(n?n:1); memcpy(prefix,def_bytes,n);
        check(hv_channel_definition_open(prefix,&b,&def,&at) && at==0 &&
              !memcmp(&def,&saved_def,sizeof def),"every counted definition prefix rejected");
        free(prefix);
    }
    uint8_t zero[]={0,0}, extra[sizeof def_bytes+1];
    memcpy(extra,def_bytes,sizeof def_bytes); extra[sizeof def_bytes]=0;
    b.end=sizeof zero;
    check(hv_channel_definition_open(zero,&b,&def,&at)!=NULL,"empty definition rejected");
    b.end=sizeof extra;
    check(hv_channel_definition_open(extra,&b,&def,&at)!=NULL,"definition trailing bytes rejected");
    size_t large_size=2+6*(size_t)65535;
    uint8_t *large=calloc(large_size,1);
    if (!large) { check(0,"large channel fixture allocation"); return; }
    large[0]=large[1]=255;
    large[large_size-6]=0x80; large[large_size-3]=3;
    b.end=large_size;
    check(!hv_channel_definition_open(large,&b,&def,&at) && def.count==65535 &&
          !hv_channel_definition_read(&def,65534,&d) && d.channel==32768 && d.type==3,
          "full definition count and uninterpreted reserved role");
    b.type=HV_BOX_CMAP; b.end=4*(size_t)65536;
    check(!hv_component_mapping_open(large,&b,&map,&at) && map.count==65536 &&
          !hv_component_mapping_read(&map,65535,&m),"mapping list has no corpus count cap");
    free(large);
    b.type=HV_BOX_XML; b.end=sizeof def_bytes;
    check(hv_channel_definition_open(def_bytes,&b,&def,&at)!=NULL &&
          hv_component_mapping_open(map_bytes,&b,&map,&at)!=NULL,"wrong channel box type");
}
static void colour_fields(void) {
    uint8_t body[] = {1,128,4,0,0,0,16,9,8};
    hv_box box = {0}; box.type=HV_BOX_COLR; box.end=sizeof body;
    hv_colour colour={0}, saved; size_t at=(size_t)-1;
    check(!hv_colour_read(body,&box,&colour,&at) && colour.method==1 &&
          colour.precedence==-128 && colour.approximation==4 && colour.colour_space==16 &&
          colour.data==body+7 && colour.size==2 && at==(size_t)-1,"enumerated colour and borrowed parameters");
    saved=colour;
    for(size_t n=0;n<7;n++) {
        uint8_t *prefix=malloc(n?n:1); memcpy(prefix,body,n); box.end=n;
        check(hv_colour_read(prefix,&box,&colour,&at) && at==0 &&
              !memcmp(&colour,&saved,sizeof colour),"every enumerated colour prefix rejected");
        free(prefix);
    }
    box.end=sizeof body;
    for(unsigned method=2;method<=4;method++) {
        body[0]=(uint8_t)method; body[1]=127;
        check(!hv_colour_read(body,&box,&colour,&at) && colour.method==method &&
              colour.precedence==127 && colour.colour_space==0 && colour.data==body+3 &&
              colour.size==6,"ICC/vendor colour body remains opaque");
    }
    body[0]=255; body[1]=255; box.end=3;
    check(!hv_colour_read(body,&box,&colour,&at) && colour.method==255 && colour.precedence==-1 &&
          colour.size==0,"unknown colour method remains accessible");
    body[0]=1; body[3]=0xff; body[4]=0xff; body[5]=0xff; body[6]=0xff; box.end=7;
    check(!hv_colour_read(body,&box,&colour,&at) && colour.colour_space==UINT32_MAX,
          "full enumerated colour space field");
    box.type=HV_BOX_XML;
    check(hv_colour_read(body,&box,&colour,&at)!=NULL,"wrong colour box type");
}
static void palette_fields(void) {
    const hv_palette_sample middle[]={{2,3,0},{341,10,0},{682,10,0},{-171,10,1}};
    const uint8_t display[]={73,85,171,85};
    for(size_t i=0;i<sizeof display;i++)
        check(hv_palette_byte(&middle[i])==display[i],"palette display rounding");
    hv_box box={0}; box.type=HV_BOX_PCLR;
    hv_palette palette={0}, saved; hv_palette_sample sample={0}, saved_sample;
    size_t at;
    for(unsigned bits=1;bits<=38;bits++) {
        unsigned width=(bits+7)/8; size_t size=5+4*width;
        uint8_t body[25]={0,2,2,0,0};
        body[3]=(uint8_t)(bits-1); body[4]=(uint8_t)(128+bits-1);
        uint64_t raw[]={0,UINT64_C(1)<<(bits-1), (UINT64_C(1)<<bits)-1,
                        (UINT64_C(1)<<(bits-1))-1};
        for(unsigned v=0;v<4;v++) {
            for(unsigned b=0;b<width;b++) body[5+v*width+b]=(uint8_t)(raw[v]>>(8*(width-1-b)));
            if(bits%8) body[5+v*width]|=(uint8_t)(255u<<(bits%8));
        }
        box.end=size; at=(size_t)-1;
        check(!hv_palette_open(body,&box,&palette,&at) && palette.entry_count==2 &&
              palette.column_count==2 && palette.row_bytes==2*width &&
              palette.depths==body+3 && palette.values==body+5 && at==(size_t)-1,
              "borrowed mixed signed palette layout");
        for(size_t e=0;e<2;e++) for(size_t c=0;c<2;c++) {
            int64_t expected=c ? (e ? (INT64_C(1)<<(bits-1))-1 : -(INT64_C(1)<<(bits-1)))
                               : (e ? (INT64_C(1)<<bits)-1 : 0);
            check(!hv_palette_read(&palette,e,c,&sample) && sample.value==expected &&
                  sample.bits==bits && sample.is_signed==(int)c,"exact palette value and padding mask");
            check(hv_palette_byte(&sample)==(e?255:0),"palette display endpoints");
        }
        saved_sample=sample;
        check(hv_palette_read(&palette,2,0,&sample) && !memcmp(&sample,&saved_sample,sizeof sample) &&
              hv_palette_read(&palette,0,2,&sample) && !memcmp(&sample,&saved_sample,sizeof sample),
              "palette invalid indices preserve output");
        saved=palette;
        for(size_t n=0;n<size;n++) {
            uint8_t *prefix=malloc(n?n:1); memcpy(prefix,body,n); box.end=n;
            check(hv_palette_open(prefix,&box,&palette,&at) && !memcmp(&palette,&saved,sizeof palette),
                  "every exact-sized palette prefix rejected"); free(prefix);
        }
        box.end=size+1;
        uint8_t extra[26]; memcpy(extra,body,size); extra[size]=0;
        check(hv_palette_open(extra,&box,&palette,&at)!=NULL,"palette trailing bytes rejected");
    }
    uint8_t invalid[]={0,1,1,38,0,0,0,0,0}; box.end=sizeof invalid; at=(size_t)-1;
    check(hv_palette_open(invalid,&box,&palette,&at) && at==3,"unsupported palette depth location");
    invalid[3]=7; invalid[0]=4; invalid[1]=1;
    check(hv_palette_open(invalid,&box,&palette,&at)!=NULL,"palette entry count exceeds standard bound");
    invalid[0]=invalid[1]=0;
    check(hv_palette_open(invalid,&box,&palette,&at)!=NULL,"zero palette entries");
    invalid[1]=1; invalid[2]=0;
    check(hv_palette_open(invalid,&box,&palette,&at)!=NULL,"zero palette columns");
    box.type=HV_BOX_XML;
    check(hv_palette_open(invalid,&box,&palette,&at)!=NULL,"wrong palette box type");
    size_t large_size=3+255+1024*(size_t)255*5;
    uint8_t *large=calloc(large_size,1);
    if(!large){check(0,"large palette fixture allocation");return;}
    large[0]=4; large[2]=255; memset(large+3,37,255); large[large_size-1]=123;
    box.type=HV_BOX_PCLR; box.end=large_size;
    check(!hv_palette_open(large,&box,&palette,&at) && palette.entry_count==1024 &&
          palette.column_count==255 && !hv_palette_read(&palette,1023,254,&sample) &&
          sample.value==123 && sample.bits==38,"maximum palette counts and final sample");
    free(large);
}
static void colour_box(bytes *b, unsigned space, int precedence) {
    size_t h=start(b,HV_BOX_COLR);
    b->data[b->size++]=1; b->data[b->size++]=(uint8_t)precedence; b->data[b->size++]=1;
    u32(b,space); end(b,h);
}
static void body_box(bytes *b,uint32_t type,const uint8_t *body,size_t size) {
    size_t h=start(b,type); memcpy(b->data+b->size,body,size); b->size+=size; end(b,h);
}
static bytes render_file(int jpx) {
    bytes b={{0},0}; size_t h;
    h=start(&b,HV_BOX_JP); u32(&b,0x0d0a870a); end(&b,h);
    h=start(&b,HV_BOX_FTYP);u32(&b,jpx?0x6a707820:0x6a703220);u32(&b,0);
    u32(&b,jpx?0x6a707820:0x6a703220);end(&b,h);
    h=start(&b,HV_BOX_JP2H);colour_box(&b,16,0);end(&b,h);
    box(&b,HV_BOX_JP2C);return b;
}
static void render_result(bytes *b,size_t components,hv_render *render,const char *expected) {
    hv_presentation p={0};size_t at=777;hv_render saved=*render;
    check(!hv_presentation_open(b->data,b->size,&p,&at),"render test inventory");
    const char *error=hv_render_read(b->data,&p,0,components,render,&at);
    if(expected)
        check(error && !strcmp(error,expected) && at!=777 && !memcmp(render,&saved,sizeof saved),expected);
    else check(!error && at==777,"supported render instructions");
    hv_presentation_free(&p);
}
static void render_channels(void) {
    bytes b=render_file(1);hv_render render={0};size_t h,g;
    render_result(&b,3,&render,NULL);
    check(render.colour_space==16 && render.channel_count==3 && render.codestream==0 &&
          render.channel[0].component==0 && render.channel[2].component==2 &&
          render.channel[0].palette_column==-1,"direct RGB instructions");
    render_result(&b,2,&render,"incomplete colour channel definitions");
    render_result(&b,0,&render,"no decoder output components");
    h=start(&b,HV_BOX_JPLH);g=start(&b,HV_BOX_CGRP);
    colour_box(&b,17,-128);colour_box(&b,16,127);colour_box(&b,17,126);end(&b,g);end(&b,h);
    render_result(&b,3,&render,NULL);
    check(render.colour_space==16,"highest supported signed precedence");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);g=start(&b,HV_BOX_CGRP);
    colour_box(&b,17,0);colour_box(&b,16,0);colour_box(&b,18,127);end(&b,g);end(&b,h);
    render_result(&b,1,&render,NULL);
    check(render.colour_space==17 && render.channel_count==1,"supported fallback and first equal precedence");
    b=render_file(0);
    /* Add alternatives inside JP2H; JP2 ignores precedence. */
    size_t original=12+20;
    /* Rebuild to keep the child extents simple. */
    b.size=original;h=start(&b,HV_BOX_JP2H);
    colour_box(&b,17,-128);colour_box(&b,16,127);end(&b,h);box(&b,HV_BOX_JP2C);
    render_result(&b,1,&render,NULL);check(render.colour_space==17,"JP2 ignores precedence");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);
    const uint8_t reorder[]={0,3,0,0,0,0,0,3, 0,1,0,0,0,2, 0,2,0,0,0,1};
    body_box(&b,HV_BOX_CDEF,reorder,sizeof reorder);end(&b,h);
    render_result(&b,3,&render,NULL);
    check(render.channel[0].component==2 && render.channel[1].component==1 &&
          render.channel[2].component==0,"CDEF reorders output components");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);
    const uint8_t all[]={0,1,0,0,0,0,0,0};body_box(&b,HV_BOX_CDEF,all,sizeof all);end(&b,h);
    render_result(&b,1,&render,NULL);
    check(render.channel[0].component==0 && render.channel[2].component==0,"whole-image colour association");
    b=render_file(1);h=start(&b,HV_BOX_JPCH);
    const uint8_t palette[]={0,2,3,7,7,7, 1,2,3, 4,5,6};
    const uint8_t mapping[]={0,1,1,2, 0,1,1,0, 0,0,0,255, 255,255,7,255};
    body_box(&b,HV_BOX_PCLR,palette,sizeof palette);body_box(&b,HV_BOX_CMAP,mapping,sizeof mapping);end(&b,h);
    render_result(&b,2,&render,NULL);
    check(render.channel[0].component==1 && render.channel[0].palette_column==2 &&
          render.channel[1].palette_column==0 && render.channel[2].component==0 &&
          render.channel[2].palette_column==-1 && render.palette.entry_count==2,
          "mixed direct and reordered palette channels; unused mapping ignored");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);body_box(&b,HV_BOX_OPCT,(const uint8_t[]){0},1);end(&b,h);
    render_result(&b,3,&render,"unsupported opacity");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);
    const uint8_t alpha[]={0,1,0,0,0,1,0,0};body_box(&b,HV_BOX_CDEF,alpha,sizeof alpha);end(&b,h);
    render_result(&b,3,&render,"unsupported opacity");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);box(&b,HV_BOX_PXFM);end(&b,h);
    render_result(&b,3,&render,"unsupported pixel format");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);g=registration(&b,2,0,2);end(&b,g);end(&b,h);
    render_result(&b,3,&render,NULL);
    b.data[g+14]=1;render_result(&b,3,&render,"unsupported nonidentity registration");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);g=registration(&b,1,0,1);
    const uint8_t another[]={0,0,1,1,0,0};memcpy(b.data+b.size,another,sizeof another);b.size+=sizeof another;
    end(&b,g);end(&b,h);render_result(&b,3,&render,"unsupported multiple-codestream composition");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);g=start(&b,HV_BOX_CGRP);colour_box(&b,18,0);end(&b,g);end(&b,h);
    render_result(&b,3,&render,"unsupported colour conversion");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);box(&b,HV_BOX_CGRP);end(&b,h);
    render_result(&b,3,&render,"missing colour description");
    b=render_file(1);h=start(&b,HV_BOX_JPCH);
    const uint8_t bad_map[]={0,3,0,0,0,0,0,0,0,0,0,0};body_box(&b,HV_BOX_CMAP,bad_map,sizeof bad_map);end(&b,h);
    render_result(&b,3,&render,"decoder output component out of range");
    b=render_file(1);h=start(&b,HV_BOX_JPCH);
    body_box(&b,HV_BOX_CMAP,(const uint8_t[]){0,0,1,0,0,0,1,0,0,0,1,0},12);end(&b,h);
    render_result(&b,1,&render,"missing mapped palette");
    b=render_file(1);h=start(&b,HV_BOX_JPCH);
    body_box(&b,HV_BOX_PCLR,palette,sizeof palette);
    const uint8_t bad_column[]={0,0,1,3,0,0,1,0,0,0,1,0};
    body_box(&b,HV_BOX_CMAP,bad_column,sizeof bad_column);end(&b,h);
    render_result(&b,1,&render,"palette column out of range");
    b=render_file(1);h=start(&b,HV_BOX_JPCH);
    body_box(&b,HV_BOX_CMAP,(const uint8_t[]){0,0,7,0,0,0,0,0,0,0,0,0},12);end(&b,h);
    render_result(&b,3,&render,"unsupported component mapping method");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);
    const uint8_t duplicate[]={0,2,0,0,0,0,0,1,0,1,0,0,0,1};
    body_box(&b,HV_BOX_CDEF,duplicate,sizeof duplicate);end(&b,h);
    render_result(&b,3,&render,"duplicate colour channel association");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);
    const uint8_t bad_channel[]={0,1,0,3,0,0,0,1};
    body_box(&b,HV_BOX_CDEF,bad_channel,sizeof bad_channel);end(&b,h);
    render_result(&b,3,&render,"colour channel reference out of range");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);
    const uint8_t short_def[]={0,1,0,0,0,0,0,1};
    body_box(&b,HV_BOX_CDEF,short_def,sizeof short_def);end(&b,h);
    render_result(&b,3,&render,"incomplete colour channel definitions");
    b=render_file(1);h=start(&b,HV_BOX_JPLH);
    const uint8_t ignore[]={0,4,0,0,0,0,0,1,0,1,0,0,0,2,0,2,0,0,0,3,
                            255,255,0,1,255,255};
    body_box(&b,HV_BOX_CDEF,ignore,sizeof ignore);end(&b,h);
    render_result(&b,3,&render,NULL);
    check(render.channel[2].component==2,"unassociated opacity entry ignored");
    b=render_file(1);h=start(&b,HV_BOX_JPCH);box(&b,HV_BOX_PCLR);end(&b,h);
    render_result(&b,3,&render,NULL);
    check(!render.palette.entry_count,"unused palette payload stays opaque");
    hv_presentation p={0};size_t at=777;
    check(!hv_presentation_open(b.data,b.size,&p,&at),"invalid render index inventory");
    hv_render saved=render;
    const char *error=hv_render_read(b.data,&p,p.layers,3,&render,&at);
    check(error && !strcmp(error,"presentation layer index out of range") &&
          !memcmp(&render,&saved,sizeof render),"render index failure preserves output");
    hv_presentation_free(&p);

}
int main(void) { inventory(); malformed(); inherited_headers(); channel_fields(); colour_fields(); palette_fields(); render_channels(); return failures ? 1 : 0; }
