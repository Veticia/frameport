/* SPDX-License-Identifier: GPL-3.0-only */
#include "refine.h"

static int error_json(const char *error)
{
    Buf t;buf_init(&t,4096);put(&t,"{\"ok\":false,\"error\":\"");put(&t,error);put(&t,"\"}\n");
    io_write(1,t.data,t.n);buf_free(&t);return 2;
}
static int parse_float(const char *p,float *out)
{
    int digits=0;float n=0,scale=1;
    while(*p>='0' && *p<='9') { n=n*10+*p++-'0';digits++;if(n>10000) return 0; }
    if(*p=='.') { p++;while(*p>='0' && *p<='9') { scale*=.1f;n+=(*p++-'0')*scale;digits++; } }
    if(!digits || *p || !finite(n)) return 0;
    *out=n;return 1;
}
static int parse_universe(const char *p,u64 *out)
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
/* "x,y,z,r;x,y,z,r;..." (metres, STAGE space) */
static int parse_erase(const char *value, Refine *o)
{
    char *p=(char *)value;o->nerase=0;
    while(*p) {
        if(o->nerase==MAX_ERASE) return 0;
        float v[4];
        for(int i=0;i<4;i++) {
            if(!parse_number(&p,&v[i])) return 0;
            if(i<3) { if(*p!=',') return 0;p++; }
        }
        if(v[3]<=0 || v[3]>5) return 0;
        o->erase[o->nerase++]=(Sphere){v[0],v[1],v[2],v[3]};
        while(*p==' ') p++;
        if(*p==';') p++;
        else if(*p) return 0;
    }
    return 1;
}
static const char *options(Options *o,int argc,char **argv)
{
    *o=(Options){.voxel=.05f,.ceiling_default=2.5f,.truncation=3,.max_edge=.6f,.grazing=.12f,.range=4.f,
        .depth_jump=.25f,.depth_ratio=.12f,.min_views=5,.patch_residual=.03f,
        .plane_distance=.045f,.min_plane_area=.45f,.wall_height=1.5f,.snap_degrees=5,.floor_band=.35f,
        .ceiling_min=1.8f,.min_volume=.05f,.cluster_gap=.10f,.gap=.65f,.horizontal=.85f,
        /* The owner's refinement settings (room-scan cleanup page, 2026-10-18). */
        .rf={.refine=1,.walls=2,.protect=0,.label_rules=1,.smooth=8,.band=.35f,.facing_deg=60,.band_in=.12f,
             .floor_in=.08f,.ceil_in=.25f,.top_strip=.5f,.near=.03f,.clip=.05f,.clip_open=.10f,.island=.20f,
             .tops=.10f,.tubes=3,.tube_thick=.15f,.walked=.40f}};
    for(int i=1;i<argc;i++) {
        const char *arg=argv[i];
        if(same(arg,"--json")) continue;
        if(i+1==argc) return "option requires a value";
        const char *value=argv[++i];
        float number=0;
        if(same(arg,"--offline")) o->input=value;
        else if(same(arg,"--out")) o->out=value;
        else if(same(arg,"--hint")) o->hint=value;
        else if(same(arg,"--refine-only")) o->refine_only=value;
        else if(same(arg,"--universe")) { if(!parse_universe(value,&o->universe)) return "invalid universe"; }
        else if(same(arg,"--walls")) {
            if(same(value,"off")) o->rf.walls=0;
            else if(same(value,"snap")) o->rf.walls=1;
            else if(same(value,"panels")) o->rf.walls=2;
            else return "option outside supported range";
        }
        else if(same(arg,"--erase")) { if(!parse_erase(value,&o->rf)) return "invalid erase spheres"; }
        else if(same(arg,"--refine") || same(arg,"--protect") || same(arg,"--label-rules") || same(arg,"--smooth")) {
            int high=same(arg,"--smooth") ? 50 : 1;
            if(!parse_float(value,&number) || number!=(float)(int)number || number>high) return "option outside supported range";
            int *dest=same(arg,"--refine") ? &o->rf.refine : same(arg,"--protect") ? &o->rf.protect :
                      same(arg,"--label-rules") ? &o->rf.label_rules : &o->rf.smooth;
            *dest=(int)number;
        }
        else {
            float *dest=0,low=0,high=100;
            if(same(arg,"--voxel")) { dest=&o->voxel;low=.04f;high=.2f; }
            else if(same(arg,"--ceiling-default")) { dest=&o->ceiling_default;low=1.8f;high=3.8f; }
            else if(same(arg,"--truncation")) { dest=&o->truncation;low=1;high=5; }
            else if(same(arg,"--max-edge")) { dest=&o->max_edge;low=.05f;high=1.5f; }
            else if(same(arg,"--grazing")) { dest=&o->grazing;low=.01f;high=.9f; }
            else if(same(arg,"--range")) { dest=&o->range;low=1;high=10; }
            else if(same(arg,"--depth-jump")) { dest=&o->depth_jump;low=.02f;high=1; }
            else if(same(arg,"--depth-ratio")) { dest=&o->depth_ratio;high=.5f; }
            else if(same(arg,"--min-views")) { dest=&o->min_views;low=1;high=16; }
            else if(same(arg,"--patch-residual")) { dest=&o->patch_residual;low=.005f;high=.1f; }
            else if(same(arg,"--plane-distance")) { dest=&o->plane_distance;low=.01f;high=.15f; }
            else if(same(arg,"--min-plane-area")) { dest=&o->min_plane_area;low=.1f;high=4; }
            else if(same(arg,"--wall-height")) { dest=&o->wall_height;low=.5f;high=3; }
            else if(same(arg,"--snap-degrees")) { dest=&o->snap_degrees;high=15; }
            else if(same(arg,"--floor-band")) { dest=&o->floor_band;low=.05f;high=1; }
            else if(same(arg,"--ceiling-min")) { dest=&o->ceiling_min;low=1.5f;high=3; }
            else if(same(arg,"--min-volume")) { dest=&o->min_volume;low=.005f;high=1; }
            else if(same(arg,"--cluster-gap")) { dest=&o->cluster_gap;low=.04f;high=.2f; }
            else if(same(arg,"--gap")) { dest=&o->gap;low=.1f;high=1.5f; }
            else if(same(arg,"--horizontal")) { dest=&o->horizontal;low=.6f;high=.99f; }
            else if(same(arg,"--band")) { dest=&o->rf.band;low=.02f;high=1; }
            else if(same(arg,"--facing-deg")) { dest=&o->rf.facing_deg;low=5;high=89; }
            else if(same(arg,"--band-in")) { dest=&o->rf.band_in;high=1; }
            else if(same(arg,"--floor-in")) { dest=&o->rf.floor_in;high=1; }
            else if(same(arg,"--ceil-in")) { dest=&o->rf.ceil_in;high=1; }
            else if(same(arg,"--top-strip")) { dest=&o->rf.top_strip;high=2; }
            else if(same(arg,"--near")) { dest=&o->rf.near;high=.3f; }
            else if(same(arg,"--clip")) { dest=&o->rf.clip;high=1; }
            else if(same(arg,"--clip-open")) { dest=&o->rf.clip_open;high=2; }
            else if(same(arg,"--island")) { dest=&o->rf.island;high=5; }
            else if(same(arg,"--tops")) { dest=&o->rf.tops;high=.5f; }
            else if(same(arg,"--tubes")) { dest=&o->rf.tubes;high=20; }
            else if(same(arg,"--tube-thick")) { dest=&o->rf.tube_thick;low=.01f;high=1; }
            else if(same(arg,"--walked")) { dest=&o->rf.walked;high=1.5f; }
            else return "unknown option";
            if(!parse_float(value,dest) || *dest<low || *dest>high) return "option outside supported range";
        }
    }
    if(o->rf.walked>0 && o->rf.walked<.05f) return "option outside supported range";
    if(o->refine_only) return o->input ? "--refine-only takes no --offline dump" : 0;
    return o->input && o->out ? 0 : "required: --offline dump.fprs --out directory";
}

