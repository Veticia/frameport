/* SPDX-License-Identifier: GPL-3.0-only */
/* Mesh refinement: the passes of the owner's tuned cleanup page (2026-10-18), in its order and with its rules:
 * plane offsets re-estimated from the scan, structural (wall/floor/ceiling) triangles classified with one-sided
 * bands, erase spheres, the walked volume, flat panels or snapping, furniture tops, clipping outside the outline,
 * small islands, thin floating pieces, Taubin smoothing. docs/mr/scan.md "Refinement" describes each pass. */
#include "refine.h"

#define WELD_MM 1000.0           /* vertices closer than 1 mm (quantized) are one vertex */
#define WELD_BIAS 16384          /* key offset, as the reference: +-16 m */
#define FURNITURE_PAD .1         /* m around a box that still counts as the box */
#define MIN_SHIFT_SAMPLES 50     /* plane offsets move only with more facing triangles than this */
#define WALKED_BELOW .2          /* m above the floor: the walked column starts here (feet, rugs stay) */
#define WALKED_ABOVE .25         /* m above the head: the column ends here */
#define TOP_NORMAL_Y .8          /* a triangle this upward-facing near a box top is part of the top */
#define TUBE_MIN_TRIANGLES 10
#define TUBE_MAX_AREA 1.0        /* m^2: bigger pieces are never thin floating pieces */
#define TUBE_MAX_FURNITURE .5    /* share of a piece inside furniture boxes that keeps it */
#define TAUBIN_LAMBDA .5
#define TAUBIN_MU -.53

double dfloor(double x) { i64 i=(i64)x; return x<(double)i ? (double)(i-1) : (double)i; }
double droot(double x) { return x>0 ? __builtin_sqrt(x) : 0; }
/* Double-precision sine (range reduction + Taylor series to x^19, error < 1e-15), as Math.sin. */
double dsin(double x)
{
    const double pi=3.14159265358979323846;
    x-=dfloor(x/(2*pi)+.5)*(2*pi);
    if(x>pi/2) x=pi-x;
    if(x<-pi/2) x=-pi-x;
    double q=x*x,term=x,sum=x;
    for(int k=1;k<10;k++) { term*=-q/((2*k)*(2*k+1));sum+=term; }
    return sum;
}
double dcos(double x) { return dsin(x+3.14159265358979323846/2); }
/* Math.hypot as V8 computes it (scaled, Kahan-compensated): distance ties at outline corners break the same way. */
double dhypot(double a, double b)
{
    a=a<0 ? -a : a;b=b<0 ? -b : b;
    double m=a>b ? a : b;
    if(m==0) return 0;
    double sum=0,comp=0,v[2]={a/m,b/m};
    for(int i=0;i<2;i++) { double s=v[i]*v[i]-comp,p=sum+s;comp=(p-sum)-s;sum=p; }
    return droot(sum)*m;
}
static double dab(double x) { return x<0 ? -x : x; }

static void *alloc(usize n) { return n ? io_alloc(n) : 0; }

