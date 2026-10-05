#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "jpeg2000/hv_served.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "jpeg2000/hv_reader.h"
#include "jpeg2000/hv_render.h"

static int fail_alloc = -1, allocations, fail_io, io_index, io_calls, descriptors, mappings;
static int alloc_fails(void) { allocations++; return fail_alloc >= 0 && fail_alloc-- == 0; }
static void *test_calloc(size_t n, size_t s) { return alloc_fails() ? NULL : calloc(n,s); }
static void *test_malloc(size_t n) { return alloc_fails() ? NULL : malloc(n); }
static void *test_realloc(void *p,size_t n) { return alloc_fails() ? NULL : realloc(p,n); }
static int io_fails(int kind) {
    if (fail_io == kind && io_calls++ == io_index) { errno = EIO; return 1; }
    return 0;
}
static int test_open(const char *p, int flags) {
    int fd;
    if (io_fails(1)) return -1;
    fd = open(p,flags); if (fd >= 0) descriptors++; return fd;
}
static int test_close(int fd) { descriptors--; return close(fd); }
static int test_fstat(int fd, struct stat *st) { return io_fails(2) ? -1 : fstat(fd,st); }
static void *test_mmap(void *p,size_t n,int prot,int flags,int fd,off_t off) {
    void *result;
    if (io_fails(3)) return MAP_FAILED;
    result = mmap(p,n,prot,flags,fd,off); if (result != MAP_FAILED) mappings++; return result;
}
static int test_munmap(void *p,size_t n) { mappings--; return munmap(p,n); }
#define calloc test_calloc
#define malloc test_malloc
#define realloc test_realloc
#define open test_open
#define close test_close
#define fstat test_fstat
#define mmap test_mmap
#define munmap test_munmap
#include "jpeg2000/hv_reader.c"
#include "jpeg2000/hv_metadata.c"
#include "jpeg2000/hv_local.c"
#undef calloc
#undef malloc
#undef realloc
#undef open
#undef close
#undef fstat
#undef mmap
#undef munmap

static int failures;
static void check(int ok,const char *name) { if (!ok) { fprintf(stderr,"FAIL: %s\n",name); failures++; } }
typedef struct { uint8_t data[8192]; size_t size; } bytes;
static void put(bytes *b,const void *p,size_t n) {
    if (b->size+n > sizeof b->data) abort();
    if (n) memcpy(b->data+b->size,p,n); b->size+=n;
}
static void u16(bytes *b,unsigned n) { uint8_t p[]={n>>8,n}; put(b,p,2); }
static void u32(bytes *b,uint32_t n) { u16(b,n>>16); u16(b,n); }
static void u64(bytes *b,uint64_t n) { u32(b,n>>32); u32(b,n); }
static size_t begin(bytes *b,const char *type) { size_t at=b->size; u32(b,0); put(b,type,4); return at; }
static void end(bytes *b,size_t at) {
    uint32_t n=b->size-at;
    b->data[at]=n>>24; b->data[at+1]=n>>16; b->data[at+2]=n>>8; b->data[at+3]=n;
}
static void box(bytes *b,const char *type,const void *p,size_t n) { size_t at=begin(b,type); put(b,p,n); end(b,at); }
static void save(const char *path,const bytes *b) {
    FILE *f=fopen(path,"wb"); check(f!=NULL,"fixture open"); if (!f) exit(2);
    check(fwrite(b->data,1,b->size,f)==b->size,"fixture write"); check(fclose(f)==0,"fixture close");
}
static void signature(bytes *b,int jpx) {
    static const uint8_t sig[]=HV_SIGNATURE_BYTES;
    box(b,"jP  ",sig,4); box(b,"ftyp",jpx?"jpx \0\0\0\1jpx ":"jp2 \0\0\0\0jp2 ",12);
}
static void url_box(bytes *b,const char *loc) { size_t at=begin(b,"url "); u32(b,0); put(b,loc,strlen(loc)+1); end(b,at); }

/* Mixed top-level codestreams and two fragments spanning main/companion.
 * The unused URL is deliberately invalid: its body has no consumer. */
static const char *test_reference;
static uint64_t external_offset=12;
static size_t fragment_split;
static bytes movie(const uint8_t *cs,size_t n,int bad) {
    bytes b={0}; size_t mdat,ftbl,flst,dtbl, split=fragment_split?fragment_split:n/2;
    signature(&b,1);
    box(&b,"jp2c",cs,n);
    mdat=begin(&b,"mdat"); put(&b,cs,split); end(&b,mdat);
    ftbl=begin(&b,"ftbl"); flst=begin(&b,"flst"); u16(&b,bad==7?0:2);
    if (bad!=7) {
        u64(&b,bad==4?12:mdat+8); u32(&b,split); u16(&b,0);
        u64(&b,bad==2?UINT64_MAX:external_offset); u32(&b,bad==3?UINT32_MAX:n-split); u16(&b,bad==1?3:1);
    }
    end(&b,flst); end(&b,ftbl);
    dtbl=begin(&b,"dtbl"); u16(&b,2);
    url_box(&b,bad==0 && test_reference?test_reference:bad==8?"https://example.org/part":bad==9?"part%00data.bin":bad==10?"missing.bin":"part%20data.bin");
    box(&b,"url ","bad",3); end(&b,dtbl);
    if (bad==5) box(&b,"j2cx",NULL,0);
    if (bad==6) { dtbl=begin(&b,"dtbl"); u16(&b,0); end(&b,dtbl); }
    if (bad==11) put(&b,"x",1);
    return b;
}