/* ------------------------------------------------------------------------------------------------ refinement */
typedef struct {
    Report rep;
    Label labels[MAX_VOLUMES];
    int nlabels, changed, removed, nsuggest, out_triangles, out_volumes;
    float ceiling_y;
    i64 ms;
} Outcome;
static const char *copy_name(char *dst, const char *src, usize cap)
{
    usize i=0;while(src[i] && i+1<cap) { dst[i]=src[i];i++; }
    dst[i]=0;return dst;
}
static void box_json(Buf *b, const RVol *v)
{
    put(b,"{\"cx\":");put_dec(b,v->cx);put(b,",\"cz\":");put_dec(b,v->cz);put(b,",\"top_y\":");put_dec(b,v->top);
    put(b,",\"yaw_deg\":");put_dec(b,v->yaw);put(b,",\"w\":");put_dec(b,v->w);put(b,",\"d\":");put_dec(b,v->d);
    put(b,",\"h\":");put_dec(b,v->h);put(b,"}");
}
static void scan_json(Buf *b, const Refine *o, const Vec *heads, int nheads, const Outcome *r)
{
    put(b,"{\"version\":1,\"heads\":");heads_json(b,heads,nheads);
    put(b,",\"refine\":");settings_json(b,o);
    put(b,",\"report\":");report_json(b,&r->rep,r->nlabels,r->changed,r->removed,r->nsuggest);
    put(b,",\"labels\":[");
    for(int i=0;i<r->nlabels;i++) {
        const Label *l=&r->labels[i];
        if(i) put(b,",");
        put(b,"{\"from\":\"");put(b,l->from);put(b,"\",\"to\":");
        if(l->to[0]) { put(b,"\"");put(b,l->to);put(b,"\""); } else put(b,"null");
        put(b,",\"reason\":\"");put(b,l->reason);put(b,"\"");
        if(l->suggest[0]) { put(b,",\"suggest\":\"");put(b,l->suggest);put(b,"\""); }
        put(b,",\"id\":\"");put(b,l->box.id);put(b,"\",\"box\":");box_json(b,&l->box);put(b,"}");
    }
    put(b,"],\"suggestions\":[");
    int first=1;
    for(int i=0;i<r->nlabels;i++) {
        const Label *l=&r->labels[i];
        if(!l->suggest[0]) continue;
        if(!first) put(b,",");
        first=0;
        put(b,"{\"kind\":\"");put(b,l->suggest);put(b,"\",\"box\":");box_json(b,&l->box);
        put(b,",\"wall\":");put_int(b,l->wall);put(b,",\"along\":");put_dec(b,l->along);
        put(b,",\"reason\":\"");put(b,l->reason);put(b,"\"}");
    }
    put(b,"]}\n");
}
/* Refines `raw` (as read back from its own text) and publishes: [raw mesh, raw text,] scan.json, room-mesh.bin,
 * room.txt. Everything is computed before the first file is written, so a failure leaves the old files. */
