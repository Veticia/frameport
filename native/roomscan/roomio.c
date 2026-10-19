/* SPDX-License-Identifier: GPL-3.0-only */
/* Room files: the room.txt writer/reader (docs/mr/ROOM_FORMAT.md), mesh sidecars, atomic publishing and the JSON
 * pieces of scan.json. Every number goes through put_dec, so a room read back from its own text is bit-identical
 * to the one the scan path refines (that is what makes --refine-only reproduce a scan's refined output). */
#include "refine.h"

void buf_init(Buf *b, usize limit) { *b=(Buf){0};b->limit=limit;b->ok=1; }
void buf_free(Buf *b) { io_free(b->data,b->cap);*b=(Buf){0}; }
void put_n(Buf *b, const char *s, usize n)
{
    if(!b->ok) return;
    if(b->n+n+1>b->cap) {
        usize cap=b->cap ? b->cap : 256;
        while(cap<b->n+n+1) cap*=2;
        if(cap>b->limit) cap=b->limit;
        if(cap<b->n+n+1) { b->ok=0;return; }
        char *data=io_alloc(cap);
        if(!data) { b->ok=0;return; }
        if(b->n) memcpy(data,b->data,b->n);
        io_free(b->data,b->cap);b->data=data;b->cap=cap;
    }
    memcpy(b->data+b->n,s,n);b->n+=n;b->data[b->n]=0;
}
void put(Buf *b, const char *s) { usize n=0;while(s[n]) n++;put_n(b,s,n); }
void put_u64(Buf *b, u64 value)
{
    char rev[24];int n=0;
    do { rev[n++]=(char)('0'+value%10);value/=10; } while(value);
    char out[24];for(int i=0;i<n;i++) out[i]=rev[n-1-i];
    put_n(b,out,(usize)n);
}
void put_int(Buf *b, i64 v) { if(v<0) { put(b,"-");v=-v; } put_u64(b,(u64)v); }
void put_dec(Buf *b, float value)
{
    /* Same arithmetic as the scanner's original writer: the --refine 0 output stays byte-for-byte unchanged. */
    if(value<0) { put(b,"-");value=-value; }
    u64 scaled=(u64)(value*10000.f+.5f);
    put_u64(b,scaled/10000);put(b,".");
    u64 fraction=scaled%10000;
    if(fraction<1000) put(b,"0");
    if(fraction<100) put(b,"0");
    if(fraction<10) put(b,"0");
    put_u64(b,fraction);
}
void put_fixed(Buf *b, double v, int places)
{
    /* Exact, like Number.prototype.toFixed: v*10^places from the double's bits in 128-bit integers, ties up. */
    double scale=1;for(int i=0;i<places;i++) scale*=10;
    int negative=v<0;if(negative) v=-v;
    if(!(v<1e9)) v=0;
    union { double d; u64 u; } bits={v};
    int e=(int)((bits.u>>52)&0x7ff);
    u64 m=bits.u&((1ull<<52)-1);
    if(e) m|=1ull<<52;else e=1;
    e-=1075;  /* v = m * 2^e */
    unsigned __int128 big=(unsigned __int128)m*(u64)scale;
    u64 scaled;
    if(e>=0) scaled=(u64)(big<<e);
    else if(e<=-127) scaled=0;
    else {
        unsigned __int128 half=(unsigned __int128)1<<(-e-1),rest=big&(((unsigned __int128)1<<-e)-1);
        scaled=(u64)(big>>-e)+(rest>=half);
    }
    if(negative && scaled) put(b,"-");
    put_u64(b,(u64)(scaled/(u64)scale));
    if(places) {
        put(b,".");u64 fraction=scaled%(u64)scale;
        for(u64 d=(u64)scale/10;d>1 && fraction<d;d/=10) put(b,"0");
        put_u64(b,fraction);
    }
}
void put_round(Buf *b, double v) { put_int(b,(i64)dfloor(v+.5)); }

