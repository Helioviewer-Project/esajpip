#define _XOPEN_SOURCE 700

#include "jpeg2000/hv_metadata.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "client/hvc_decode.h"
#include "jpeg2000/hv_local.h"

static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "%s\n", message); exit(1); }
}
typedef struct { int calls, mode; } inspector;
static int inspect(hvc *source, size_t frame, void *context,
                    hvc_info *info, char *error, size_t error_size) {
    inspector *state = context;
    uint8_t prefix[6];
    state->calls++;
    if (state->mode == 1) { snprintf(error, error_size, "decoder failure"); return -1; }
    hvc_input *input = hvc_input_open(source, frame);
    if (!input) { snprintf(error, error_size, "%s", hvc_error(source)); return -1; }
    check(hvc_input_read(input, 0, prefix, sizeof prefix) == 6 &&
          prefix[0] == 0xff && prefix[1] == 0x4f, "inspector reads source");
    hvc_input_close(input);
    /* Controlled decoder geometry: a nonzero-origin grid need not reduce as
     * ceil(full width/2). Actual decoder geometry is checked by the KDU probe. */
    *info = (hvc_info){.components=3, .resolutions=3, .layers=7,
                            .width={129,64,32}, .height={131,65,32}};
    if (state->mode == 2) info->resolutions = 34;
    if (state->mode == 3) info->width[1] = 130;
    if (state->mode == 4) info->height[2] = 0;
    if (state->mode == 5) info->layers = 0;
    if (state->mode == 6) info->components = 0;
    if (state->mode == 7) info->resolutions = 0;
    return 0;
}
static size_t read_frame(hvc *client, uint64_t frame, size_t offset, uint8_t *out, size_t size) {
    hvc_input *input = hvc_input_open(client, frame);
    if (!input) return 0;
    size_t result = hvc_input_read(input, offset, out, size);
    hvc_input_close(input);
    return result;
}
typedef struct { uint8_t data[4096]; size_t size; } boxes;
static void u16(boxes *b, unsigned value) {
    b->data[b->size++] = value >> 8; b->data[b->size++] = value;
}
static void u32(boxes *b, uint32_t value) { u16(b,value>>16); u16(b,value); }
static size_t begin(boxes *b, const char *type) {
    size_t start=b->size; u32(b,0); memcpy(b->data+b->size,type,4); b->size+=4; return start;
}
static void end(boxes *b, size_t start) {
    uint32_t size=b->size-start;
    for (int i=0;i<4;i++) b->data[start+i]=(uint8_t)(size>>(24-8*i));
}
static void box(boxes *b, const char *type, const void *data, size_t size) {
    check(size<=sizeof b->data-b->size-8,"fixture overflow");
    size_t start=begin(b,type); memcpy(b->data+b->size,data,size); b->size+=size; end(b,start);
}
static void test_layer_order(int mode) {
    boxes b={0}; static const uint8_t signature[]=HV_SIGNATURE_BYTES;
    box(&b,"jP  ",signature,4); box(&b,"ftyp","jpx \0\0\0\1jpx ",12);
    size_t header=begin(&b,"jp2h"), image=begin(&b,"ihdr");
    u32(&b,2); u32(&b,2); u16(&b,1);
    const uint8_t format[]={7,7,0,0}; memcpy(b.data+b.size,format,4); b.size+=4; end(&b,image);
    const uint8_t color[]={1,0,0,0,0,0,17}; box(&b,"colr",color,sizeof color); end(&b,header);
    for (int cs=0;cs<2;cs++) {
        header=begin(&b,"jpch");
        const uint8_t palette[]={0,2,1,7,(uint8_t)(cs?9:2),(uint8_t)(cs?10:3)};
        const uint8_t mapping[]={0,0,1,0};
        box(&b,"pclr",palette,sizeof palette); box(&b,"cmap",mapping,sizeof mapping); end(&b,header);
    }
    int layers=mode==1?2:1;
    for (int layer=0;layer<layers;layer++) {
        header=begin(&b,"jplh"); image=begin(&b,"creg"); u16(&b,1); u16(&b,1);
        for (int i=0;i<(mode==2?2:1);i++) {
            u16(&b,mode==4?3:(mode==2?i:1-layer));
            const uint8_t registration[]={1,1,0,0};
            memcpy(b.data+b.size,registration,4); b.size+=4;
        }
        end(&b,image); end(&b,header);
    }
    if (mode==3) {
        header=begin(&b,"asoc"); image=begin(&b,"nlst"); u32(&b,0x02000000u); end(&b,image);
        box(&b,"xml ","layer-zero",10); end(&b,header);
    }
    for (int cs=0;cs<2;cs++) {
        uint8_t bytes[]={255,79,255,81,0,(uint8_t)(10+cs)}; box(&b,"jp2c",bytes,sizeof bytes);
        header=begin(&b,"asoc"); image=begin(&b,"nlst"); u32(&b,0x01000000u+cs); end(&b,image);
        const char *xml=cs?"stream-one":"stream-zero"; box(&b,"xml ",xml,strlen(xml)); end(&b,header);
    }
    char path[]="/tmp/hvc-layers-XXXXXX", error[256]; int fd=mkstemp(path);
    check(fd>=0,"temporary fixture open");
    check(write(fd,b.data,b.size)==(ssize_t)b.size && close(fd)==0,"temporary fixture write");
    inspector state={0}; hvc *client=hvc_open_local(path,inspect,&state,error,sizeof error);
    check(client!=NULL,error);
    hv_local *raw=hv_local_open(path,error,sizeof error);
    check(unlink(path)==0,"temporary fixture unlink"); check(raw && hv_local_codestreams(raw)==2,"borrowed raw codestream count");
    if (mode==4) {
        check(hvc_frames(client)==0 && hvc_error(client)[0],"invalid presentation cannot establish frames");
        char saved[256]; snprintf(saved,sizeof saved,"%s",hvc_error(client));
        const uint8_t *xml=(const uint8_t*)1; size_t size=99; uint8_t prefix[6];
        check(hvc_xml(client,0,&xml,&size)==-1 && xml==NULL && size==0 &&
              strcmp(hvc_error(client),saved)==0,"presentation error retained through frame XML query");
        check(hv_local_read(raw,0,0,prefix,sizeof prefix,error,sizeof error)==6 && prefix[5]==10,
              "presentation failure preserves independent raw bytes");
        check(hv_local_xml(raw,0,&xml,&size,error,sizeof error)==0 && xml && size==11,
              "presentation failure preserves independent raw metadata");
        hv_local_close(raw); hvc_destroy(client); return;
    }
    check(hvc_frames(client)==(size_t)layers && state.calls==0,"layer count independent of two codestreams");
    const uint8_t *raw_xml; size_t raw_size;
    check(hv_local_xml(raw,0,&raw_xml,&raw_size,error,sizeof error)==0 && raw_xml && raw_size==11 &&
          memcmp(raw_xml,"stream-zero",11)==0,"raw XML before layer indexing");
    uint8_t bytes[6];
    check(hv_local_read(raw,0,0,bytes,sizeof bytes,error,sizeof error)==6 && bytes[5]==10,"raw codestream ordering preserved");
    for (int layer=0;layer<layers;layer++) {
        const uint8_t *xml=NULL; size_t size=0; hvc_view view;
        check(hvc_xml(client,layer,&xml,&size)==0 && xml,"layer XML available");
        if (mode==2) {
            check(size==11 && memcmp(xml,"stream-zero",11)==0,"composition XML uses registered sources");
            check(hvc_status(client,layer,NULL,&view)==-1 && strstr(hvc_error(client),"composition"),"host rejects unsupported composition");
            check(hvc_codestream(client,layer,bytes,sizeof bytes)==0 && read_frame(client,layer,0,bytes,sizeof bytes)==0,"composition byte operations reject ambiguity");
            int channels=99; check(hvc_palette(client,layer,&channels,bytes,sizeof bytes)==-1 && channels==0,"composition palette rejected");
        } else {
            int cs=1-layer;
            check(hvc_status(client,layer,NULL,&view)==0,"layer decoder callback");
            check(hvc_status(client,layer,NULL,&view)==0 && state.calls==layer+1,"layer geometry cached independently");
            check(read_frame(client,layer,0,bytes,sizeof bytes)==6 && bytes[5]==10+cs,"frame reads resolve registered codestream");
            check(hvc_codestream(client,layer,bytes,sizeof bytes)==6 && bytes[5]==10+cs,"frame copies resolve registered codestream");
            const char *expected=mode==3?"layer-zero":(cs?"stream-one":"stream-zero");
            check(size==strlen(expected) && memcmp(xml,expected,size)==0,"frame XML follows registered codestream");
            int channels=0; check(hvc_palette(client,layer,&channels,bytes,sizeof bytes)==2 && channels==1 && bytes[0]==(cs?9:2),"frame palette follows registered codestream");
        }
    }
    check(raw_size==11 && memcmp(raw_xml,"stream-zero",11)==0,"borrowed raw XML survives layer reindexing");
    check(hv_local_xml(raw,0,&raw_xml,&raw_size,error,sizeof error)==0 && raw_size==11 &&
          memcmp(raw_xml,"stream-zero",11)==0,"raw XML remains independent of layer associations");
    const uint8_t *xml=(const uint8_t*)1; size_t size=99;
    check(hvc_xml(client,layers,&xml,&size)==-1 && xml==NULL && size==0,"layer metadata bounds clear outputs");
    hv_local_close(raw); hvc_destroy(client);
}