static const char *refine_publish(const char *dir, const Room *raw, const Vec *v, const u32 *idx, int nv, int ni,
                                  const Vec *heads, int nheads, const Refine *o, int write_raw, Outcome *res)
{
    static Room refined;
    static RVol ruled[MAX_VOLUMES];
    const char *error=0;
    Prep prep={0};
    Vec *rv=0;u32 *ri=0;int rn=0;
    u8 *raw_mesh=0,*mesh=0;usize raw_size=0,mesh_size=0;
    Buf raw_text,text,json;
    buf_init(&raw_text,1u<<20);buf_init(&text,1u<<20);buf_init(&json,MAX_JSON_FILE);
    i64 t0=io_time(1);
    memset(res,0,sizeof(*res));
    refined=*raw;
    copy_name(refined.mesh_name,"room-mesh.bin",sizeof(refined.mesh_name));
    if(o->refine) {
        error=prep_mesh(&prep,v,idx,nv,ni);
        int nruled=raw->nvols;
        if(!error) {
            if(o->label_rules) res->nlabels=rules_apply(raw,&prep,ruled,&nruled,res->labels);
            else memcpy(ruled,raw->vols,sizeof(RVol)*(usize)raw->nvols);
        }
        if(!error) error=refine_run(o,raw,&prep,ruled,nruled,heads,nheads,&rv,&ri,&rn,&res->rep);
        if(!error) {
            refined.ceiling=res->rep.ceiling_y;
            for(int i=0;i<refined.nwalls;i++) if(refined.walls[i].y1==raw->ceiling) refined.walls[i].y1=refined.ceiling;
            memcpy(refined.vols,ruled,sizeof(RVol)*(usize)nruled);refined.nvols=nruled;
            refined.mesh_nv=refined.mesh_ni=(u32)rn;refined.has_mesh=1;
            for(int i=0;i<res->nlabels;i++) {
                Label *l=&res->labels[i];
                if(l->suggest[0]) res->nsuggest++;
                if(!l->to[0]) res->removed++;
                else if(!same(l->to,l->from)) res->changed++;
            }
        }
    }
    if(!error) error=mesh_bytes(rv ? rv : v,rv ? ri : idx,rv ? rn : nv,rv ? rn : ni,&mesh,&mesh_size);
    if(!error && write_raw) error=mesh_bytes(v,idx,nv,ni,&raw_mesh,&raw_size);
    if(!error && write_raw && !room_text(raw,&raw_text)) error="room text exceeds output budget";
    if(!error && !room_text(&refined,&text)) error="room text exceeds output budget";
    if(!error) { scan_json(&json,o,heads,nheads,res);if(!json.ok) error="scan.json exceeds output budget"; }
    res->ms=io_time(1)-t0;
    res->out_triangles=(int)refined.mesh_ni/3;res->out_volumes=refined.nvols;res->ceiling_y=refined.ceiling;
    if(!error && !io_mkdir(dir)) error="cannot create output directory";
    if(!error && write_raw) error=publish(dir,"room-mesh-raw.bin",raw_mesh,raw_size);
    if(!error && write_raw) error=publish(dir,"room-raw.txt",raw_text.data,raw_text.n);
    if(!error) error=publish(dir,"scan.json",json.data,json.n);
    if(!error) error=publish(dir,"room-mesh.bin",mesh,mesh_size);
    if(!error) error=publish(dir,"room.txt",text.data,text.n);
    prep_free(&prep);
    io_free(rv,(usize)MAX_MESH_VERTICES*sizeof(Vec));io_free(ri,(usize)MAX_TRIANGLES*3*sizeof(u32));
    io_free(raw_mesh,raw_size);io_free(mesh,mesh_size);
    buf_free(&raw_text);buf_free(&text);buf_free(&json);
    return error;
}
static void refine_summary(Buf *t, const Outcome *r)
{
    put(t,"\"refine\":");report_json(t,&r->rep,r->nlabels,r->changed,r->removed,r->nsuggest);
    if(t->ok && t->n) t->n--;  /* reopen the object for the output counts */
    put(t,",\"output_triangles\":");put_int(t,r->out_triangles);
    put(t,",\"output_volumes\":");put_int(t,r->out_volumes);put(t,"}");
}