static int uuid_hex(char out[33])
{
    u8 bytes[16];if(!io_random(bytes,sizeof(bytes))) return 0;
    bytes[6]=(bytes[6]&15)|64;bytes[8]=(bytes[8]&63)|128;
    const char *hex="0123456789abcdef";
    for(int i=0;i<16;i++) { out[2*i]=hex[bytes[i]>>4];out[2*i+1]=hex[bytes[i]&15]; }
    out[32]=0;return 1;
}
static void copy(char *dst, const char *src, usize cap)
{
    usize i=0;while(src[i] && i+1<cap) { dst[i]=src[i];i++; }
    dst[i]=0;
}
const char *room_from_scan(Scan *s, Room *r)
{
    memset(r,0,sizeof(*r));
    r->universe=s->opt.universe;r->time=(u64)(io_time(0)/1000000000);copy(r->source,"scan",sizeof(r->source));
    if(!uuid_hex(r->room_id) || !uuid_hex(r->floor_id) || !uuid_hex(r->ceiling_id)) return "getrandom failed";
    r->floor=s->floor;r->ceiling=s->ceiling;r->noutline=s->noutline;
    for(int i=0;i<s->noutline;i++) {
        Pt a=s->outline[i],b=s->outline[(i+1)%s->noutline];
        r->outline[i]=(DPt){a.x,a.z};
        r->walls[i]=(RWall){a.x,a.z,b.x,b.z,s->floor,s->ceiling,s->invisible[i],{0}};
        if(!uuid_hex(r->walls[i].id)) return "getrandom failed";
    }
    r->nwalls=s->noutline;
    for(int i=0;i<s->nvolumes;i++) {
        Volume *v=&s->volumes[i];RVol *o=&r->vols[i];
        *o=(RVol){v->p.x,v->p.z,v->p.y,v->yaw,v->w,v->d,v->h,{0},{0}};
        copy(o->label,v->label,sizeof(o->label));
        if(!uuid_hex(o->id)) return "getrandom failed";
    }
    r->nvols=s->nvolumes;
    if(!uuid_hex(r->mesh_id)) return "getrandom failed";
    copy(r->mesh_name,"room-mesh.bin",sizeof(r->mesh_name));
    r->has_mesh=1;r->mesh_nv=(u32)s->nv;r->mesh_ni=(u32)s->ni;
    return 0;
}
static void sp(Buf *b, double v) { put(b," ");put_dec(b,(float)v); }
int room_text(const Room *r, Buf *b)
{
    put(b,"frameport-room 1\nuniverse ");put_u64(b,r->universe);
    put(b,"\nsource ");put(b,r->source);put(b,"\ntime ");put_u64(b,r->time);
    put(b,"\nroom ");put(b,r->room_id);
    put(b,"\nfloor ");put(b,r->floor_id);sp(b,r->floor);
    put(b,"\nceiling ");put(b,r->ceiling_id);sp(b,r->ceiling);
    put(b,"\noutline ");put_u64(b,(u64)r->noutline);
    for(int i=0;i<r->noutline;i++) { sp(b,r->outline[i].x);sp(b,r->outline[i].z); }
    put(b,"\n");
    for(int i=0;i<r->nwalls;i++) {
        const RWall *w=&r->walls[i];
        put(b,"wall ");put(b,w->id);sp(b,w->x0);sp(b,w->z0);sp(b,w->x1);sp(b,w->z1);sp(b,w->y0);sp(b,w->y1);
        if(w->invisible) put(b," INVISIBLE_WALL_FACE");
        put(b,"\n");
    }
    for(int i=0;i<r->nvols;i++) {
        const RVol *v=&r->vols[i];
        put(b,"volume ");put(b,v->id);put(b," ");put(b,v->label);
        sp(b,v->cx);sp(b,v->cz);sp(b,v->top);sp(b,v->yaw);sp(b,v->w);sp(b,v->d);sp(b,v->h);put(b,"\n");
    }
    put_n(b,r->extra,r->nextra);
    if(r->has_mesh) {
        put(b,"mesh ");put(b,r->mesh_id);put(b," ");put(b,r->mesh_name);put(b," ");
        put_u64(b,r->mesh_nv);put(b," ");put_u64(b,r->mesh_ni);put(b,"\n");
    }
    return b->ok;
}