static void siz_checks(const char *path,const char *part,const uint8_t *cs,size_t n) {
    char error[512]; size_t at=0;
    SizFixed expected, fixed, saved;
    Component components[16384], before;
    bytes b={0}, companion={0}, altered={0};
    hv_local *source;
    check(hv_read_siz(cs,n,&expected,NULL,0,&at)==NULL,"SIZ fixed query");
    check(hv_read_siz(cs,n,&fixed,components,16384,&at)==NULL &&
          fixed.csiz==expected.csiz,"SIZ component fields");
    saved=fixed; before=components[0];
    check(hv_read_siz(cs,n,&fixed,components,0,&at)!=NULL &&
          memcmp(&fixed,&saved,sizeof fixed)==0 && memcmp(&before,&components[0],sizeof before)==0,
          "SIZ capacity failure leaves outputs unchanged");
    for (size_t cut=0;cut<4+(((size_t)cs[4]<<8)|cs[5]);cut++) {
        fixed=saved;
        check(hv_read_siz(cs,cut,&fixed,NULL,0,&at)!=NULL && memcmp(&fixed,&saved,sizeof fixed)==0,
              "every truncated SIZ prefix fails without publishing fields");
    }
    /* Rsiz capabilities and coding bodies belong to the host decoder. */
    put(&altered,cs,n); altered.data[6]=0xff; altered.data[7]=0xff;
    check(hv_read_siz(altered.data,n,&fixed,NULL,0,&at)==NULL && fixed.rsiz==65535,
          "SIZ query imposes no capability restriction");
    altered.data[19]=1; /* nonzero XOsiz */
    altered.data[43]=2; altered.data[44]=3; /* component sampling */
    check(hv_read_siz(altered.data,4+(((size_t)cs[4]<<8)|cs[5]),&fixed,components,16384,&at)==NULL &&
          fixed.xosiz==1 && components[0].xrsiz==2 && components[0].yrsiz==3,
          "SIZ-only query preserves origins and sampling without requiring COD");
    /* Csiz disagrees with the bounded component list. */
    altered.data[40]=(expected.csiz+1)>>8; altered.data[41]=expected.csiz+1;
    fixed=saved;
    check(hv_read_siz(altered.data,n,&fixed,NULL,0,&at)!=NULL && memcmp(&fixed,&saved,sizeof fixed)==0,
          "SIZ count failure leaves outputs unchanged");
    altered.size=0; put(&altered,cs,n);
    altered.data[43+3*(expected.csiz-1)]=0; /* last component's horizontal sampling */
    fixed=saved; memset(&components[0],0xA5,sizeof components[0]); before=components[0];
    check(hv_read_siz(altered.data,n,&fixed,components,16384,&at)!=NULL &&
          memcmp(&fixed,&saved,sizeof fixed)==0 && memcmp(&before,&components[0],sizeof before)==0,
          "invalid component leaves all outputs unchanged");
    for (size_t split=2;split<4+(((size_t)cs[4]<<8)|cs[5]);split++) {
        companion=(bytes){0}; put(&companion,"padding01234",12); put(&companion,cs+split,n-split); save(part,&companion);
        fragment_split=split; b=movie(cs,n,0); save(path,&b);
        source=hv_local_open(path,error,sizeof error);
        check(source!=NULL,"fragmented SIZ source open");
        if (source) {
            for (size_t i=0;i<2;i++) {
                check(hv_local_siz(source,i,&fixed,components,16384,error,sizeof error)==0 &&
                      fixed.xsiz==expected.xsiz && fixed.ysiz==expected.ysiz && fixed.csiz==expected.csiz,
                      "embedded and every fragmented SIZ boundary");
            }
            fixed=saved; fail_alloc=0;
            check(hv_local_siz(source,1,&fixed,NULL,0,error,sizeof error)==-1 &&
                  memcmp(&fixed,&saved,sizeof fixed)==0,"SIZ allocation failure leaves output unchanged");
            fail_alloc=-1;
            check(hv_local_copy(source,1,NULL,0,error,sizeof error)==n,
                  "SIZ query failure leaves byte access available");
            check(hv_local_siz(source,SIZE_MAX,&fixed,NULL,0,error,sizeof error)==-1,"SIZ invalid stream index");
            hv_local_close(source);
        }
        check(descriptors==0 && mappings==0,"SIZ source cleanup");
    }
    fragment_split=0;
}