/* ------------------------------------------------------------------------------------------------ welding */
static u64 mix(u64 k) { k^=k>>33;k*=0xff51afd7ed558ccdull;k^=k>>33;k*=0xc4ceb9fe1a85ec53ull;k^=k>>33;return k; }
static i64 quant(float v) { return (i64)dfloor((double)v*WELD_MM+.5)+WELD_BIAS; }
const char *prep_mesh(Prep *p, const Vec *v, const u32 *idx, int nv, int ni)
{
    *p=(Prep){0};
    int T=ni/3;
    usize cap=1024;while(cap<(usize)nv*2) cap*=2;
    i64 *keys=alloc(cap*sizeof(i64));
    int *slot=alloc(cap*sizeof(int)),*wid=alloc((usize)(nv ? nv : 1)*sizeof(int));
    double *sums=alloc((usize)(nv ? nv : 1)*4*sizeof(double));
    const char *error=0;
    if(!keys || !slot || !wid || !sums) { error="refine allocation failed";goto done; }
    int W=0;
    for(int i=0;i<nv;i++) {
        i64 key=(quant(v[i].x)*32768+quant(v[i].y))*32768+quant(v[i].z);
        usize h=(usize)mix((u64)key)&(cap-1);
        while(slot[h] && keys[h]!=key) h=(h+1)&(cap-1);
        if(!slot[h]) { keys[h]=key;slot[h]=++W; }
        int w=slot[h]-1;wid[i]=w;
        sums[4*w]+=v[i].x;sums[4*w+1]+=v[i].y;sums[4*w+2]+=v[i].z;sums[4*w+3]+=1;
    }
    p->T=T;p->W=W;
    p->pos=alloc((usize)(W ? W : 1)*3*sizeof(double));
    p->F=alloc((usize)(T ? T : 1)*3*sizeof(int));
    p->fn=alloc((usize)(T ? T : 1)*3*sizeof(float));
    p->cen=alloc((usize)(T ? T : 1)*3*sizeof(float));
    p->area=alloc((usize)(T ? T : 1)*sizeof(float));
    if(!p->pos || !p->F || !p->fn || !p->cen || !p->area) { error="refine allocation failed";goto done; }
    for(int w=0;w<W;w++) for(int k=0;k<3;k++) p->pos[3*w+k]=sums[4*w+k]/sums[4*w+3];
    for(int k=0;k<T*3;k++) p->F[k]=wid[idx[k]];
    for(int t=0;t<T;t++) {
        const double *a=p->pos+3*p->F[3*t],*b=p->pos+3*p->F[3*t+1],*c=p->pos+3*p->F[3*t+2];
        double ux=b[0]-a[0],uy=b[1]-a[1],uz=b[2]-a[2],wx=c[0]-a[0],wy=c[1]-a[1],wz=c[2]-a[2];
        double nx=uy*wz-uz*wy,ny=uz*wx-ux*wz,nz=ux*wy-uy*wx,l=droot(nx*nx+ny*ny+nz*nz);
        if(!(l>0)) l=1e-12;
        p->fn[3*t]=(float)(nx/l);p->fn[3*t+1]=(float)(ny/l);p->fn[3*t+2]=(float)(nz/l);p->area[t]=(float)(l/2);
        for(int k=0;k<3;k++) p->cen[3*t+k]=(float)((a[k]+b[k]+c[k])/3);
    }
done:
    io_free(keys,cap*sizeof(i64));io_free(slot,cap*sizeof(int));
    io_free(wid,(usize)(nv ? nv : 1)*sizeof(int));io_free(sums,(usize)(nv ? nv : 1)*4*sizeof(double));
    if(error) prep_free(p);
    return error;
}
void prep_free(Prep *p)
{
    int T=p->T ? p->T : 1,W=p->W ? p->W : 1;
    io_free(p->pos,(usize)W*3*sizeof(double));io_free(p->F,(usize)T*3*sizeof(int));
    io_free(p->fn,(usize)T*3*sizeof(float));io_free(p->cen,(usize)T*3*sizeof(float));io_free(p->area,(usize)T*sizeof(float));
    *p=(Prep){0};
}