/* ------------------------------------------------------------------------------------------------ summaries */
static void duration(Buf *t,i64 a,i64 b) { put_dec(t,(float)(b-a)/1000000); }
static void warnings_json(Buf *t, const char **w, int n)
{
    put(t,",\"warnings\":[");
    for(int i=0;i<n;i++) { if(i) put(t,",");put(t,"\"");put(t,w[i]);put(t,"\""); }
    put(t,"]");
}
static void summary(Scan *s, const Outcome *r)
{
    Buf t;buf_init(&t,1u<<16);put(&t,"{\"ok\":true,\"frames\":");put_u64(&t,s->frames);
    put(&t,",\"skipped_frames\":");put_u64(&t,s->skipped_frames);
    put(&t,",\"far_vertices\":");put_u64(&t,s->far_vertices);
    put(&t,",\"discontinuity_triangles\":");put_u64(&t,s->discontinuities);
    put(&t,",\"walls\":");put_u64(&t,(u64)s->noutline);
    put(&t,",\"detected_walls\":");put_u64(&t,(u64)s->nplanes);
    put(&t,",\"volumes\":");put_u64(&t,(u64)s->nvolumes);
    put(&t,",\"floor_y\":");put_dec(&t,s->floor);put(&t,",\"ceiling_y\":");put_dec(&t,s->ceiling);
    put(&t,",\"coverage_percent\":");put_dec(&t,s->coverage);
    put(&t,",\"hint_used\":");put(&t,s->hint_used ? "true" : "false");
    put(&t,",\"triangles\":");put_u64(&t,(u64)s->ni/3);
    put(&t,",\"accepted_triangles\":");put_u64(&t,s->accepted);
    put(&t,",\"rejected_triangles\":");put_u64(&t,s->rejected);
    put(&t,",\"heads\":");put_int(&t,s->nheads);put(&t,",");
    refine_summary(&t,r);
    warnings_json(&t,s->warnings,s->nwarnings);
    put(&t,",\"timings_ms\":{\"read\":");duration(&t,s->t0,s->tread);
    put(&t,",\"fusion\":");duration(&t,s->tread,s->tfuse);
    put(&t,",\"model\":");duration(&t,s->tfuse,s->tmodel);
    put(&t,",\"mesh\":");duration(&t,s->tmodel,s->tmesh);
    put(&t,",\"refine\":");duration(&t,s->tmesh,s->trefine);
    put(&t,",\"total\":");duration(&t,s->t0,io_time(1));put(&t,"}}\n");io_write(1,t.data,t.n);buf_free(&t);
}