/* Byte access must not depend on interpreted headers or packet validity. */
static void opaque_codestream_checks(const char *path,const char *part,const uint8_t *cs,size_t n) {
    size_t cod=2, at;
    hv_marker marker={0};
    while (hv_marker_read(cs,cod,n,&marker)==NULL && marker.code!=HV_COD) cod=marker.end;
    check(cod<n && marker.code==HV_COD,"opaque fixture COD located");
    if (cod>=n || marker.code!=HV_COD) return;
    for (int bad=0;bad<5;bad++) {
        bytes altered={0}, companion={0};
        put(&altered,cs,n);
        if (bad==0) { altered.data[6]=0xff; altered.data[7]=0xff; } /* unsupported Rsiz */
        if (bad==1) { altered.data[40]=0; altered.data[41]=0; } /* invalid Csiz */
        if (bad==2) altered.data[cod+5]=0xff; /* invalid progression order */
        if (bad==3) altered.data[n-1]=0; /* missing EOC */
        if (bad==4) altered.size=4+(((size_t)cs[4]<<8)|cs[5]); /* SIZ only */
        check(hv_codestream_check(altered.data,0,altered.size,HV_READ_VALIDATE,&at)!=NULL,
              "explicit validation rejects unusable codestream");
        for (int fragmented=0;fragmented<2;fragmented++) {
            bytes b={0}; char error[256]; uint8_t out[8192]; SizFixed fixed;
            hv_local *source;
            if (fragmented) {
                put(&companion,"padding01234",12);
                put(&companion,altered.data+altered.size/2,altered.size-altered.size/2);
                save(part,&companion);
                b=movie(altered.data,altered.size,0);
            } else {
                signature(&b,0); box(&b,"jp2c",altered.data,altered.size);
            }
            /* A bounded JP2H whose child framing cannot be interpreted. */
            box(&b,"jp2h","x",1); save(path,&b);
            if (!fragmented) {
                hv_header header;
                check(hv_read_jp2h(b.data,b.size,&header,&at)!=NULL,
                      "explicit header reading rejects malformed JP2H");
            }
            source=hv_local_open(path,error,sizeof error);
            check(source!=NULL,"opaque source opens despite unusable headers/codestream");
            if (source) {
                for (size_t i=0;i<hv_local_codestreams(source);i++) {
                    check((hv_local_siz(source,i,&fixed,NULL,0,error,sizeof error)==0)==(bad!=1),
                          "SIZ query interprets only bounded SIZ fields");
                    int before=allocations;
                    check(hv_local_copy(source,i,out,sizeof out,error,sizeof error)==altered.size &&
                          memcmp(out,altered.data,altered.size)==0,
                          "exact copy survives interpretation failure");
                    memset(out,0,sizeof out);
                    for (size_t offset=0;offset<altered.size;) {
                        size_t expected=altered.size-offset<7?altered.size-offset:7;
                        check(hv_local_read(source,i,offset,out+offset,7,error,sizeof error)==expected,
                              "bounded reads survive interpretation failure");
                        offset+=expected;
                    }
                    check(memcmp(out,altered.data,altered.size)==0 && before==allocations,
                          "opaque copy/read retain bytes without allocation");
                }
                hv_local_close(source);
            }
            check(mappings==0 && descriptors==0,"opaque source resource cleanup");
        }
    }
}

static void expect_bytes(const char *path,const uint8_t *cs,size_t n,size_t count) {
    char error[512]; uint8_t out[8192]; hv_local *source=hv_local_open(path,error,sizeof error);
    if (!source) { fprintf(stderr,"%s\n",error); check(0,"source open"); return; }
    check(descriptors==0,"mapped descriptors closed immediately");
    check(mappings==(count==1?1:2),"only used files mapped once per reference");
    check(hv_local_codestreams(source)==count,"codestream count");
    for (size_t i=0;i<count;i++) {
        check(hv_local_copy(source,i,NULL,0,error,sizeof error)==n,"size query");
        { SizFixed fixed;
          check(hv_local_siz(source,i,&fixed,NULL,0,error,sizeof error)==0,
                "SIZ query for real and large-offset sources"); }
        memset(out,0xA5,sizeof out);
        check(hv_local_copy(source,i,out,n-1,error,sizeof error)==0,"capacity error");
        for (size_t j=0;j<sizeof out;j++) if (out[j]!=0xA5) { check(0,"capacity failure writes nothing"); break; }
        check(hv_local_copy(source,i,out,sizeof out,error,sizeof error)==n && memcmp(out,cs,n)==0,"original codestream bytes");
        for (size_t offset=0;offset<=n;offset++) {
            const size_t capacities[]={0,1,2,13,sizeof out,SIZE_MAX};
            for (size_t k=0;k<sizeof capacities/sizeof capacities[0];k++) {
                size_t length=n-offset;
                if (length>capacities[k]) length=capacities[k];
                memset(out,0xA5,sizeof out);
                check(hv_local_read(source,i,offset,out,capacities[k],error,sizeof error)==length &&
                      memcmp(out,cs+offset,length)==0 && out[length]==0xA5,
                      "every offset, clipped read, EOF and output boundary");
            }
        }
        memset(out,0xA5,sizeof out);
        check(hv_local_read(source,i,n+1,out,sizeof out,error,sizeof error)==0 && out[0]==0xA5,
              "past-end read rejected before writing");
        check(hv_local_read(source,i,SIZE_MAX,out,sizeof out,error,sizeof error)==0 && out[0]==0xA5,
              "overflowing offset rejected before writing");
        check(hv_local_read(source,i,0,NULL,1,error,sizeof error)==0,"missing read buffer rejected");
        check(hv_local_read(source,i,n,NULL,0,error,sizeof error)==0,"zero read needs no buffer");
        check(hv_local_copy(source,i,out,sizeof out,error,sizeof error)==n,"whole-copy contract retained");
    }
    check(hv_local_copy(source,SIZE_MAX,NULL,0,error,sizeof error)==0,"invalid codestream index");
    check(hv_local_read(source,SIZE_MAX,0,out,sizeof out,error,sizeof error)==0,"read invalid codestream index");
    hv_local_close(source);
    check(memcmp(out,cs,n)==0,"caller bytes survive destruction");
    check(descriptors==0 && mappings==0,"source resources released");
}

