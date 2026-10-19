/* SPDX-License-Identifier: GPL-3.0-only */
/* Label rules on the scanner's furniture boxes (the owner's tuned rules, 2026-10-18): boxes outside the room,
 * boxes that merged several things, boxes nested in a bigger one are removed; the rest are relabelled BED, COUCH,
 * TABLE, STORAGE, SCREEN or OTHER by shape; tall thin slabs from the floor become door/closet suggestions (never
 * applied automatically). Every decision is reported with a reason in scan.json. */
#include "refine.h"

#define R_INSIDE .1f         /* m: a centre further outside the outline is behind a wall */
#define R_NEAR_WALL .35f
#define R_BED_AREA 2.f
#define R_BED_LONG 1.8f
#define R_BED_MAX 3.f
#define R_BED_TOP .65f
#define R_BED_MIN_TOP .25f
#define R_COUCH_MIN_TOP .35f
#define R_COUCH_TOP .6f
#define R_COUCH_LONG 1.4f
#define R_TABLE_MIN .6f
#define R_TABLE_MAX 1.1f
#define R_STORAGE_H 1.1f
#define R_STORAGE_D .25f
#define R_SCREEN_BOTTOM .3f
#define R_SCREEN_TOP 2.2f
#define R_SCREEN_ASPECT 1.2f
#define R_SCREEN_PARALLEL 15.f  /* degrees off the nearest wall */
#define R_SCREEN_FLAT .03f      /* m: spread of the scan's depth inside the box */
#define R_THIN .22f
#define R_TALL_MIN 1.8f
#define R_TALL_FLOOR .3f        /* m: a tall slab's bottom at most this far above the floor */
#define R_TALL_BEHIND .5f
#define R_DOOR_MAX 1.1f
#define R_MERGE .6f             /* share of a box's footprint inside another that counts as held by it */
#define R_MERGE_APART 1.f
#define R_NESTED .75f
#define FLAT_MIN_TRIANGLES 30
#define FLAT_DEPTH_PAD .05f
#define FOOTPRINT_STEP .05