/* ------------------------------------------------------------------------------------------------ helpers */
typedef struct {
    int kind, wall;       /* 0 wall (index into room->walls), 1 floor, 2 ceiling */
    double n[3], off, shift;
    double ox, oz, ux, uz, L;
} RPlane;
typedef struct { double cs, sn; const RVol *v; } Box;
static void box_local(const Box *b, double x, double z, double *lx, double *lz)
{
    double dx=x-b->v->cx,dz=z-b->v->cz;
    *lx=b->cs*dx-b->sn*dz;*lz=b->sn*dx+b->cs*dz;
}
static double s_dist(const RPlane *pl, const float *c) { return pl->n[0]*c[0]+pl->n[1]*c[1]+pl->n[2]*c[2]-pl->off; }
static int in_ext(const RPlane *pl, const float *c, double m)
{
    if(pl->kind) return 1;
    double s=(c[0]-pl->ox)*pl->ux+(c[2]-pl->oz)*pl->uz;
    return s>-m && s<pl->L+m;
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
/* Distance outside the outline (0 inside) and the extra allowance of the nearest wall (unseen walls). */
static double outside_info(const Room *r, double x, double z, double open_extra, double *allow)
{
    double best=1e300;*allow=0;
    for(int i=0;i<r->nwalls;i++) {
        const RWall *w=&r->walls[i];
        double abx=w->x1-w->x0,abz=w->z1-w->z0,l2=abx*abx+abz*abz;
        double t=l2>0 ? ((x-w->x0)*abx+(z-w->z0)*abz)/l2 : 0;
        t=t<0 ? 0 : t>1 ? 1 : t;
        double dx=x-(w->x0+t*abx),dz=z-(w->z0+t*abz),d=dhypot(dx,dz);
        if(d<best) { best=d;*allow=w->invisible ? open_extra : 0; }
    }
    return point_in_poly(x,z,r->outline,r->noutline) ? 0 : best;
}
static void nth(float *a, int n, int k)
{
    int lo=0,hi=n-1;
    while(lo<hi) {
        float pivot=a[(lo+hi)/2];int i=lo,j=hi;
        while(i<=j) {
            while(a[i]<pivot) i++;
            while(a[j]>pivot) j--;
            if(i<=j) { float t=a[i];a[i++]=a[j];a[j--]=t; }
        }
        if(k<=j) hi=j;else if(k>=i) lo=i;else break;
    }
}
static int find(int *parent, int x)
{
    while(parent[x]!=x) { parent[x]=parent[parent[x]];x=parent[x]; }
    return x;
}
static void unite(int *parent, const int *F, const u8 *keep, int T, int W)
{
    for(int i=0;i<W;i++) parent[i]=i;
    for(int t=0;t<T;t++) if(keep[t]) {
        int r=find(parent,F[3*t]);
        parent[find(parent,F[3*t+1])]=r;parent[find(parent,F[3*t+2])]=r;
    }
}
/* Eigenvalues of a symmetric 3x3 matrix by Jacobi rotations, as the reference's eigSym3. */
static void eig3(double a[3][3], double out[3])
{
    static const int pairs[3][2]={{0,1},{0,2},{1,2}};
    for(int sweep=0;sweep<20;sweep++) {
        double off=dab(a[0][1])+dab(a[0][2])+dab(a[1][2]);
        if(off<1e-14) break;
        for(int k=0;k<3;k++) {
            int p=pairs[k][0],q=pairs[k][1];
            if(dab(a[p][q])<1e-18) continue;
            double th=(a[q][q]-a[p][p])/(2*a[p][q]);
            double t=(th<0 ? -1 : 1)/(dab(th)+droot(th*th+1)),c=1/droot(t*t+1),s=t*c;
            for(int m=0;m<3;m++) { double akp=a[m][p],akq=a[m][q];a[m][p]=c*akp-s*akq;a[m][q]=s*akp+c*akq; }
            for(int m=0;m<3;m++) { double apk=a[p][m],aqk=a[q][m];a[p][m]=c*apk-s*aqk;a[q][m]=s*apk+c*aqk; }
        }
    }
    out[0]=a[0][0];out[1]=a[1][1];out[2]=a[2][2];
}
/* Ear clipping of the (positively wound) outline, exactly the reference's order. */
static int ear_clip(const DPt *poly, int n, int tris[][3])
{
    int idx[MAX_OUTLINE],m=n,count=0,guard=0;
    for(int i=0;i<n;i++) idx[i]=i;
#define A2(a,b,c) ((poly[b].x-poly[a].x)*(poly[c].z-poly[a].z)-(poly[b].z-poly[a].z)*(poly[c].x-poly[a].x))
    while(m>3 && guard++<10000) {
        int cut=0;
        for(int k=0;k<m;k++) {
            int i0=idx[(k-1+m)%m],i1=idx[k],i2=idx[(k+1)%m];
            if(A2(i0,i1,i2)<=0) continue;
            int blocked=0;
            for(int q=0;q<m && !blocked;q++) {
                int j=idx[q];
                if(j!=i0 && j!=i1 && j!=i2 && A2(i0,i1,j)>0 && A2(i1,i2,j)>0 && A2(i2,i0,j)>0) blocked=1;
            }
            if(blocked) continue;
            tris[count][0]=i0;tris[count][1]=i1;tris[count][2]=i2;count++;
            for(int q=k;q+1<m;q++) idx[q]=idx[q+1];
            m--;cut=1;break;
        }
        if(!cut) break;
    }
#undef A2
    if(m==3) { tris[count][0]=idx[0];tris[count][1]=idx[1];tris[count][2]=idx[2];count++; }
    return count;
}

/* ------------------------------------------------------------------------------------------------ passes */
typedef struct {
    u8 *furn, *keep;
    signed char *owner;
    int *parent, *start, *nb, *fill, *comp, *stamp;
    double *pos, *tmp, *acc;
    float *ds;
    int nb_n, comp_n;
} Work;
static void work_free(Work *w, int T, int W)
{
    usize t=(usize)(T ? T : 1),v=(usize)(W ? W : 1);
    io_free(w->furn,t);io_free(w->keep,t);io_free(w->owner,t);io_free(w->parent,v*sizeof(int));
    io_free(w->start,(v+1)*sizeof(int));io_free(w->nb,(usize)(w->nb_n ? w->nb_n : 1)*sizeof(int));
    io_free(w->fill,v*sizeof(int));io_free(w->comp,v*sizeof(int));io_free(w->stamp,v*sizeof(int));
    io_free(w->pos,v*3*sizeof(double));io_free(w->tmp,v*3*sizeof(double));
    io_free(w->acc,(usize)(w->comp_n ? w->comp_n : 1)*12*sizeof(double));io_free(w->ds,t*sizeof(float));
}

/* Heads binned on a horizontal grid with cells of at least the walked radius: the 3 x 3 cells around a point hold
 * every head within the radius, so this gives the same result as testing every head, with far fewer tests. */
typedef struct { double x0, z0, cell; int nx, nz; int *start, *list; } HeadGrid;
static int head_cell(const HeadGrid *g, double v, double v0) { return (int)dfloor((v-v0)/g->cell); }
static int head_grid(HeadGrid *g, const Vec *heads, int n, double radius)
{
    double x0=1e300,x1=-1e300,z0=1e300,z1=-1e300;
    for(int i=0;i<n;i++) { x0=heads[i].x<x0 ? heads[i].x : x0;x1=heads[i].x>x1 ? heads[i].x : x1;
                           z0=heads[i].z<z0 ? heads[i].z : z0;z1=heads[i].z>z1 ? heads[i].z : z1; }
    g->cell=radius;
    if((x1-x0)/256>g->cell) g->cell=(x1-x0)/256;
    if((z1-z0)/256>g->cell) g->cell=(z1-z0)/256;
    g->x0=x0;g->z0=z0;g->nx=head_cell(g,x1,x0)+1;g->nz=head_cell(g,z1,z0)+1;
    g->start=alloc((usize)(g->nx*g->nz+1)*sizeof(int));g->list=alloc((usize)n*sizeof(int));
    int *fill=alloc((usize)(g->nx*g->nz)*sizeof(int));
    if(!g->start || !g->list || !fill) { io_free(fill,(usize)(g->nx*g->nz)*sizeof(int));return 0; }
    for(int i=0;i<n;i++) g->start[head_cell(g,heads[i].z,z0)*g->nx+head_cell(g,heads[i].x,x0)+1]++;
    for(int c=0;c<g->nx*g->nz;c++) g->start[c+1]+=g->start[c];
    for(int i=0;i<n;i++) {
        int c=head_cell(g,heads[i].z,z0)*g->nx+head_cell(g,heads[i].x,x0);
        g->list[g->start[c]+fill[c]++]=i;
    }
    io_free(fill,(usize)(g->nx*g->nz)*sizeof(int));
    return 1;
}
static void head_grid_free(HeadGrid *g, int n)
{
    io_free(g->start,(usize)(g->nx*g->nz+1)*sizeof(int));io_free(g->list,(usize)n*sizeof(int));
}
static int walked_hit(const HeadGrid *g, const Vec *heads, double x, double y, double z, double r2)
{
    int cx=head_cell(g,x,g->x0),cz=head_cell(g,z,g->z0);
    for(int v=cz-1;v<=cz+1;v++) for(int u=cx-1;u<=cx+1;u++) {
        if(u<0 || v<0 || u>=g->nx || v>=g->nz) continue;
        for(int k=g->start[v*g->nx+u];k<g->start[v*g->nx+u+1];k++) {
            const Vec *h=&heads[g->list[k]];
            if(y<h->y+WALKED_ABOVE && (x-h->x)*(x-h->x)+(z-h->z)*(z-h->z)<=r2) return 1;
        }
    }
    return 0;
}

const char *refine_run(const Refine *o, const Room *room, const Prep *P, const RVol *vols, int nvols,
                       const Vec *heads, int nheads, Vec **out_v, u32 **out_i, int *out_n, Report *rep)
{
    int T=P->T,W=P->W;
    const float *cen=P->cen,*fn=P->fn,*area=P->area;
    const int *F=P->F;
    Work w={0};
    const char *error=0;
    *out_v=0;*out_i=0;*out_n=0;
    memset(rep,0,sizeof(*rep));rep->triangles=T;
    usize tb=(usize)(T ? T : 1),wb=(usize)(W ? W : 1);
    w.furn=alloc(tb);w.keep=alloc(tb);w.owner=alloc(tb);w.ds=alloc(tb*sizeof(float));
    w.parent=alloc(wb*sizeof(int));w.pos=alloc(wb*3*sizeof(double));w.stamp=alloc(wb*sizeof(int));
    if(!w.furn || !w.keep || !w.owner || !w.ds || !w.parent || !w.pos || !w.stamp) { error="refine allocation failed";goto done; }
    memcpy(w.pos,P->pos,(usize)W*3*sizeof(double));
    double *pos=w.pos;

    /* furniture flags: the ruled boxes (+10 cm) */
    Box boxes[MAX_VOLUMES];
    for(int i=0;i<nvols;i++) {
        double a=vols[i].yaw*3.14159265358979323846/180;
        boxes[i]=(Box){dcos(a),dsin(a),&vols[i]};
    }
    for(int i=0;i<nvols;i++) {
        const RVol *v=&vols[i];
        for(int t=0;t<T;t++) {
            double lx,lz,y=cen[3*t+1];
            box_local(&boxes[i],cen[3*t],cen[3*t+2],&lx,&lz);
            if(dab(lx)<=v->w/2.+FURNITURE_PAD && dab(lz)<=v->d/2.+FURNITURE_PAD &&
               y<=v->top+FURNITURE_PAD && y>=v->top-v->h-FURNITURE_PAD) w.furn[t]=1;
        }
    }

    /* 1. planes: seen walls, floor, ceiling; offsets re-estimated (median of facing triangles within the band) */
    RPlane planes[MAX_OUTLINE+2];int np=0;
    for(int i=0;i<room->nwalls;i++) {
        const RWall *wl=&room->walls[i];
        if(wl->invisible) continue;
        double dx=wl->x1-wl->x0,dz=wl->z1-wl->z0,L=dhypot(dx,dz);
        if(!(L>0)) continue;
        RPlane *pl=&planes[np++];
        *pl=(RPlane){0,i,{-dz/L,0,dx/L},0,0,wl->x0,wl->z0,dx/L,dz/L,L};
        pl->off=pl->n[0]*wl->x0+pl->n[2]*wl->z0;
    }
    planes[np++]=(RPlane){1,-1,{0,1,0},room->floor,0,0,0,0,0,0};
    planes[np++]=(RPlane){2,-1,{0,-1,0},-(double)room->ceiling,0,0,0,0,0,0};
    double cos_lim=dcos(o->facing_deg*3.14159265358979323846/180);
#define FACING(pl,t) ((pl)->n[0]*fn[3*(t)]+(pl)->n[1]*fn[3*(t)+1]+(pl)->n[2]*fn[3*(t)+2]>cos_lim)
    for(int k=0;k<np;k++) {
        RPlane *pl=&planes[k];int n=0;
        for(int t=0;t<T;t++) {
            if(w.furn[t]) continue;
            double d=s_dist(pl,cen+3*t);
            if(dab(d)<o->band && FACING(pl,t) && in_ext(pl,cen+3*t,o->band)) w.ds[n++]=(float)d;
        }
        if(n>MIN_SHIFT_SAMPLES) { nth(w.ds,n,n>>1);pl->shift=w.ds[n>>1]; }
        pl->off+=pl->shift;
        rep->shifts[rep->nshifts++]=(Shift){pl->kind,pl->wall,(float)pl->shift};
    }
    double floor_y=planes[np-2].off,ceil_y=-planes[np-1].off;
    rep->floor_y=(float)floor_y;rep->ceiling_y=(float)ceil_y;

    /* 2. structural triangles: behind a surface = band; in front = band_in (walls; the full band in the wall's top
     * strip), floor_in, ceil_in */
    for(int t=0;t<T;t++) w.owner[t]=-1;
    if(o->walls) for(int t=0;t<T;t++) {
        if(w.furn[t]) continue;
        double best=1e300;
        for(int k=0;k<np;k++) {
            const RPlane *pl=&planes[k];double d=s_dist(pl,cen+3*t);
            double front=!pl->kind ? (cen[3*t+1]>ceil_y-o->top_strip ? o->band : o->band_in) :
                         pl->kind==2 ? o->ceil_in : o->floor_in;
            if(d>-o->band && d<front && dab(d)<best && FACING(pl,t) && in_ext(pl,cen+3*t,o->band)) {
                best=dab(d);w.owner[t]=(signed char)k;
            }
        }
    }
#undef FACING
    for(int t=0;t<T;t++) w.keep[t]=1;
    /* erase spheres */
    for(int e=0;e<o->nerase;e++) {
        const Sphere *s=&o->erase[e];
        for(int t=0;t<T;t++) {
            double dx=cen[3*t]-s->x,dy=cen[3*t+1]-s->y,dz=cen[3*t+2]-s->z;
            if(w.keep[t] && dx*dx+dy*dy+dz*dz<=(double)s->r*s->r) { w.keep[t]=0;rep->erased++; }
        }
    }
    /* the walked volume: a column under each head position, never inside furniture */
    if(o->walked>0 && nheads) {
        HeadGrid g={0};
        if(!head_grid(&g,heads,nheads,o->walked)) { head_grid_free(&g,nheads);error="refine allocation failed";goto done; }
        double r2=(double)o->walked*o->walked;
        for(int t=0;t<T;t++) {
            if(!w.keep[t] || w.furn[t]) continue;
            double y=cen[3*t+1];
            if(y<room->floor+WALKED_BELOW) continue;
            if(walked_hit(&g,heads,cen[3*t],y,cen[3*t+2],r2)) { w.keep[t]=0;rep->walked++; }
        }
        head_grid_free(&g,nheads);
    } else if(o->walked>0) rep->walked_skipped=1;
    for(int t=0;t<T;t++) if(w.owner[t]>=0) rep->structural++;
    if(o->walls==2) {
        for(int t=0;t<T;t++) if(w.owner[t]>=0) w.keep[t]=0;
        if(o->near>0) for(int t=0;t<T;t++) {
            if(!w.keep[t] || w.furn[t]) continue;
            for(int k=0;k<np;k++) if(dab(s_dist(&planes[k],cen+3*t))<o->near && in_ext(&planes[k],cen+3*t,o->near)) {
                w.keep[t]=0;rep->near++;break;
            }
        }
    } else if(o->walls==1) {
        /* snap: a vertex goes onto the plane of the last structural triangle that uses it */
        int *vown=w.stamp;
        for(int i=0;i<W;i++) vown[i]=-1;
        for(int t=0;t<T;t++) if(w.owner[t]>=0) for(int j=0;j<3;j++) vown[F[3*t+j]]=w.owner[t];
        for(int i=0;i<W;i++) {
            if(vown[i]<0) continue;
            const RPlane *pl=&planes[vown[i]];
            double d=pl->n[0]*pos[3*i]+pl->n[1]*pos[3*i+1]+pl->n[2]*pos[3*i+2]-pl->off;
            for(int k=0;k<3;k++) pos[3*i+k]-=d*pl->n[k];
        }
    }
    /* 2b. furniture tops: upward scan near a box's top, inside its footprint, goes onto the top */
    if(o->tops>0) {
        for(int i=0;i<W;i++) w.stamp[i]=0;
        for(int i=0;i<nvols;i++) {
            const RVol *v=&vols[i];int mark=i+1;
            for(int t=0;t<T;t++) {
                if(!w.keep[t]) continue;
                const double *a=pos+3*F[3*t],*b=pos+3*F[3*t+1],*c=pos+3*F[3*t+2];
                double cy=(a[1]+b[1]+c[1])/3;
                if(dab(cy-v->top)>=o->tops) continue;
                double lx,lz;
                box_local(&boxes[i],(a[0]+b[0]+c[0])/3,(a[2]+b[2]+c[2])/3,&lx,&lz);
                if(dab(lx)>v->w/2. || dab(lz)>v->d/2.) continue;
                double ux=b[0]-a[0],uy=b[1]-a[1],uz=b[2]-a[2],wx=c[0]-a[0],wy=c[1]-a[1],wz=c[2]-a[2];
                double nx=uy*wz-uz*wy,ny=uz*wx-ux*wz,nz=ux*wy-uy*wx,l=droot(nx*nx+ny*ny+nz*nz);
                if(ny/(l>0 ? l : 1)<=TOP_NORMAL_Y) continue;
                for(int j=0;j<3;j++) w.stamp[F[3*t+j]]=mark;
                rep->tops++;
            }
            for(int k=0;k<W;k++) if(w.stamp[k]==mark) pos[3*k+1]=v->top;
        }
    }
    /* 3. clip outside the room (unseen walls allow clip_open more) and above/below the planes */
    if(o->clip<1 || o->clip_open<2) for(int t=0;t<T;t++) {
        if(!w.keep[t]) continue;
        double allow,out=outside_info(room,cen[3*t],cen[3*t+2],o->clip_open,&allow),y=cen[3*t+1];
        if(out>o->clip+allow || y>ceil_y+o->clip || y<floor_y-o->clip) { w.keep[t]=0;rep->outside++; }
    }
    /* 4. islands: connected pieces below the minimum area */
    if(o->island>0) {
        double *comp=w.tmp=alloc(wb*3*sizeof(double));
        if(!comp) { error="refine allocation failed";goto done; }
        unite(w.parent,F,w.keep,T,W);
        for(int t=0;t<T;t++) if(w.keep[t]) comp[find(w.parent,F[3*t])]+=area[t];
        for(int t=0;t<T;t++)
            if(w.keep[t] && comp[find(w.parent,F[3*t])]<o->island && !(o->protect && w.furn[t])) { w.keep[t]=0;rep->islands++; }
        io_free(w.tmp,wb*3*sizeof(double));w.tmp=0;
    }
    /* 4b. thin floating pieces: elongated (PCA), thin, small, not furniture */
    if(o->tubes>0) {
        unite(w.parent,F,w.keep,T,W);
        w.comp=alloc(wb*sizeof(int));
        if(!w.comp) { error="refine allocation failed";goto done; }
        for(int i=0;i<W;i++) w.comp[i]=-1;
        int nc=0;
        for(int t=0;t<T;t++) if(w.keep[t]) { int r=find(w.parent,F[3*t]);if(w.comp[r]<0) w.comp[r]=nc++; }
        w.comp_n=nc;
        w.acc=alloc((usize)(nc ? nc : 1)*12*sizeof(double));
        if(!w.acc) { error="refine allocation failed";goto done; }
        for(int t=0;t<T;t++) {
            if(!w.keep[t]) continue;
            double *a=w.acc+12*w.comp[find(w.parent,F[3*t])];
            for(int j=0;j<3;j++) {
                const double *q=pos+3*F[3*t+j];double x=q[0],y=q[1],z=q[2];
                a[0]++;a[1]+=x;a[2]+=y;a[3]+=z;a[4]+=x*x;a[5]+=x*y;a[6]+=x*z;a[7]+=y*y;a[8]+=y*z;a[9]+=z*z;
            }
            a[10]+=area[t];a[11]+=w.furn[t] ? 1 : 0;
        }
        u8 *drop=(u8 *)w.stamp;  /* nc <= T bytes fit in the W ints */
        for(int c=0;c<nc;c++) {
            double *a=w.acc+12*c,n=a[0],tris=n/3;drop[c]=0;
            if(tris<TUBE_MIN_TRIANGLES || a[11]/tris>TUBE_MAX_FURNITURE || a[10]>=TUBE_MAX_AREA) continue;
            double mx_=a[1]/n,my=a[2]/n,mz=a[3]/n;
            double C[3][3]={{a[4]/n-mx_*mx_,a[5]/n-mx_*my,a[6]/n-mx_*mz},{0,a[7]/n-my*my,a[8]/n-my*mz},{0,0,a[9]/n-mz*mz}};
            C[1][0]=C[0][1];C[2][0]=C[0][2];C[2][1]=C[1][2];
            double ev[3];eig3(C,ev);
            for(int k=0;k<3;k++) ev[k]=droot(ev[k]>1e-12 ? ev[k] : 1e-12);
            for(int i=0;i<2;i++) for(int j=0;j<2-i;j++) if(ev[j]>ev[j+1]) { double s=ev[j];ev[j]=ev[j+1];ev[j+1]=s; }
            if(ev[2]/(ev[1]>1e-6 ? ev[1] : 1e-6)>=o->tubes && ev[1]<=o->tube_thick) drop[c]=1;
        }
        for(int t=0;t<T;t++) if(w.keep[t] && drop[w.comp[find(w.parent,F[3*t])]]) { w.keep[t]=0;rep->tubes++; }
    }
    /* 5. Taubin smoothing over the kept triangles (lambda/mu pairs keep the volume) */
    if(o->smooth>0) {
        w.start=alloc((wb+1)*sizeof(int));w.fill=alloc(wb*sizeof(int));w.tmp=alloc(wb*3*sizeof(double));
        if(!w.start || !w.fill || !w.tmp) { error="refine allocation failed";goto done; }
        for(int t=0;t<T;t++) if(w.keep[t]) for(int j=0;j<3;j++) w.start[F[3*t+j]+1]+=2;
        for(int i=0;i<W;i++) w.start[i+1]+=w.start[i];
        w.nb_n=w.start[W];
        w.nb=alloc((usize)(w.nb_n ? w.nb_n : 1)*sizeof(int));
        if(!w.nb) { error="refine allocation failed";goto done; }
        for(int i=0;i<W;i++) w.fill[i]=w.start[i];
        for(int t=0;t<T;t++) if(w.keep[t]) for(int j=0;j<3;j++) {
            int a=F[3*t+j],b=F[3*t+(j+1)%3],c=F[3*t+(j+2)%3];
            w.nb[w.fill[a]++]=b;w.nb[w.fill[a]++]=c;
        }
        for(int it=0;it<o->smooth;it++) for(int pass=0;pass<2;pass++) {
            double f=pass ? TAUBIN_MU : TAUBIN_LAMBDA;
            for(int i=0;i<W;i++) {
                int s0=w.start[i],e=w.start[i+1];
                if(s0==e) { for(int k=0;k<3;k++) w.tmp[3*i+k]=pos[3*i+k];continue; }
                double x=0,y=0,z=0;
                for(int k=s0;k<e;k++) { const double *q=pos+3*w.nb[k];x+=q[0];y+=q[1];z+=q[2]; }
                int n=e-s0;
                w.tmp[3*i]=pos[3*i]+f*(x/n-pos[3*i]);w.tmp[3*i+1]=pos[3*i+1]+f*(y/n-pos[3*i+1]);
                w.tmp[3*i+2]=pos[3*i+2]+f*(z/n-pos[3*i+2]);
            }
            memcpy(pos,w.tmp,(usize)W*3*sizeof(double));
        }
    }
    /* 6. output: the kept scan, then flat panels for seen walls, the floor and the ceiling */
    for(int t=0;t<T;t++) rep->kept+=w.keep[t];
    Vec panel[(MAX_OUTLINE*2+MAX_OUTLINE*2)*3];int npanel=0;
    if(o->walls==2) {
        for(int k=0;k<np;k++) {
            const RPlane *pl=&planes[k];
            if(pl->kind) continue;
            const RWall *wl=&room->walls[pl->wall];double sh=pl->shift;
            double a0x=wl->x0+pl->n[0]*sh,a0z=wl->z0+pl->n[2]*sh,a1x=wl->x1+pl->n[0]*sh,a1z=wl->z1+pl->n[2]*sh;
            Vec q[4]={vec((float)a0x,(float)floor_y,(float)a0z),vec((float)a1x,(float)floor_y,(float)a1z),
                      vec((float)a1x,(float)ceil_y,(float)a1z),vec((float)a0x,(float)ceil_y,(float)a0z)};
            double ux=a1x-a0x,uz=a1z-a0z,vy=ceil_y-floor_y;
            int ok=(-uz*vy)*pl->n[0]+(ux*vy)*pl->n[2]>0;
            int order[2][6]={{0,1,2,0,2,3},{1,0,3,1,3,2}};
            for(int j=0;j<6;j++) panel[npanel*3+j]=q[order[!ok][j]];
            npanel+=2;
        }
        int ears[MAX_OUTLINE][3];
        int ne=ear_clip(room->outline,room->noutline,ears);
        for(int e=0;e<ne;e++) {
            DPt A=room->outline[ears[e][0]],B=room->outline[ears[e][1]],C=room->outline[ears[e][2]];
            panel[npanel*3]=vec((float)A.x,(float)floor_y,(float)A.z);panel[npanel*3+1]=vec((float)C.x,(float)floor_y,(float)C.z);
            panel[npanel*3+2]=vec((float)B.x,(float)floor_y,(float)B.z);npanel++;
            panel[npanel*3]=vec((float)A.x,(float)ceil_y,(float)A.z);panel[npanel*3+1]=vec((float)B.x,(float)ceil_y,(float)B.z);
            panel[npanel*3+2]=vec((float)C.x,(float)ceil_y,(float)C.z);npanel++;
        }
    }
    rep->panels=npanel;
    int room_left=MAX_TRIANGLES-npanel,kept_out=rep->kept<room_left ? rep->kept : room_left;
    rep->truncated=rep->kept-kept_out;
    *out_v=io_alloc((usize)MAX_MESH_VERTICES*sizeof(Vec));
    *out_i=io_alloc((usize)MAX_TRIANGLES*3*sizeof(u32));
    if(!*out_v || !*out_i) { error="refine allocation failed";goto done; }
    int o_n=0;
    for(int t=0;t<T && o_n<kept_out*3;t++) {
        if(!w.keep[t]) continue;
        for(int j=0;j<3;j++) {
            const double *q=pos+3*F[3*t+j];
            (*out_v)[o_n]=vec((float)q[0],(float)q[1],(float)q[2]);(*out_i)[o_n]=(u32)o_n;o_n++;
        }
    }
    for(int k=0;k<npanel*3;k++) { (*out_v)[o_n]=panel[k];(*out_i)[o_n]=(u32)o_n;o_n++; }
    *out_n=o_n;
done:
    if(error) {
        io_free(*out_v,(usize)MAX_MESH_VERTICES*sizeof(Vec));io_free(*out_i,(usize)MAX_TRIANGLES*3*sizeof(u32));
        *out_v=0;*out_i=0;*out_n=0;
    }
    work_free(&w,T,W);
    return error;
}