static void association(bytes *b,unsigned kind,unsigned index,char value) {
    size_t at=begin(b,"asoc"); bytes names={0}; u32(&names,kind<<24|index);
    box(b,"nlst",names.data,names.size); box(b,"xml ",&value,1); end(b,at);
}
static void expect_xml(hv_local *source,size_t index,char expected) {
    const uint8_t *xml=NULL; size_t size=0; char error[256];
    check(hv_local_xml(source,index,&xml,&size,error,sizeof error)==0 &&
          (expected ? xml!=NULL && size==1 && *xml==(uint8_t)expected : xml==NULL && size==0),
          "local codestream XML association");
}
static void expect_layer_xml(const hv_metadata *metadata, const hv_presentation *presentation,
                             size_t layer, char expected) {
    const uint8_t *xml=NULL; size_t size=0; char error[256];
    check(hv_metadata_layer_xml(metadata,layer,&presentation->layer[layer].registration,
                               &xml,&size,error,sizeof error)==0 &&
          xml!=NULL && size==1 && *xml==(uint8_t)expected,"mapped layer XML");
}
static void layer_metadata_checks(void) {
    bytes b={0}; hv_metadata metadata={0}; hv_presentation presentation={0};
    char error[256]; const uint8_t *xml; size_t size, at=0;
    signature(&b,1);
    for (int i=0;i<4;i++) box(&b,"jp2c",NULL,0);
    box(&b,"xml ","F",1); association(&b,2,5,'E'); association(&b,2,0,'L');
    association(&b,1,2,'C'); association(&b,1,0,'A'); association(&b,1,1,'B');
    association(&b,2,3,'D'); association(&b,2,2,'Q'); association(&b,1,0,'Z');
    const uint8_t source[]={2,0,0,2,0,0,3};
    for (int i=0;i<7;i++) {
        size_t header=begin(&b,"jplh");
        uint8_t reg[]={0,1,0,1,0,source[i],1,1,0,0,0,2,1,1,0,0};
        box(&b,"creg",reg,i==2 ? sizeof reg : 10); end(&b,header);
    }
    check(hv_presentation_open(b.data,b.size,&presentation,&at)==NULL,"metadata layer registration inventory");
    for (int failure=0;failure<2;failure++) {
        fail_alloc=failure;
        check(hv_metadata_read(b.data,b.size,4,7,NULL,NULL,&metadata,error,sizeof error)==-1 &&
              metadata.frames==NULL && metadata.layers==NULL && metadata.codestream_count==0 &&
              metadata.layer_count==0,"metadata allocation failure cleanup");
    }
    fail_alloc=-1;
    check(hv_metadata_read(b.data,b.size,4,7,NULL,NULL,&metadata,error,sizeof error)==0 &&
          metadata.codestream_count==4 && metadata.layer_count==7,"independent entity counts");
    const char expected[]="LACCAEF";
    for (size_t i=0;i<7;i++) expect_layer_xml(&metadata,&presentation,i,expected[i]);
    check(hv_metadata_layer_xml(&metadata,5,NULL,&xml,&size,error,sizeof error)==0 &&
          size==1 && *xml=='E',"layer beyond codestream count indexed independently");
    check(hv_metadata_codestream_xml(&metadata,0,&xml,&size,error,sizeof error)==0 &&
          size==1 && *xml=='A',"codestream-only lookup excludes layer XML");
    check(hv_metadata_xml(&metadata,0,&xml,&size,error,sizeof error)==0 && size==1 && *xml=='L',
          "aligned profile keeps first associated document");
    check(hv_metadata_layer_xml(&metadata,6,NULL,&xml,&size,error,sizeof error)==0 && size==1 && *xml=='F',
          "layer-only fallback");
    check(hv_metadata_layer_xml(&metadata,7,NULL,&xml,&size,error,sizeof error)==-1 && xml==NULL && size==0,
          "layer index bounds");
    hv_registration invalid={NULL,1,4,1,1};
    check(hv_metadata_layer_xml(&metadata,5,&invalid,&xml,&size,error,sizeof error)==-1 && xml==NULL && size==0,
          "bad mapped codestream cannot be masked by layer XML");
    hv_metadata_close(&metadata); hv_metadata_close(&metadata); hv_presentation_free(&presentation);
    check(hv_metadata_read(NULL,0,1,SIZE_MAX,NULL,NULL,&metadata,error,sizeof error)==-1 &&
          metadata.frames==NULL && metadata.layers==NULL,"layer count overflow");
    /* Header metadata has an implicit entity scope, retained through nesting. */
    b=(bytes){0}; signature(&b,1); box(&b,"xml ","F",1);
    size_t header=begin(&b,"jpch");
    at=begin(&b,"grp "); box(&b,"xml ","S",1); end(&b,at); end(&b,header);
    header=begin(&b,"jpch"); association(&b,2,2,'N'); end(&b,header);
    header=begin(&b,"jplh"); box(&b,"xml ","T",1); end(&b,header);
    check(hv_metadata_read(b.data,b.size,2,3,NULL,NULL,&metadata,error,sizeof error)==0,
          "implicit header metadata index");
    check(hv_metadata_codestream_xml(&metadata,0,&xml,&size,error,sizeof error)==0 && *xml=='S',
          "JPCH implicit association through grouping");
    check(hv_metadata_codestream_xml(&metadata,1,&xml,&size,error,sizeof error)==0 && *xml=='N',
          "JPCH implicit scope retained through explicit association");
    check(hv_metadata_layer_xml(&metadata,0,NULL,&xml,&size,error,sizeof error)==0 && *xml=='T',
          "JPLH implicit association");
    check(hv_metadata_layer_xml(&metadata,2,NULL,&xml,&size,error,sizeof error)==0 && *xml=='N',
          "nested explicit layer association");
    hv_metadata_close(&metadata);
    /* An empty associated XML is present and must not become file fallback. */
    b=(bytes){0}; box(&b,"xml ","F",1); at=begin(&b,"asoc");
    uint8_t name[]={1,0,0,0}; box(&b,"nlst",name,sizeof name); box(&b,"xml ",NULL,0); end(&b,at);
    check(hv_metadata_read(b.data,b.size,1,1,NULL,NULL,&metadata,error,sizeof error)==0,
          "empty associated XML index");
    hv_registration identity={NULL,1,0,1,1};
    check(hv_metadata_layer_xml(&metadata,0,&identity,&xml,&size,error,sizeof error)==0 && xml!=NULL && size==0,
          "empty XML preserves presence");
    hv_metadata_close(&metadata);
    /* Late defaults fill absent palette fields without replacing overrides. */
    b=(bytes){0}; header=begin(&b,"jpch");
    uint8_t palette[]={0,1,1,7,99}, defaults[]={0,1,1,7,11}, cmap[]={0,0,1,0};
    box(&b,"pclr",palette,sizeof palette); end(&b,header); header=begin(&b,"jp2h");
    box(&b,"pclr",defaults,sizeof defaults); box(&b,"cmap",cmap,sizeof cmap); end(&b,header);
    check(hv_metadata_read(b.data,b.size,1,0,NULL,NULL,&metadata,error,sizeof error)==0,"late metadata palette defaults");
    int channels=0; uint8_t table=0;
    check(hv_metadata_palette(&metadata,0,&channels,&table,1,error,sizeof error)==1 && channels==1 && table==99,
          "late default palette cannot replace JPCH palette");
    hv_metadata_close(&metadata);
}