/* ------------------------------------------------------------------------------------------------ scan path */
static const char *scan_outputs(Scan *s, Outcome *out)
{
    static Room built,raw;
    static Vec heads[MAX_HEADS];
    const char *error=room_from_scan(s,&built);
    if(error) return error;
    copy_name(built.mesh_name,"room-mesh-raw.bin",sizeof(built.mesh_name));
    /* Refine what the raw files will hold, so --refine-only on them gives the same refined files. */
    Buf t;buf_init(&t,1u<<20);
    if(!room_text(&built,&t)) { buf_free(&t);return "room text exceeds output budget"; }
    error=room_parse(t.data,t.n,&raw);
    buf_free(&t);
    if(error) return error;
    Buf h;buf_init(&h,1u<<20);heads_json(&h,s->heads,s->nheads);put(&h,"}");
    Buf wrap;buf_init(&wrap,1u<<20);put(&wrap,"{\"heads\":");put_n(&wrap,h.data,h.n);
    int nheads=wrap.ok ? heads_parse(wrap.data,wrap.n,heads) : 0;
    buf_free(&h);buf_free(&wrap);
    return refine_publish(s->opt.out,&raw,s->vertices,s->indices,s->nv,s->ni,heads,nheads,&s->opt.rf,1,out);
}

/* ------------------------------------------------------------------------------------------------ --refine-only */
static int refine_only(Options *o)
{
    static Room raw;
    static Vec heads[MAX_HEADS];
    static Outcome res;
    const char *warns[4];int nwarn=0;
    i64 t0=io_time(1);
    const char *dir=o->refine_only,*error=0;
    char path[4096];
    u8 *text=0,*bin=0,*json=0;usize text_n=0,bin_n=0,json_n=0;
    Vec *v=0;u32 *idx=0;int nv=0,ni=0,nheads=0,write_raw=0;
    if(!join_path(path,dir,"room-raw.txt")) return error_json("output path too long");
    error=read_file(path,1u<<20,&text,&text_n);
    if(error) {
        /* An older room folder: its room.txt + mesh are the unrefined scan. */
        write_raw=1;
        if(!join_path(path,dir,"room.txt")) return error_json("output path too long");
        error=read_file(path,1u<<20,&text,&text_n);
        if(error) return error_json("room folder has neither room-raw.txt nor room.txt");
    }
    error=room_parse((const char *)text,text_n,&raw);
    io_free(text,(1u<<20)+1);
    if(error) return error_json(error);
    if(!raw.has_mesh) return error_json("room has no mesh to refine");
    if(!join_path(path,dir,write_raw ? raw.mesh_name : "room-mesh-raw.bin")) return error_json("output path too long");
    error=read_file(path,MAX_RAW_FILE,&bin,&bin_n);
    if(!error) error=mesh_parse(bin,bin_n,&v,&idx,&nv,&ni);
    io_free(bin,MAX_RAW_FILE+1);
    if(error) return error_json(error);
    if((u32)nv!=raw.mesh_nv || (u32)ni!=raw.mesh_ni) {
        io_free(v,(usize)MAX_MESH_VERTICES*sizeof(Vec));io_free(idx,(usize)MAX_TRIANGLES*3*sizeof(u32));
        return error_json("mesh record does not match the mesh file");
    }
    copy_name(raw.mesh_name,"room-mesh-raw.bin",sizeof(raw.mesh_name));
    if(join_path(path,dir,"scan.json") && !read_file(path,MAX_JSON_FILE,&json,&json_n)) {
        nheads=heads_parse((const char *)json,json_n,heads);
        io_free(json,MAX_JSON_FILE+1);
    }
    if(!nheads && o->rf.walked>0) warns[nwarn++]="no head path; walked volume skipped";
    if(write_raw) warns[nwarn++]="no raw files; the current room was taken as the unrefined scan";
    error=refine_publish(dir,&raw,v,idx,nv,ni,heads,nheads,&o->rf,write_raw,&res);
    io_free(v,(usize)MAX_MESH_VERTICES*sizeof(Vec));io_free(idx,(usize)MAX_TRIANGLES*3*sizeof(u32));
    if(error) return error_json(error);
    if(res.rep.truncated) warns[nwarn++]="refined mesh truncated to the triangle budget";
    Buf t;buf_init(&t,1u<<16);
    put(&t,"{\"ok\":true,\"refine_only\":true,\"raw_triangles\":");put_int(&t,ni/3);
    put(&t,",\"volumes\":");put_int(&t,raw.nvols);put(&t,",\"floor_y\":");put_dec(&t,raw.floor);
    put(&t,",\"ceiling_y\":");put_dec(&t,res.ceiling_y);put(&t,",\"heads\":");put_int(&t,nheads);put(&t,",");
    refine_summary(&t,&res);
    warnings_json(&t,warns,nwarn);
    put(&t,",\"timings_ms\":{\"refine\":");put_dec(&t,(float)res.ms/1000000);
    put(&t,",\"total\":");duration(&t,t0,io_time(1));put(&t,"}}\n");
    io_write(1,t.data,t.n);buf_free(&t);
    return 0;
}

