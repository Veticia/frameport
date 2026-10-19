/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FP_ROOMSCAN_REFINE_H
#define FP_ROOMSCAN_REFINE_H
#include "scan.h"

/* Room cleanup after the mesh (docs/mr/scan.md "Refinement"): the scanner's unrefined output is kept as
 * room-raw.txt + room-mesh-raw.bin, the refined one replaces room.txt + room-mesh.bin, scan.json holds the head
 * path, the settings used, the per-pass report, the label-rule decisions and door/closet suggestions. */

#define MAX_EXTRA_TEXT 8192
#define MAX_LABEL 24
#define MAX_REASON 240
#define MAX_RAW_FILE (16u+(usize)MAX_MESH_VERTICES*12u+(usize)MAX_TRIANGLES*12u)
#define MAX_JSON_FILE (1u<<20)

/* Growable text buffer (io_alloc backed). ok=0 after an allocation failure or the cap. */
typedef struct { char *data; usize n, cap, limit; int ok; } Buf;
void buf_init(Buf *b, usize limit);
void buf_free(Buf *b);
void put(Buf *b, const char *s);
void put_n(Buf *b, const char *s, usize n);
void put_int(Buf *b, i64 v);
void put_u64(Buf *b, u64 v);
void put_dec(Buf *b, float v);                 /* 4 decimals, the room.txt number format */
void put_fixed(Buf *b, double v, int places);  /* JavaScript's toFixed */
void put_round(Buf *b, double v);              /* JavaScript's Math.round */

typedef struct { double x, z; } DPt;
/* Room numbers are doubles parsed exactly from their decimal text, as the reference page reads them. */
typedef struct { double x0, z0, x1, z1, y0, y1; int invisible; char id[33]; } RWall;
typedef struct { double cx, cz, top, yaw, w, d, h; char label[MAX_LABEL]; char id[33]; } RVol;
typedef struct {
    u64 universe, time;
    char source[16];
    char room_id[33], floor_id[33], ceiling_id[33], mesh_id[33];
    double floor, ceiling;
    DPt outline[MAX_OUTLINE];
    int noutline;
    RWall walls[MAX_OUTLINE];
    int nwalls;
    RVol vols[MAX_VOLUMES];
    int nvols;
    char mesh_name[64];
    u32 mesh_nv, mesh_ni;
    int has_mesh;
    char extra[MAX_EXTRA_TEXT];  /* records this scanner does not model (plane, ...), kept verbatim */
    usize nextra;
} Room;

/* The mesh with its vertices welded at 1 mm (as the reference page does), per-triangle normals/centroids/areas. */
typedef struct {
    int T, W;
    double *pos;
    int *F;
    float *fn, *cen, *area;
} Prep;

typedef struct { int kind, wall; float shift; } Shift;   /* kind: 0 wall, 1 floor, 2 ceiling */
typedef struct {
    int triangles, kept, structural, near, outside, islands, tubes, erased, walked, tops, panels, truncated;
    int walked_skipped;
    float floor_y, ceiling_y;
    Shift shifts[MAX_OUTLINE+2];
    int nshifts;
} Report;
typedef struct {
    char from[MAX_LABEL], to[MAX_LABEL], suggest[8], reason[MAX_REASON];
    RVol box;
    int wall;
    float along;
} Label;

/* roomio.c */
const char *room_from_scan(Scan *s, Room *r);
const char *room_parse(const char *text, usize n, Room *r);
int room_text(const Room *r, Buf *b);
const char *read_file(const char *path, usize limit, u8 **data, usize *size);
const char *publish(const char *dir, const char *name, const void *data, usize size);
int join_path(char *out, const char *dir, const char *name);
const char *mesh_parse(const u8 *data, usize size, Vec **vertices, u32 **indices, int *nv, int *ni);
const char *mesh_bytes(const Vec *vertices, const u32 *indices, int nv, int ni, u8 **data, usize *size);
void heads_json(Buf *b, const Vec *heads, int n);
int heads_parse(const char *text, usize n, Vec *heads);
void settings_json(Buf *b, const Refine *o);
void report_json(Buf *b, const Report *r, int nlabels, int changed, int removed, int nsuggest);

/* refine.c */
const char *prep_mesh(Prep *p, const Vec *vertices, const u32 *indices, int nv, int ni);
void prep_free(Prep *p);
const char *refine_run(const Refine *o, const Room *room, const Prep *p, const RVol *vols, int nvols,
                       const Vec *heads, int nheads, Vec **out_v, u32 **out_i, int *out_n, Report *rep);
double dfloor(double x);
double droot(double x);
double dsin(double x);
double dcos(double x);
double dhypot(double a, double b);

/* rules.c */
int rules_apply(const Room *room, const Prep *p, RVol *out, int *nout, Label *labels);

#endif