static void presentation_checks(const char *path, const uint8_t *cs, size_t n) {
    bytes b={0}, header={0}; char error[256]; hv_local *source;
    const uint8_t *data; const hv_presentation *presentation; hv_render render;
    size_t at=0;
    const uint8_t colour[]={1,0,0,0,0,0,17};
    const uint8_t palette[]={0,2,1,131,8,7}, mapping[]={0,0,1,0};
    signature(&b,1);
    box(&header,"colr",colour,sizeof colour); box(&header,"pclr",palette,sizeof palette);
    box(&header,"cmap",mapping,sizeof mapping); box(&b,"jp2h",header.data,header.size);
    box(&b,"jp2c",cs,n); box(&b,"jp2c",cs,n);
    for (int i=0;i<3;i++) {
        size_t start=begin(&b,"jplh");
        uint8_t registration[]={0,1,0,1,0,i==1?0:1,1,1,0,0};
        box(&b,"creg",registration,sizeof registration); end(&b,start);
    }
    save(path,&b); source=hv_local_open(path,error,sizeof error);
    check(source!=NULL,"presentation source open"); if (!source) return;
    check(source->presentation.layer==NULL,"presentation is lazy");
    for (int failure=0;failure<2;failure++) {
        data=b.data; presentation=&source->presentation; fail_alloc=failure;
        check(hv_local_presentation(source,&data,&presentation,error,sizeof error)==-1 &&
              data==NULL && presentation==NULL && source->presentation.layer==NULL &&
              source->presentation.codestream_headers==NULL,"presentation allocation failure cleanup");
        fail_alloc=-1;
        check(hv_local_copy(source,1,NULL,0,error,sizeof error)==n,
              "presentation failure preserves byte access");
    }
    check(hv_local_presentation(source,&data,&presentation,error,sizeof error)==0 &&
          presentation->codestreams==2 && presentation->layers==3 &&
          hv_local_codestreams(source)==2,"independent local layer count");
    const uint8_t *saved_data=data; const hv_presentation *saved_presentation=presentation;
    int before=allocations;
    check(hv_local_presentation(source,&data,&presentation,error,sizeof error)==0 &&
          data==saved_data && presentation==saved_presentation && before==allocations,
          "presentation view reused without allocation");
    /* The mapped view remains usable after pathname removal. */
    check(unlink(path)==0,"presentation pathname removal");
    for (size_t i=0;i<3;i++) {
        check(hv_render_read(data,presentation,i,1,&render,&at)==NULL &&
              render.codestream==(i==1?0:1) && render.channel_count==1 &&
              render.channel[0].component==0 && render.channel[0].palette_column==0,
              "borrowed presentation resolves reordered/reused layer channels");
        hv_palette_sample sample;
        check(hv_palette_read(&render.palette,0,0,&sample)==NULL && sample.value==-8 &&
              sample.bits==4 && sample.is_signed,"borrowed signed palette sample");
    }
    hv_local_close(source);
    check(mappings==0 && descriptors==0,"presentation source cleanup");

    /* An invalid presentation does not prevent original codestream access. */
    b=(bytes){0}; signature(&b,1); box(&b,"jp2c",cs,n);
    size_t start=begin(&b,"jplh");
    const uint8_t invalid[]={0,1,0,1,0,1,1,1,0,0};
    box(&b,"creg",invalid,sizeof invalid); end(&b,start); save(path,&b);
    source=hv_local_open(path,error,sizeof error);
    check(source!=NULL,"invalid presentation leaves source open");
    if (source) {
        for (int i=0;i<2;i++)
            check(hv_local_presentation(source,&data,&presentation,error,sizeof error)==-1 &&
                  data==NULL && presentation==NULL && strstr(error,"reference out of range")!=NULL,
                  "invalid presentation leaves no borrowed view");
        uint8_t out[8192];
        check(hv_local_copy(source,0,out,sizeof out,error,sizeof error)==n && memcmp(out,cs,n)==0,
              "invalid presentation preserves exact bytes");
        hv_local_close(source);
    }
    check(mappings==0 && descriptors==0,"invalid presentation cleanup");
}