typedef struct { double cs, sn; } Rot;
static Rot rot(const RVol *v) { double a=v->yaw*3.14159265358979323846/180; return (Rot){dcos(a),dsin(a)}; }
static void local(const RVol *v, Rot r, double x, double z, double *lx, double *lz)
{
    double dx=x-v->cx,dz=z-v->cz;
    *lx=r.cs*dx-r.sn*dz;*lz=r.sn*dx+r.cs*dz;
}
static double dabs(double x) { return x<0 ? -x : x; }
static void corners(const RVol *v, Rot r, double out[4][2])
{
    static const int sg[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
    for(int k=0;k<4;k++) {
        double x=sg[k][0]*v->w/2.,z=sg[k][1]*v->d/2.;
        out[k][0]=v->cx+r.cs*x+r.sn*z;out[k][1]=v->cz-r.sn*x+r.cs*z;
    }
}
static int point_in_poly(double x, double z, const DPt *poly, int n)
{
    int inside=0;
    for(int i=0,j=n-1;i<n;j=i++) {
        double xi=poly[i].x,zi=poly[i].z,xj=poly[j].x,zj=poly[j].z;
        if((zi>z)!=(zj>z) && x<(xj-xi)*(z-zi)/(zj-zi+1e-12)+xi) inside=!inside;
    }
    return inside;
}
typedef struct { double dist, t, L, ux, uz; int i; } Near;
static Near nearest_wall(const Room *r, double x, double z)
{
    Near best={1e300,0,0,1,0,-1};
    for(int i=0;i<r->nwalls;i++) {
        const RWall *w=&r->walls[i];
        double abx=w->x1-w->x0,abz=w->z1-w->z0,l2=abx*abx+abz*abz,L=dhypot(abx,abz);
        if(!(l2>0)) continue;
        double t=((x-w->x0)*abx+(z-w->z0)*abz)/l2;
        t=t<0 ? 0 : t>1 ? 1 : t;
        double dx=x-(w->x0+t*abx),dz=z-(w->z0+t*abz),d=dhypot(dx,dz);
        if(d<best.dist) best=(Near){d,t,L,abx/L,abz/L,i};
    }
    return best;
}
static double outside(const Room *r, double x, double z)
{
    return point_in_poly(x,z,r->outline,r->noutline) ? 0 : nearest_wall(r,x,z).dist;
}
/* Spread (std) of the scan's depth inside a thin upright box; <0 = too little scan to tell. */
static double flatness(const Prep *p, const RVol *v)
{
    Rot r=rot(v);
    double sum=0,sum2=0;int n=0;
    for(int pass=0;pass<2;pass++) {
        double mean=n ? sum/n : 0;
        for(int t=0;t<p->T;t++) {
            double y=p->cen[3*t+1];
            if(y>v->top || y<v->top-v->h) continue;
            double lx,lz;local(v,r,p->cen[3*t],p->cen[3*t+2],&lx,&lz);
            double a=lx,b=lz,half=v->w/2.,depth=v->d/2.+FLAT_DEPTH_PAD;
            if(v->d>v->w) { a=lz;b=lx;half=v->d/2.;depth=v->w/2.+FLAT_DEPTH_PAD; }
            if(dabs(a)<=half && dabs(b)<=depth) {
                if(!pass) { sum+=b;n++; } else sum2+=(b-mean)*(b-mean);
            }
        }
        if(!pass && n<FLAT_MIN_TRIANGLES) return -1;
    }
    return droot(sum2/n);
}
static void cm(Buf *b, double x) { put_round(b,x*100);put(b," cm"); }
static void lower(Buf *b, const char *s)
{
    char c[2]={0,0};
    for(;*s;s++) { c[0]=*s>='A' && *s<='Z' ? (char)(*s+32) : *s;put(b,c); }
}
static void set_text(char *dst, usize cap, Buf *b)
{
    usize n=b->ok ? b->n : 0;
    if(n>=cap) {  /* cut at a UTF-8 boundary */
        n=cap-1;
        while(n && ((u8)b->data[n]&0xc0)==0x80) n--;
    }
    if(n) memcpy(dst,b->data,n);
    dst[n]=0;
}
static void copy(char *dst, const char *src, usize cap)
{
    usize i=0;while(src[i] && i+1<cap) { dst[i]=src[i];i++; }
    dst[i]=0;
}
/* label "" = remove, suggest "door"/"closet" or "" */
static void classify(const Room *room, const Prep *p, const RVol *v, char *label, char *suggest, Buf *why)
{
    double out=outside(room,v->cx,v->cz);
    double top=v->top-room->floor,bottom=top-v->h,lng=v->w>v->d ? v->w : v->d,shrt=v->w<v->d ? v->w : v->d;
    label[0]=suggest[0]=0;
    if(shrt<R_THIN && v->h>=R_TALL_MIN && bottom<=R_TALL_FLOOR && out<=R_TALL_BEHIND) {
        int door=lng<=R_DOOR_MAX;
        copy(suggest,door ? "door" : "closet",8);
        put(why,"tall thin slab from the floor (");put_fixed(why,v->h,2);put(why," m)");
        if(out>0) { put(why,", ");cm(why,out);put(why," behind the wall line"); }
        put(why,door ? ": a door or closet door?" : ": a closet front?");
        return;
    }
    if(out>R_INSIDE) { put(why,"centre ");cm(why,out);put(why," outside the room (behind a wall)");return; }
    Near wall=nearest_wall(room,v->cx,v->cz);
    if(shrt<R_THIN) {
        Rot r=rot(v);
        double ax=v->w>=v->d ? r.cs : r.sn,az=v->w>=v->d ? -r.sn : r.cs;
        double c=dabs(ax*wall.ux+az*wall.uz);c=c<1 ? c : 1;
        double off=angle((float)droot(1-c*c),(float)c)*180/PI;
        double fl=flatness(p,v);
        Buf b;buf_init(&b,1024);int n=0;
        if(bottom<R_SCREEN_BOTTOM) { put(&b,"starts ");cm(&b,bottom>0 ? bottom : 0);put(&b," off the floor");n++; }
        if(top>R_SCREEN_TOP) { if(n++) put(&b,", ");put(&b,"top at ");put_fixed(&b,top,2);put(&b," m"); }
        if(lng/v->h<R_SCREEN_ASPECT) { if(n++) put(&b,", ");put(&b,"taller than wide (");put_fixed(&b,lng/v->h,2);put(&b,")"); }
        if(wall.dist<R_NEAR_WALL && off>R_SCREEN_PARALLEL) { if(n++) put(&b,", ");put_round(&b,off);put(&b,"\xc2\xb0 off the wall"); }
        if(fl>=0 && fl>R_SCREEN_FLAT) { if(n++) put(&b,", ");put(&b,"not flat (");cm(&b,fl);put(&b," spread)"); }
        if(!n) {
            copy(label,"SCREEN",MAX_LABEL);put(why,"thin, raised, screen-shaped");
            if(fl>=0) { put(why,", flat (");cm(why,fl);put(why,")"); }
        } else { put(why,"thin upright slab: ");put_n(why,b.data,b.n); }
        buf_free(&b);
        return;
    }
    if(top<=R_BED_TOP && v->w*v->d>=R_BED_AREA && lng>=R_BED_LONG && lng<=R_BED_MAX && top>=R_BED_MIN_TOP) {
        copy(label,"BED",MAX_LABEL);put_fixed(why,(double)v->w*v->d,1);put(why," m\xc2\xb2, ");put_fixed(why,lng,1);
        put(why," m long, top ");put_fixed(why,top,2);put(why," m");return;
    }
    if(top>=R_COUCH_MIN_TOP && top<=R_COUCH_TOP && lng>=R_COUCH_LONG) {
        copy(label,"COUCH",MAX_LABEL);put(why,"seat-height top ");put_fixed(why,top,2);put(why," m, ");
        put_fixed(why,lng,1);put(why," m long");return;
    }
    if(top>=R_TABLE_MIN && top<=R_TABLE_MAX) { copy(label,"TABLE",MAX_LABEL);put(why,"top ");put_fixed(why,top,2);put(why," m");return; }
    if(v->h>=R_STORAGE_H && shrt>=R_STORAGE_D && wall.dist<R_NEAR_WALL) {
        copy(label,"STORAGE",MAX_LABEL);put_fixed(why,v->h,2);put(why," m tall by a wall");return;
    }
    if(top>R_TABLE_MAX) { put_fixed(why,top,2);put(why," m tall, matches nothing");return; }
    copy(label,"OTHER",MAX_LABEL);put(why,"top ");put_fixed(why,top,2);put(why," m, ");put_fixed(why,lng,1);
    put(why," \xc3\x97 ");put_fixed(why,shrt,1);put(why," m");
}
/* Share of a's and b's footprint they have in common, sampled on a 5 cm grid over both (as the reference). */
static void footprint_overlap(const RVol *a, const RVol *b, double *fa, double *fb)
{
    Rot ra=rot(a),rb=rot(b);
    double ca[4][2],cb[4][2],x0=1e300,x1=-1e300,z0=1e300,z1=-1e300;
    corners(a,ra,ca);corners(b,rb,cb);
    for(int k=0;k<4;k++) {
        x0=ca[k][0]<x0 ? ca[k][0] : x0;x1=ca[k][0]>x1 ? ca[k][0] : x1;z0=ca[k][1]<z0 ? ca[k][1] : z0;z1=ca[k][1]>z1 ? ca[k][1] : z1;
        x0=cb[k][0]<x0 ? cb[k][0] : x0;x1=cb[k][0]>x1 ? cb[k][0] : x1;z0=cb[k][1]<z0 ? cb[k][1] : z0;z1=cb[k][1]>z1 ? cb[k][1] : z1;
    }
    double na=0,nb=0,both=0;
    for(double x=x0;x<=x1;x+=FOOTPRINT_STEP) for(double z=z0;z<=z1;z+=FOOTPRINT_STEP) {
        double ax,az,bx,bz;local(a,ra,x,z,&ax,&az);local(b,rb,x,z,&bx,&bz);
        int ia=dabs(ax)<=a->w/2. && dabs(az)<=a->d/2.,ib=dabs(bx)<=b->w/2. && dabs(bz)<=b->d/2.;
        na+=ia;nb+=ib;both+=ia && ib;
    }
    *fa=both/(na>1 ? na : 1);*fb=both/(nb>1 ? nb : 1);
}
static int boxes_apart(const RVol *a, const RVol *b)
{
    /* footprints whose bounding circles don't touch share nothing (skips the sampling, same result) */
    double ra=droot((double)a->w*a->w+(double)a->d*a->d)/2,rb=droot((double)b->w*b->w+(double)b->d*b->d)/2;
    double dx=a->cx-b->cx,dz=a->cz-b->cz;
    return dx*dx+dz*dz>(ra+rb+.1)*(ra+rb+.1);
}
int rules_apply(const Room *room, const Prep *p, RVol *out, int *nout, Label *labels)
{
    int n=room->nvols;const RVol *box=room->vols;
    typedef struct { int i, j; double fa, fb; } Ov;
    static Ov ov[MAX_VOLUMES*(MAX_VOLUMES-1)/2];
    int nov=0,removed[MAX_VOLUMES]={0};
    static char reason[MAX_VOLUMES][MAX_REASON];
    for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) {
        if(boxes_apart(&box[i],&box[j])) continue;
        double fa,fb;footprint_overlap(&box[i],&box[j],&fa,&fb);
        if(fa>0 || fb>0) ov[nov++]=(Ov){i,j,fa,fb};
    }
    /* merged: a box that holds most of two boxes far apart covers several things */
    for(int k=0;k<n;k++) {
        int held[MAX_VOLUMES],nh=0;
        for(int q=0;q<nov;q++) if(ov[q].i==k && ov[q].fb>=R_MERGE) held[nh++]=ov[q].j;
        for(int q=0;q<nov;q++) if(ov[q].j==k && ov[q].fa>=R_MERGE) held[nh++]=ov[q].i;
        for(int x=0;x<nh && !removed[k];x++) for(int y=0;y<nh && !removed[k];y++) {
            const RVol *a=&box[held[x]],*b=&box[held[y]];
            double dx=a->cx-b->cx,dz=a->cz-b->cz,d=dhypot(dx,dz);
            if(held[x]<held[y] && d>=R_MERGE_APART) {
                Buf t;buf_init(&t,1024);
                put(&t,"holds a ");lower(&t,a->label);put(&t," and a ");lower(&t,b->label);put(&t," ");
                put_fixed(&t,d,1);put(&t," m apart: several things merged");
                set_text(reason[k],MAX_REASON,&t);buf_free(&t);removed[k]=1;
            }
        }
    }
    /* nested: mostly inside a bigger box with a lower top = part of it */
    for(int q=0;q<nov;q++) for(int side=0;side<2;side++) {
        int s=side ? ov[q].j : ov[q].i,g=side ? ov[q].i : ov[q].j;double f=side ? ov[q].fb : ov[q].fa;
        const RVol *S=&box[s],*G=&box[g];
        if(removed[s] || removed[g] || f<R_NESTED || (double)S->w*S->d>=(double)G->w*G->d || S->top>G->top+.05) continue;
        Buf t;buf_init(&t,1024);
        put_round(&t,f*100);put(&t,"% inside the bigger ");lower(&t,G->label);put(&t," box: part of it");
        set_text(reason[s],MAX_REASON,&t);buf_free(&t);removed[s]=1;
    }
    int count=0;
    for(int i=0;i<n;i++) {
        Label *l=&labels[i];
        memset(l,0,sizeof(*l));
        copy(l->from,box[i].label,MAX_LABEL);l->box=box[i];l->wall=-1;
        if(removed[i]) { copy(l->reason,reason[i],MAX_REASON);continue; }
        Buf why;buf_init(&why,1024);
        classify(room,p,&box[i],l->to,l->suggest,&why);
        set_text(l->reason,MAX_REASON,&why);buf_free(&why);
        if(l->suggest[0]) {
            Near nw=nearest_wall(room,box[i].cx,box[i].cz);
            l->wall=nw.i;l->along=(float)nw.t;
        }
        if(l->to[0]) { out[count]=box[i];copy(out[count].label,l->to,MAX_LABEL);count++; }
    }
    *nout=count;
    return n;
}