int main(void) {
    char error[256]; inspector state = {0};
    hvc *client = hvc_open_local(IMAGE, inspect, &state, error, sizeof error);
    check(client != NULL, error);
    check(hvc_frames(client) == 1 && state.calls == 0, "local count needs no decoder");
    hvc_view view; hvc_options options = {0};
    check(hvc_status(client, 0, NULL, &view) == 0 && view.ready && view.request == HVC_READY &&
          view.width == 129 && view.height == 131 && view.requested_layers == 7 &&
          view.source.complete == 3 && state.calls == 1, "full local view");
    options.fit_width=64; options.fit_height=65;
    check(hvc_status(client, 0, &options, &view) == 0 && view.reduce == 1 &&
          view.width == 64 && view.height == 65 && state.calls == 1, "exact decoder table and cached information");
    options.fit_width=65; options.fit_height=66;
    check(hvc_status(client, 0, &options, &view) == 0 && view.reduce == 0, "fit crossing exact dimension boundary");
    options=(hvc_options){.reduce=INT_MAX,.layers=99};
    check(hvc_status(client, 0, &options, &view) == 0 && view.reduce == 2 &&
          view.width == 32 && view.height == 32 && view.requested_layers == 7, "clamped local reduction and quality");
    options=(hvc_options){.reduce=1,.layers=2};
    check(hvc_status(client, 0, &options, &view) == 0 && view.requested_layers == 2 &&
          view.layers == 7 && view.ready, "quality limit is a decode instruction");
    options.fit_width=NAN;
    check(hvc_status(client, 0, &options, &view) == -1, "invalid local options");
    options=(hvc_options){0};
    check(hvc_status(client, UINT64_MAX, &options, &view) == -1, "invalid local frame");
    size_t n=hvc_codestream(client,0,NULL,0); uint8_t *bytes=malloc(n), prefix[100];
    check(n && bytes, "local byte allocation");
    memset(bytes,0xa5,n);
    check(hvc_codestream(client,0,bytes,n-1)==0 && bytes[0]==0xa5, "local copy capacity");
    check(hvc_codestream(client,0,bytes,n)==n, "local original bytes");
    check(read_frame(client,0,7,prefix,sizeof prefix)==sizeof prefix &&
          memcmp(prefix,bytes+7,sizeof prefix)==0, "client range reads");
    check(read_frame(client,UINT64_MAX,0,prefix,sizeof prefix)==0, "range frame bounds");
    size_t cursor=0; char model[100];
    check(hvc_prepare(client,0,NULL,&view)==0 && view.request==HVC_READY && hvc_response(client,NULL,0)==-1 &&
          hvc_model(client,&cursor,model,sizeof model)==-1 && hvc_restore_response(client,NULL,0)==-1,
          "local prepare needs no request; network-only operations are rejected");
    check(hvc_status(client,0,NULL,&view)==0 && view.ready, "JPIP rejection preserves local source");
    hvc_destroy(client);
    check(bytes[0]==0xff && bytes[1]==0x4f, "copied bytes survive destruction"); free(bytes);
    for (int mode=1;mode<=7;mode++) {
        state=(inspector){.mode=mode}; client=hvc_open_local(IMAGE,inspect,&state,error,sizeof error);
        check(client!=NULL,error);
        check(hvc_status(client,0,NULL,&view)==-1, "failed or invalid decoder info rejected");
        state.mode=0;
        check(hvc_status(client,0,NULL,&view)==0 && state.calls==2, "failed information is not cached; retry succeeds");
        hvc_destroy(client);
    }
    client=hvc_open_local(IMAGE,NULL,NULL,error,sizeof error); check(client!=NULL,error);
    check(hvc_frames(client)==1 && hvc_codestream(client,0,NULL,0)==n &&
          hvc_status(client,0,NULL,&view)==-1, "byte-only source needs no decoder dependency");
    hvc_destroy(client);
    check(hvc_open_local("/nonexistent/hvc-local-test",NULL,NULL,error,sizeof error)==NULL && error[0], "open failure");
    client=hvc_create(NULL, NULL); check(client!=NULL,"remote allocation");
    check(read_frame(client,0,0,prefix,sizeof prefix)==0,"input without remote headers rejected"); hvc_destroy(client);
    client=hvc_open_local(MOVIE,inspect,&state,error,sizeof error); check(client!=NULL,error);
    check(hvc_frames(client)==8,"local movie layer count");
    for (size_t frame=0;frame<8;frame++) {
        const uint8_t *xml=NULL; size_t size=0; int channels; uint8_t palette[HV_PALETTE_MAX];
        check(hvc_xml(client,frame,&xml,&size)==0 && xml && size,"local movie XML");
        check(hvc_palette(client,frame,&channels,palette,sizeof palette)>=0,"local movie palette");
        check(hvc_codestream(client,frame,NULL,0)>0,"local movie bytes");
    }
    hvc_destroy(client); hvc_destroy(NULL);
    for (int mode=0;mode<5;mode++) test_layer_order(mode);
    puts("local client checks passed"); return 0;
}