static void metadata_checks(const char *path,const uint8_t *cs,size_t n) {
    bytes b={0}, header={0}, own={0}; size_t at; char error[256]; hv_local *source;
    const uint8_t pclr[]={0,2,1,7,10,20}, override[]={0,2,1,7,30,40}, cmap[]={0,0,1,0};
    const uint8_t *xml; size_t size; int channels; uint8_t table[2];
    signature(&b,1);
    box(&header,"pclr",pclr,sizeof pclr); box(&header,"cmap",cmap,sizeof cmap);
    box(&b,"jp2h",header.data,header.size);
    box(&b,"jpch",NULL,0); box(&own,"pclr",override,sizeof override); box(&b,"jpch",own.data,own.size);
    box(&b,"jp2c",cs,n); box(&b,"jp2c",cs,n);
    /* A layer numbered 0 is independent of codestream 0. */
    association(&b,2,0,'L');
    at=begin(&b,"grp "); association(&b,1,0,'A');
    size_t nested=begin(&b,"grp "); association(&b,1,1,'B'); end(&b,nested); end(&b,at);
    association(&b,1,0,'Z'); box(&b,"xml ","F",1);
    /* Unknown bodies, including JPIP placeholders in a plain file, are opaque. */
    box(&b,"phld","x",1); box(&b,"uuid","x",1); save(path,&b);
    source=hv_local_open(path,error,sizeof error); check(source!=NULL,"metadata source open");
    if (!source) return;
    fail_alloc=0;
    check(hv_local_xml(source,0,&xml,&size,error,sizeof error)==-1 && xml==NULL && size==0 &&
          source->metadata.frames==NULL,"metadata allocation failure leaves empty index");
    fail_alloc=-1;
    check(hv_local_copy(source,0,NULL,0,error,sizeof error)==n,"metadata failure preserves byte access");
    expect_xml(source,0,'A'); expect_xml(source,1,'B');
    { int before=allocations;
      expect_xml(source,0,'A'); check(before==allocations,"metadata index reused"); }
    check(hv_local_xml(source,2,&xml,&size,error,sizeof error)==-1 && xml==NULL && size==0,
          "metadata index bounds");
    table[0]=table[1]=0xA5;
    check(hv_local_palette(source,0,&channels,table,1,error,sizeof error)==2 && channels==1 &&
          table[0]==0xA5 && table[1]==0xA5,"palette capacity writes nothing");
    check(hv_local_palette(source,0,&channels,table,2,error,sizeof error)==2 && channels==1 &&
          table[0]==10 && table[1]==20,"default palette");
    check(hv_local_palette(source,1,&channels,table,2,error,sizeof error)==2 && channels==1 &&
          table[0]==30 && table[1]==40,"codestream palette override with inherited mapping");
    hv_local_close(source); check(mappings==0 && descriptors==0,"metadata source cleanup");

    b=(bytes){0}; signature(&b,1); box(&b,"jp2c",cs,n); association(&b,2,0,'L');
    at=begin(&b,"grp "); box(&b,"xml ","F",1); end(&b,at); save(path,&b);
    source=hv_local_open(path,error,sizeof error); check(source!=NULL,"fallback source open");
    if (source) { expect_xml(source,0,'F'); hv_local_close(source); }
    b=(bytes){0}; signature(&b,1); box(&b,"jp2c",cs,n); association(&b,2,0,'L'); save(path,&b);
    source=hv_local_open(path,error,sizeof error);
    if (source) { expect_xml(source,0,0); hv_local_close(source); } else check(0,"layer-only source open");

    /* Palette body validation happens when queried, leaving XML accessible. */
    b=(bytes){0}; header=(bytes){0}; signature(&b,0);
    box(&header,"pclr","bad",3); box(&header,"cmap",cmap,sizeof cmap);
    box(&b,"jp2h",header.data,header.size); box(&b,"jp2c",cs,n); box(&b,"xml ","F",1); save(path,&b);
    source=hv_local_open(path,error,sizeof error);
    if (source) {
        expect_xml(source,0,'F');
        check(hv_local_palette(source,0,&channels,table,2,error,sizeof error)==-1,"malformed palette rejected at query");
        expect_xml(source,0,'F'); hv_local_close(source);
    } else check(0,"opaque palette source open");
    { bytes content={0}; association(&content,1,0,'X');
      for (int i=1;i<HV_BOX_DEPTH_MAX;i++) { bytes outer={0}; box(&outer,"grp ",content.data,content.size); content=outer; }
      b=(bytes){0}; signature(&b,1); box(&b,"jp2c",cs,n); put(&b,content.data,content.size); save(path,&b);
      source=hv_local_open(path,error,sizeof error);
      if (source) { expect_xml(source,0,'X'); hv_local_close(source); } else check(0,"metadata maximum depth source open"); }

    for (int bad=0;bad<5;bad++) {
        const char *reasons[]={"nlst.length","asoc.children","grp.asoc-first","metadata:","box.depth-limit"};
        bytes content={0}; b=(bytes){0}; signature(&b,1); box(&b,"jp2c",cs,n);
        if (bad==0) { box(&content,"nlst","abc",3); box(&content,"xml ","X",1); box(&b,"asoc",content.data,content.size); }
        if (bad==1) { box(&content,"nlst","abcd",4); box(&b,"asoc",content.data,content.size); }
        if (bad==2) { box(&content,"grp ",NULL,0); box(&content,"xml ","X",1); box(&b,"asoc",content.data,content.size); }
        if (bad==3) { const uint8_t cut[]={0,0,0,9,'x','m','l',' '}; box(&b,"grp ",cut,sizeof cut); }
        if (bad==4) {
            association(&content,1,0,'X');
            for (int i=0;i<HV_BOX_DEPTH_MAX;i++) { bytes outer={0}; box(&outer,"grp ",content.data,content.size); content=outer; }
            put(&b,content.data,content.size);
        }
        save(path,&b); source=hv_local_open(path,error,sizeof error);
        check(source!=NULL,"malformed metadata leaves opaque source open");
        if (source) {
            check(hv_local_xml(source,0,&xml,&size,error,sizeof error)==-1 &&
                  source->metadata.frames==NULL && strstr(error,reasons[bad])!=NULL,
                  "malformed metadata index cleaned up with expected error");
            check(hv_local_copy(source,0,NULL,0,error,sizeof error)==n,"malformed metadata leaves copying available");
            hv_local_close(source);
        }
        check(mappings==0 && descriptors==0,"malformed metadata resources released");
    }
    { hv_metadata metadata={0};
      check(hv_metadata_read(NULL,0,SIZE_MAX,0,NULL,NULL,&metadata,error,sizeof error)==-1 &&
            metadata.frames==NULL,"metadata count overflow"); }
}