/* ------------------------------------------------------------------------------------------------ reading */
typedef struct { char *p[64]; int n; } Fields;
static int is_space(char c) { return c==' ' || c=='\t' || c=='\r'; }
/* Semantic labels are Meta's upper-case names; anything else would also be unsafe in scan.json strings. */
static int field_label(const char *p)
{
    int n=0;
    for(;p[n];n++) if(!((p[n]>='A' && p[n]<='Z') || (p[n]>='0' && p[n]<='9') || p[n]=='_')) return 0;
    return n>0 && n<MAX_LABEL;
}
static int field_uuid(const char *p)
{
    int n=0;
    for(;p[n];n++) if(!((p[n]>='0' && p[n]<='9') || (p[n]>='a' && p[n]<='f'))) return 0;
    return n==32;
}
/* Decimal text to double, correctly rounded for the room format's numbers (<= 17 significant digits): the
 * mantissa and the power of ten are exact doubles, so one division/multiplication rounds once, as parseFloat. */
static int field_double(const char *p, double *out)
{
    int negative=0,digits=0,exponent=0;u64 mantissa=0;
    if(*p=='-' || *p=='+') negative=*p++=='-';
    for(;*p>='0' && *p<='9';p++,digits++) { if(mantissa<100000000000000000ull) mantissa=mantissa*10+(u64)(*p-'0');else exponent++; }
    if(*p=='.') for(p++;*p>='0' && *p<='9';p++,digits++)
        if(mantissa<100000000000000000ull) { mantissa=mantissa*10+(u64)(*p-'0');exponent--; }
    if(!digits) return 0;
    if(*p=='e' || *p=='E') {
        p++;int sign=1,e=0,ed=0;
        if(*p=='-' || *p=='+') sign=*p++=='-' ? -1 : 1;
        for(;*p>='0' && *p<='9';p++,ed++) { e=e*10+*p-'0';if(e>400) return 0; }
        if(!ed) return 0;
        exponent+=sign*e;
    }
    if(*p) return 0;
    double v=(double)mantissa,scale=1;
    int e=exponent<0 ? -exponent : exponent;
    for(int i=0;i<e && scale<1e300;i++) scale*=10;
    v=exponent<0 ? v/scale : v*scale;
    if(!(v<=1e7)) return 0;  /* also rejects NaN; room coordinates are metres */
    *out=negative ? -v : v;return 1;
}
static int field_u64(const char *p, u64 *out)
{
    u64 n=0;if(!*p) return 0;
    for(;*p;p++) {
        if(*p<'0' || *p>'9') return 0;
        unsigned digit=(unsigned)(*p-'0');
        if(n>(~(u64)0-digit)/10) return 0;
        n=n*10+digit;
    }
    *out=n;return 1;
}
static int floats(Fields *f, int from, double *out, int n)
{
    if(f->n<from+n) return 0;
    for(int i=0;i<n;i++) if(!field_double(f->p[from+i],&out[i])) return 0;
    return 1;
}
const char *room_parse(const char *text, usize n, Room *r)
{
    static char line[16384];
    memset(r,0,sizeof(*r));
    int have_floor=0,have_ceiling=0,first=1;
    usize at=0;
    while(at<n) {
        usize start=at,len=0;
        while(at<n && text[at]!='\n') at++;
        len=at-start;at++;
        if(len>=sizeof(line)) return "room line too long";
        memcpy(line,text+start,len);line[len]=0;
        usize raw_len=len;
        for(usize i=0;i<len;i++) if(line[i]=='#') { line[i]=0;len=i;break; }
        Fields f={{0},0};
        char *p=line;
        while(*p) {
            while(is_space(*p)) *p++=0;
            if(!*p) break;
            if(f.n==64) return "room line has too many fields";
            f.p[f.n++]=p;
            while(*p && !is_space(*p)) p++;
        }
        if(first) {
            if(f.n!=2 || !same(f.p[0],"frameport-room") || !same(f.p[1],"1")) return "not a frameport-room 1 file";
            first=0;continue;
        }
        if(!f.n) continue;
        const char *k=f.p[0];
        if(same(k,"universe")) { if(f.n<2 || !field_u64(f.p[1],&r->universe)) return "invalid universe record"; }
        else if(same(k,"time")) { if(f.n<2 || !field_u64(f.p[1],&r->time)) return "invalid time record"; }
        else if(same(k,"source")) { if(f.n<2) return "invalid source record";copy(r->source,f.p[1],sizeof(r->source)); }
        else if(same(k,"room")) { if(f.n<2 || !field_uuid(f.p[1])) return "invalid room record";copy(r->room_id,f.p[1],33); }
        else if(same(k,"floor") || same(k,"ceiling")) {
            int c=same(k,"ceiling");
            if(f.n<3 || !field_uuid(f.p[1]) || !field_double(f.p[2],c ? &r->ceiling : &r->floor)) return "invalid floor/ceiling record";
            copy(c ? r->ceiling_id : r->floor_id,f.p[1],33);
            if(c) have_ceiling=1;else have_floor=1;
        } else if(same(k,"outline")) {
            u64 count;
            if(f.n<2 || !field_u64(f.p[1],&count) || count<3 || count>MAX_OUTLINE || (u64)f.n!=2+2*count) return "invalid outline record";
            for(int i=0;i<(int)count;i++)
                if(!field_double(f.p[2+2*i],&r->outline[i].x) || !field_double(f.p[3+2*i],&r->outline[i].z)) return "invalid outline record";
            r->noutline=(int)count;
        } else if(same(k,"wall")) {
            double v[6];
            if(r->nwalls==MAX_OUTLINE) return "too many walls";
            if(f.n<8 || !field_uuid(f.p[1]) || !floats(&f,2,v,6)) return "invalid wall record";
            RWall *w=&r->walls[r->nwalls++];
            *w=(RWall){v[0],v[1],v[2],v[3],v[4],v[5],f.n>8 && same(f.p[8],"INVISIBLE_WALL_FACE"),{0}};
            copy(w->id,f.p[1],33);
        } else if(same(k,"volume")) {
            double v[7];
            if(r->nvols==MAX_VOLUMES) return "too many volumes";
            if(f.n<10 || !field_uuid(f.p[1]) || !field_label(f.p[2]) || !floats(&f,3,v,7)) return "invalid volume record";
            RVol *o=&r->vols[r->nvols++];
            *o=(RVol){v[0],v[1],v[2],v[3],v[4],v[5],v[6],{0},{0}};
            copy(o->label,f.p[2],sizeof(o->label));copy(o->id,f.p[1],33);
        } else if(same(k,"mesh")) {
            u64 nv,ni;
            if(f.n<5 || !field_uuid(f.p[1]) || !field_u64(f.p[3],&nv) || !field_u64(f.p[4],&ni) ||
               nv>MAX_MESH_VERTICES || ni>(u64)MAX_TRIANGLES*3) return "invalid mesh record";
            for(const char *c=f.p[2];*c;c++) if(*c=='/' || *c=='\\') return "invalid mesh file name";
            copy(r->mesh_id,f.p[1],33);copy(r->mesh_name,f.p[2],sizeof(r->mesh_name));
            r->mesh_nv=(u32)nv;r->mesh_ni=(u32)ni;r->has_mesh=1;
        } else {
            /* Forward compatibility: unknown records (plane, ...) are kept as they were written. */
            if(r->nextra+raw_len+1>=MAX_EXTRA_TEXT) return "too many extra room records";
            memcpy(r->extra+r->nextra,text+start,raw_len);r->nextra+=raw_len;r->extra[r->nextra++]='\n';
        }
    }
    if(first) return "empty room file";
    if(!r->source[0]) copy(r->source,"scan",sizeof(r->source));
    if(!r->room_id[0] && !uuid_hex(r->room_id)) return "getrandom failed";
    if(!have_floor && !uuid_hex(r->floor_id)) return "getrandom failed";
    if(!have_ceiling) { if(!uuid_hex(r->ceiling_id)) return "getrandom failed";r->ceiling=r->floor+2.5; }
    if(!r->noutline && r->nwalls>=3) {
        double x0=1e10,x1=-1e10,z0=1e10,z1=-1e10;
        for(int i=0;i<r->nwalls;i++) {
            RWall *w=&r->walls[i];
            double lx=w->x0<w->x1 ? w->x0 : w->x1,hx=w->x0<w->x1 ? w->x1 : w->x0;
            double lz=w->z0<w->z1 ? w->z0 : w->z1,hz=w->z0<w->z1 ? w->z1 : w->z0;
            x0=lx<x0 ? lx : x0;x1=hx>x1 ? hx : x1;z0=lz<z0 ? lz : z0;z1=hz>z1 ? hz : z1;
        }
        r->outline[0]=(DPt){x0,z0};r->outline[1]=(DPt){x1,z0};r->outline[2]=(DPt){x1,z1};r->outline[3]=(DPt){x0,z1};
        r->noutline=4;
    }
    if(r->noutline<3) return "room has no outline";
    double twice=0;
    for(int i=0;i<r->noutline;i++) {
        DPt a=r->outline[i],b=r->outline[(i+1)%r->noutline];twice+=a.x*b.z-b.x*a.z;
    }
    if(twice<0) for(int i=0;i<r->noutline/2;i++) {  /* readers reverse a negative outline (ROOM_FORMAT.md) */
        DPt t=r->outline[i];r->outline[i]=r->outline[r->noutline-1-i];r->outline[r->noutline-1-i]=t;
    }
    if(!r->nwalls) {
        for(int i=0;i<r->noutline;i++) {
            DPt a=r->outline[i],b=r->outline[(i+1)%r->noutline];
            r->walls[i]=(RWall){a.x,a.z,b.x,b.z,r->floor,r->ceiling,0,{0}};
            if(!uuid_hex(r->walls[i].id)) return "getrandom failed";
        }
        r->nwalls=r->noutline;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ files */
int join_path(char *out, const char *dir, const char *name)
{
    usize n=0;
    while(*dir) { if(n>=3900) return 0;out[n++]=*dir++; }
    if(n && out[n-1]!='/') out[n++]='/';
    while(*name) { if(n>=4095) return 0;out[n++]=*name++; }
    out[n]=0;return 1;
}
const char *read_file(const char *path, usize limit, u8 **data, usize *size)
{
    *data=0;*size=0;
    int fd=io_open(path,0);
    if(fd<0) return "cannot open file";
    usize cap=limit+1;
    u8 *buf=io_alloc(cap);
    if(!buf) { io_close(fd);return "file allocation failed"; }
    usize n=0;
    for(;;) {
        long r=io_read(fd,buf+n,cap-n);
        if(r<0) { io_close(fd);io_free(buf,cap);return "cannot read file"; }
        if(!r) break;
        n+=(usize)r;
        if(n>limit) { io_close(fd);io_free(buf,cap);return "file too large"; }
    }
    io_close(fd);
    /* The caller frees with io_free(data, limit+1); the size is the buffer, not the content. */
    *data=buf;*size=n;return 0;
}
const char *publish(const char *dir, const char *name, const void *data, usize size)
{
    char id[33],tmpname[256],final[4096],tmp[4096];
    if(!uuid_hex(id)) return "getrandom failed";
    usize n=0;
    for(const char *p=name;*p && n<180;p++) tmpname[n++]=*p;
    tmpname[n++]='.';
    for(int i=0;i<32;i++) tmpname[n++]=id[i];
    for(const char *p=".tmp";*p;p++) tmpname[n++]=*p;
    tmpname[n]=0;
    if(!join_path(final,dir,name) || !join_path(tmp,dir,tmpname)) return "output path too long";
    int fd=io_open(tmp,1);
    if(fd<0) return "cannot open temporary file";
    int ok=io_write(fd,data,size);
    if(io_close(fd)) ok=0;
    if(!ok) { io_remove(tmp);return "cannot write temporary file"; }
    if(io_rename(tmp,final)) { io_remove(tmp);return "cannot publish file"; }
    return 0;
}
static u32 rd32(const u8 *p) { return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }
static void wr32(u8 *p, u32 v) { for(int i=0;i<4;i++) p[i]=(u8)(v>>(8*i)); }
const char *mesh_parse(const u8 *data, usize size, Vec **vertices, u32 **indices, int *nv, int *ni)
{
    *vertices=0;*indices=0;*nv=*ni=0;
    if(size<16 || data[0]!='F' || data[1]!='P' || data[2]!='R' || data[3]!='M' || rd32(data+4)!=1) return "invalid mesh header";
    u32 v=rd32(data+8),i=rd32(data+12);
    if(v>MAX_MESH_VERTICES || i>(u32)MAX_TRIANGLES*3 || i%3 || size!=16+(usize)v*12+(usize)i*4) return "invalid mesh size";
    Vec *pv=io_alloc((usize)MAX_MESH_VERTICES*sizeof(Vec));
    u32 *pi=io_alloc((usize)MAX_TRIANGLES*3*sizeof(u32));
    if(!pv || !pi) { io_free(pv,(usize)MAX_MESH_VERTICES*sizeof(Vec));io_free(pi,(usize)MAX_TRIANGLES*3*sizeof(u32));return "mesh allocation failed"; }
    for(u32 k=0;k<v*3;k++) {
        union { u32 u; float f; } c={rd32(data+16+4*(usize)k)};
        if(!finite(c.f)) { io_free(pv,(usize)MAX_MESH_VERTICES*sizeof(Vec));io_free(pi,(usize)MAX_TRIANGLES*3*sizeof(u32));return "non-finite mesh vertex"; }
        ((float *)pv)[k]=c.f;
    }
    for(u32 k=0;k<i;k++) {
        pi[k]=rd32(data+16+(usize)v*12+4*(usize)k);
        if(pi[k]>=v) { io_free(pv,(usize)MAX_MESH_VERTICES*sizeof(Vec));io_free(pi,(usize)MAX_TRIANGLES*3*sizeof(u32));return "mesh index outside vertex array"; }
    }
    *vertices=pv;*indices=pi;*nv=(int)v;*ni=(int)i;return 0;
}
const char *mesh_bytes(const Vec *vertices, const u32 *indices, int nv, int ni, u8 **data, usize *size)
{
    *size=16+(usize)nv*12+(usize)ni*4;
    *data=io_alloc(*size);
    if(!*data) return "mesh allocation failed";
    u8 *p=*data;
    p[0]='F';p[1]='P';p[2]='R';p[3]='M';wr32(p+4,1);wr32(p+8,(u32)nv);wr32(p+12,(u32)ni);
    /* Both supported targets are little-endian IEEE float32, like the scanner's original writer. */
    memcpy(p+16,vertices,(usize)nv*12);
    memcpy(p+16+(usize)nv*12,indices,(usize)ni*4);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ scan.json */
void heads_json(Buf *b, const Vec *heads, int n)
{
    put(b,"[");
    for(int i=0;i<n;i++) {
        if(i) put(b,",");
        put(b,"[");put_dec(b,heads[i].x);put(b,",");put_dec(b,heads[i].y);put(b,",");put_dec(b,heads[i].z);put(b,"]");
    }
    put(b,"]");
}
int heads_parse(const char *text, usize n, Vec *heads)
{
    /* Finds "heads":[[x,y,z],...] in a scan.json written by heads_json; anything else gives no heads. */
    const char *key="\"heads\"";
    usize at=0;
    for(;at+7<=n;at++) {
        usize k=0;while(k<7 && text[at+k]==key[k]) k++;
        if(k==7) break;
    }
    if(at+7>n) return 0;
    char *p=(char *)text+at+7;const char *end=text+n;
    while(p<end && (is_space(*p) || *p=='\n' || *p==':')) p++;
    if(p>=end || *p!='[') return 0;
    p++;
    int count=0;
    while(p<end && count<MAX_HEADS) {
        while(p<end && (is_space(*p) || *p=='\n' || *p==',')) p++;
        if(p>=end || *p==']') break;
        if(*p!='[') return 0;
        p++;
        float v[3];
        for(int i=0;i<3;i++) {
            while(p<end && (is_space(*p) || *p==',')) p++;
            if(!parse_number(&p,&v[i])) return 0;
        }
        while(p<end && is_space(*p)) p++;
        if(p>=end || *p!=']') return 0;
        p++;
        heads[count++]=vec(v[0],v[1],v[2]);
    }
    return count;
}
static void key_float(Buf *b, const char *k, float v) { put(b,",\"");put(b,k);put(b,"\":");put_dec(b,v); }
static void key_int(Buf *b, const char *k, i64 v, int first)
{
    if(!first) put(b,",");
    put(b,"\"");put(b,k);put(b,"\":");put_int(b,v);
}
void settings_json(Buf *b, const Refine *o)
{
    static const char *walls[]={"off","snap","panels"};
    put(b,"{");key_int(b,"refine",o->refine,1);
    put(b,",\"walls\":\"");put(b,walls[o->walls]);put(b,"\"");
    key_float(b,"band",o->band);key_float(b,"facing_deg",o->facing_deg);key_float(b,"band_in",o->band_in);
    key_float(b,"floor_in",o->floor_in);key_float(b,"ceil_in",o->ceil_in);key_float(b,"top_strip",o->top_strip);
    key_float(b,"near",o->near);key_float(b,"clip",o->clip);key_float(b,"clip_open",o->clip_open);
    key_float(b,"island",o->island);key_int(b,"smooth",o->smooth,0);key_float(b,"tops",o->tops);
    key_int(b,"protect",o->protect,0);key_float(b,"tubes",o->tubes);key_float(b,"tube_thick",o->tube_thick);
    key_float(b,"walked",o->walked);key_int(b,"label_rules",o->label_rules,0);
    put(b,",\"erase\":[");
    for(int i=0;i<o->nerase;i++) {
        const Sphere *e=&o->erase[i];
        if(i) put(b,",");
        put(b,"[");put_dec(b,e->x);put(b,",");put_dec(b,e->y);put(b,",");put_dec(b,e->z);put(b,",");put_dec(b,e->r);put(b,"]");
    }
    put(b,"]}");
}
void report_json(Buf *b, const Report *r, int nlabels, int changed, int removed, int nsuggest)
{
    static const char *kinds[]={"wall","floor","ceiling"};
    put(b,"{");key_int(b,"triangles",r->triangles,1);key_int(b,"kept",r->kept,0);
    key_int(b,"structural",r->structural,0);key_int(b,"near_walls",r->near,0);key_int(b,"outside",r->outside,0);
    key_int(b,"small_pieces",r->islands,0);key_int(b,"thin_pieces",r->tubes,0);key_int(b,"erased",r->erased,0);
    key_int(b,"walked",r->walked,0);key_int(b,"tops",r->tops,0);key_int(b,"panel_triangles",r->panels,0);
    key_int(b,"truncated",r->truncated,0);
    put(b,",\"walked_skipped\":");put(b,r->walked_skipped ? "true" : "false");
    key_float(b,"floor_y",r->floor_y);key_float(b,"ceiling_y",r->ceiling_y);
    key_int(b,"boxes",nlabels,0);key_int(b,"relabelled",changed,0);key_int(b,"removed_boxes",removed,0);
    key_int(b,"suggestions",nsuggest,0);
    put(b,",\"plane_shifts\":[");
    for(int i=0;i<r->nshifts;i++) {
        if(i) put(b,",");
        put(b,"{\"kind\":\"");put(b,kinds[r->shifts[i].kind]);put(b,"\"");
        if(!r->shifts[i].kind) { put(b,",\"wall\":");put_int(b,r->shifts[i].wall); }
        key_float(b,"shift",r->shifts[i].shift);put(b,"}");
    }
    put(b,"]}");
}