int main(int argc,char **argv)
{
    if(argc==2 && same(argv[1],"--help")) {
        const char usage[]="fp_roomscan --offline dump.fprs --out directory [--universe N] [--voxel 0.05]\n"
            "  [--ceiling-default 2.5] [--hint outline.txt] [--json]\n"
            "fp_roomscan --refine-only room-directory [refinement options] [--json]\n"
            "Tuning: --truncation --max-edge --grazing --range --depth-jump --depth-ratio --plane-distance --min-plane-area\n"
            "  --min-views --patch-residual --wall-height --snap-degrees --floor-band --ceiling-min\n"
            "  --min-volume --cluster-gap --gap --horizontal\n"
            "Refinement: --refine 0|1 --walls off|snap|panels --band --facing-deg --band-in --floor-in --ceil-in\n"
            "  --top-strip --near --clip --clip-open --island --smooth --tops --protect 0|1 --tubes --tube-thick\n"
            "  --walked --label-rules 0|1 --erase \"x,y,z,r;...\"\n"
            "Always prints one JSON summary; errors exit 2. See docs/mr/scan.md for units and ranges.\n";
        io_write(1,usage,sizeof(usage)-1);return 0;
    }
    Scan *s=io_alloc(sizeof(Scan));
    if(!s) return error_json("scan allocation failed");
    s->rng=0x714ce3u;s->t0=io_time(1);
    const char *error=options(&s->opt,argc,argv);
    if(!error && s->opt.refine_only) {
        int result=refine_only(&s->opt);
        io_free(s,sizeof(Scan));return result;
    }
    static Outcome outcome;
    if(!error) error=read_dump(s,0);
    s->tread=io_time(1);
    if(!error) error=init_grid(s);
    if(!error) error=read_dump(s,1);
    if(!error) finish_fusion(s);
    s->tfuse=io_time(1);
    if(!error) error=make_model(s);
    s->tmodel=io_time(1);
    if(!error) error=make_mesh(s);
    s->tmesh=io_time(1);
    /* The grid is not needed after the mesh: freeing it keeps the refinement inside the same memory budget. */
    io_free(s->cells,(usize)s->count*sizeof(Cell));s->cells=0;
    if(!error) error=scan_outputs(s,&outcome);
    s->trefine=io_time(1);
    if(!error && outcome.rep.truncated) warn(s,"refined mesh truncated to the triangle budget");
    int result=0;
    if(error) result=error_json(error);else summary(s,&outcome);
    io_free(s->vertices,(usize)MAX_MESH_VERTICES*sizeof(Vec));
    io_free(s->indices,(usize)MAX_TRIANGLES*3*sizeof(u32));
    io_free(s,sizeof(Scan));return result;
}