int main(int argc,char **argv) {
    bytes base={0}, companion={0}, b;
    char temp[]="/tmp/test_local.XXXXXX",path[256],part[256],big[256],fifo[256],input[4096],error[512];
    hv_box stream; size_t at,n; FILE *f; hv_local *source; int calls,fd;
    if (argc!=2 || !mkdtemp(temp)) return 2;
    snprintf(input,sizeof input,"%s/jp2.jp2",argv[1]); f=fopen(input,"rb"); if (!f) return 2;
    base.size=fread(base.data,1,sizeof base.data,f); fclose(f);
    if (hv_read_jp2(base.data,base.size,&stream,&at)!=NULL) return 2;
    n=stream.end-stream.payload;
    snprintf(path,sizeof path,"%s/movie.data",temp); snprintf(part,sizeof part,"%s/part data.bin",temp);
    snprintf(big,sizeof big,"%s/large.data",temp); snprintf(fifo,sizeof fifo,"%s/fifo",temp);
    save(path,&base); expect_bytes(path,base.data+stream.payload,n,1);
    put(&companion,"abcdefghijkl",12); put(&companion,base.data+stream.payload+n/2,n-n/2); save(part,&companion);
    b=movie(base.data+stream.payload,n,0); save(path,&b); expect_bytes(path,base.data+stream.payload,n,2);
    {
        char absolute_url[512];
        snprintf(absolute_url,sizeof absolute_url,"file://%s/part%%20data.bin",temp);
        const char *locations[]={absolute_url,part,"file://part%20data.bin"};
        for (size_t i=0;i<3;i++) {
            test_reference=locations[i]; b=movie(base.data+stream.payload,n,0); save(path,&b);
            expect_bytes(path,base.data+stream.payload,n,2);
        }
        test_reference=NULL; b=movie(base.data+stream.payload,n,0); save(path,&b);
    }
    /* Original view is retained after pathname removal, with no live descriptors. */
    source=hv_local_open(path,error,sizeof error); check(source!=NULL,"lifetime source open");
    if (source) {
        uint8_t out[8192]; unlink(part);
        check(hv_local_copy(source,1,out,sizeof out,error,sizeof error)==n && memcmp(out,base.data+stream.payload,n)==0,"mapped companion lifetime");
        hv_local_close(source); save(part,&companion);
    }
    for (int bad=1;bad<=11;bad++) {
        b=movie(base.data+stream.payload,n,bad); save(path,&b);
        source=hv_local_open(path,error,sizeof error); check(source==NULL,"malformed/unsupported source rejected"); hv_local_close(source);
        check(descriptors==0 && mappings==0,"failed source cleanup");
    }
    b=movie(base.data+stream.payload,n,0); save(path,&b);
    allocations=0; source=hv_local_open(path,error,sizeof error); calls=allocations; check(source!=NULL,"allocation baseline"); hv_local_close(source);
    for (int i=0;i<calls;i++) {
        fail_alloc=i; source=hv_local_open(path,error,sizeof error); fail_alloc=-1;
        check(source==NULL,"allocation failure injected"); hv_local_close(source);
        check(descriptors==0 && mappings==0,"allocation failure cleanup");
    }
    for (int kind=1;kind<=3;kind++) for (int index=0;index<2;index++) {
        fail_io=kind; io_index=index; io_calls=0; source=hv_local_open(path,error,sizeof error); fail_io=0;
        check(source==NULL,"main/companion I/O failure injected"); hv_local_close(source);
        check(descriptors==0 && mappings==0,"I/O failure cleanup");
    }
    for (int i=0;i<20;i++) expect_bytes(path,base.data+stream.payload,n,2);
    mkfifo(fifo,0600); source=hv_local_open(fifo,error,sizeof error); check(source==NULL,"nonregular file rejected without blocking"); hv_local_close(source);
    /* Sparse >2GiB source: original bytes copied from a late JP2C. */
    {
        size_t start=(size_t)INT_MAX+128, size=start+8+n;
        bytes head={0}, tail={0}; signature(&head,0); u32(&head,1); put(&head,"free",4); u64(&head,start-32);
        box(&tail,"jp2c",base.data+stream.payload,n);
        fd=open(big,O_RDWR|O_CREAT|O_TRUNC,0600); check(fd>=0,"large fixture open");
        if (fd>=0) {
            check(ftruncate(fd,(off_t)size)==0 && pwrite(fd,head.data,head.size,0)==(ssize_t)head.size &&
                  pwrite(fd,tail.data,tail.size,(off_t)start)==(ssize_t)tail.size,"large sparse fixture");
            close(fd); expect_bytes(big,base.data+stream.payload,n,1);
        }
    }
    /* A referenced fragment itself starts above 2GiB, without loading the
     * unused gap into RAM or retaining an open file descriptor. */
    {
        size_t start=(size_t)INT_MAX+128, tail=n-n/2;
        fd=open(part,O_RDWR|O_TRUNC); check(fd>=0,"large companion open");
        if (fd>=0) {
            check(ftruncate(fd,(off_t)(start+tail))==0 &&
                  pwrite(fd,base.data+stream.payload+n/2,tail,(off_t)start)==(ssize_t)tail,
                  "large companion sparse extent");
            close(fd); external_offset=start;
            b=movie(base.data+stream.payload,n,0); save(path,&b);
            expect_bytes(path,base.data+stream.payload,n,2);
            external_offset=12; save(part,&companion);
        }
    }
    /* Real embedded and linked JPX corpus files, with all stream extents
     * compared against the existing serving reader. */
    {
        const char *names[]={"jpx-embedded.jpx","jpx-linked.jpx"};
        for (size_t k=0;k<2;k++) {
            bytes file={0}; hv_served_sources index;
            snprintf(input,sizeof input,"%s/%s",argv[1],names[k]);
            f=fopen(input,"rb"); if (!f) return 2;
            file.size=fread(file.data,1,sizeof file.data,f); fclose(f);
            if (hv_served_jpx(file.data,file.size,&index,&at)!=NULL) return 2;
            source=hv_local_open(input,error,sizeof error);
            check(source!=NULL && hv_local_codestreams(source)==index.count,"real JPX streams");
            if (source) for (size_t i=0;i<index.count;i++) {
                uint8_t out[8192]; bytes linked={0}; const uint8_t *expected; size_t length;
                if (index.jp2c) {
                    expected=file.data+index.jp2c[i].payload;
                    length=index.jp2c[i].end-index.jp2c[i].payload;
                } else {
                    char target[4096];
                    if (hv_served_path(&index.links[i],input,target,sizeof target)!=NULL) return 2;
                    f=fopen(target,"rb"); if (!f) return 2;
                    linked.size=fread(linked.data,1,sizeof linked.data,f); fclose(f);
                    expected=linked.data+index.links[i].offset; length=index.links[i].length;
                }
                check(length<=sizeof out && hv_local_copy(source,i,out,sizeof out,error,sizeof error)==length &&
                      memcmp(out,expected,length)==0,"real JPX original codestream bytes");
            }
            hv_local_close(source); hv_served_sources_free(&index);
            check(descriptors==0 && mappings==0,"real JPX resources released");
        }
    }
    opaque_codestream_checks(path,part,base.data+stream.payload,n);
    siz_checks(path,part,base.data+stream.payload,n);
    layer_metadata_checks();
    presentation_checks(path,base.data+stream.payload,n);
    metadata_checks(path,base.data+stream.payload,n);
    hv_local_close(NULL);
    check(descriptors==0 && mappings==0,"final resource accounting");
    unlink(path); unlink(part); unlink(big); unlink(fifo); rmdir(temp);
    if (!failures) puts("all local-source checks passed");
    return failures!=0;
}
