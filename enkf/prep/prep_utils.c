/******************************************************************************
 *
 * File:        prep_utils.c        
 *
 * Created:     12/2012
 *
 * Author:      Pavel Sakov
 *              Bureau of Meteorology
 *
 * Description:
 *
 * Revisions:
 *
 *****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <assert.h>
#include "stringtable.h"
#include "ncw.h"
#include "kdtree.h"
#include "definitions.h"
#include "utils.h"
#include "obsprm.h"
#include "grid.h"
#include "model.h"
#include "observations.h"
#include "allreaders.h"
#include "prep_utils.h"

#define DT_EPS 1.0e-6

/**
 */
static int obs_badob(observations* obs, int i)
{
    observation* o = &obs->data[i];

    if (o->status != STATUS_OK)
        return 0;
    if (o->type < 0 || o->product < 0 || o->instrument < 0 || o->fid < 0 || o->batch < 0 || !isfinite(o->value) || fabs(o->value) > MAXOBSVAL || isnan(o->std) || o->std <= 0.0 || !isfinite(o->fi) || !isfinite(o->fj) || !isfinite(o->fk) || !isfinite(o->lon) || !isfinite(o->lat) || !isfinite(o->depth))
        return 1;
    return 0;
}

/**
 */
static void readobs(obsmeta* meta, model* m, obsread_fn reader, observations* obs)
{
    int otid = obstype_getid(obs->nobstypes, obs->obstypes, meta->type, 1);
    obstype* ot = &obs->obstypes[otid];
    grid* g = model_getgridbyid(m, ot->gridid);
    int nfiles;
    char** fnames = NULL;
    int i;

    nfiles = 0;
    get_obsfiles(meta, &nfiles, &fnames);
    for (i = 0; i < nfiles; ++i) {
        int nobs0 = obs->nobs;
        int fid;

        enkf_printf("      reading %s:\n", fnames[i]);
        fid = st_add_ifabsent(obs->datafiles, fnames[i], -1);
        reader(fnames[i], fid, meta, g, obs);

        enkf_printf("        # good obs = %d\n", obs->nobs - nobs0);
        enkf_flush();
        free(fnames[i]);
    }
    free(fnames);
}

/** Adds observations from a certain provider.
 *  This procedure is put in prep_utils.c because of its dependence on
 *  `obsmeta'.
 */
void obs_add(observations* obs, model* m, obsmeta* meta)
{
    int nobs0 = obs->nobs;
    int otid = obstype_getid(obs->nobstypes, obs->obstypes, meta->type, 1);
    obstype* ot = &obs->obstypes[otid];

    grid* g = model_getgridbyid(m, ot->gridid);
    float** depth = grid_getdepth(g);
    int** numlevels = grid_getnumlevels(g);
    int isperiodic_i = grid_isperiodic_i(g);
    double lonbase = grid_getlonbase(g);

    double mindepth = NAN;
    obsread_fn reader;
    int i, ngood;

    enkf_printf("    PRODUCT = %s, TYPE = %s, reader = %s\n", meta->product, meta->type, meta->reader);
    st_add_ifabsent(obs->products, meta->product, -1);
    reader = get_obsreadfn(meta);
    readobs(meta, m, reader, obs);      /* adds the data */

    for (i = 0; i < meta->npars; ++i) {
        if (strcasecmp(meta->pars[i].name, "MINDEPTH") == 0)
            if (!str2double(meta->pars[i].value, &mindepth))
                enkf_quit("observation prm file: can not convert MINDEPTH = \"%s\" to double\n", meta->pars[i].value);
    }

    if (!isnan(lonbase)) {
        for (i = nobs0; i < obs->nobs; ++i) {
            observation* o = &obs->data[i];

            o->id_orig = i;
            if (o->lon < lonbase)
                o->lon += 360.0;
            else if (o->lon >= lonbase + 360.0)
                o->lon -= 360.0;
        }
    }

    /*
     * common checks
     */
    if (obs->nobs - nobs0 > 0) {
        int nmin = 0;
        int nmax = 0;
        int noutow = 0;
        int nland = 0;
        int nshallow = 0;
        int ni, nj;

        enkf_printf("      id = %d - %d\n", nobs0, obs->nobs - 1);
        grid_getdims(g, &ni, &nj, NULL);
        for (i = nobs0; i < obs->nobs; ++i) {
            observation* o = &obs->data[i];

            if (o->status != STATUS_OK)
                continue;
            if (o->date - obs->da_date < ot->windowmin + DT_EPS) {
                o->status = STATUS_OUTSIDEOBSWINDOW;
                noutow++;
                continue;
            }
            if (o->date - obs->da_date > ot->windowmax - DT_EPS) {
                o->status = STATUS_OUTSIDEOBSWINDOW;
                noutow++;
                continue;
            }
            if (o->value < ot->allowed_min) {
                o->status = STATUS_RANGE;
                nmin++;
                continue;
            }
            if (o->value > ot->allowed_max) {
                o->status = STATUS_RANGE;
                nmax++;
                continue;
            }
            if (depth != NULL) {
                o->model_depth = (double) interpolate2d(o->fi, o->fj, ni, nj, depth, numlevels, isperiodic_i);
                if (!isfinite(o->model_depth) || o->model_depth == 0) {
                    o->status = STATUS_LAND;
                    nland++;
                    continue;
                }
            } else if (island(o->fi, o->fj, ni, nj, numlevels, isperiodic_i)) {
                o->status = STATUS_LAND;
                nland++;
                continue;
            }
            if (isfinite(mindepth)) {
                if (depth == NULL)
                    enkf_quit("MINDEPTH specified for the obs reader, but no depth specified for grid \"%s\"", grid_getname(g));
                if (o->model_depth < mindepth) {
                    o->status = STATUS_SHALLOW;
                    nshallow++;
                }
            }
        }
        if (noutow > 0)
            enkf_printf("        %d observations outside obs. window\n", noutow);
        if (nmin > 0)
            enkf_printf("        %d observations below allowed minimum of %.4g\n", nmin, ot->allowed_min);
        if (nmax > 0)
            enkf_printf("        %d observations above allowed maximum of %.4g\n", nmax, ot->allowed_max);
        if (nland > 0)
            enkf_printf("        %d observations on land\n", nland);
        if (nshallow > 0)
            enkf_printf("        %d observations in shallow areas\n", nshallow);
    }
    obs->compacted = 0;
    obs->hasstats = 0;
    enkf_printf("      total %d observations\n", obs->nobs - nobs0);
    for (ngood = 0, i = nobs0; i < obs->nobs; ++i)
        if (obs->data[i].status == STATUS_OK)
            ngood++;
    enkf_printf("      %d valid observations\n", ngood);

    /*
     * add specified errors 
     */
    if (obs->nobs - nobs0 > 0 && meta->nstds > 0) {
        int i, o;

        for (i = 0; i < meta->nstds; ++i) {
            metastd* std = &meta->stds[i];

            if (std->type == STDTYPE_VALUE) {
                double v = ((double*) std->data)[0];

                enkf_printf("      adding error_std = %.3g:\n", v);
                for (o = nobs0; o < obs->nobs; ++o) {
                    observation* oo = &obs->data[o];

                    if (oo->status != STATUS_OK)
                        continue;
                    if (std->op == ARITHMETIC_EQ)
                        oo->std = v;
                    else if (std->op == ARITHMETIC_PLUS)
                        oo->std = sqrt(oo->std * oo->std + v * v);
                    else if (std->op == ARITHMETIC_MULT)
                        oo->std *= v;
                    else if (std->op == ARITHMETIC_MIN)
                        oo->std = (oo->std < v) ? v : oo->std;
                    else if (std->op == ARITHMETIC_MAX)
                        oo->std = (oo->std > v) ? v : oo->std;
                    else
                        enkf_quit("programming error");
                }
            } else if (std->type == STDTYPE_FILE) {
                char* fname = (char*) std->data;
                int ni, nj, nk;

                enkf_printf("      adding error_std from %s %s:\n", fname, std->varname);

                grid_getdims(g, &ni, &nj, &nk);

                if (ot->issurface) {
                    float** v = alloc2d(nj, ni, sizeof(float));

                    readfield(fname, std->varname, 0, ni, nj, nk, v[0]);
                    for (o = nobs0; o < obs->nobs; ++o) {
                        observation* oo = &obs->data[o];
                        float vv;

                        if (oo->status != STATUS_OK)
                            continue;

                        vv = (float) interpolate2d(oo->fi, oo->fj, ni, nj, v, numlevels, isperiodic_i);
                        if (std->op == ARITHMETIC_EQ)
                            oo->std = vv;
                        else if (std->op == ARITHMETIC_PLUS)
                            oo->std = sqrt(oo->std * oo->std + vv * vv);
                        else if (std->op == ARITHMETIC_MULT)
                            oo->std *= vv;
                        else if (std->op == ARITHMETIC_MIN)
                            oo->std = (oo->std < vv) ? vv : oo->std;
                        else if (std->op == ARITHMETIC_MAX)
                            oo->std = (oo->std > vv) ? vv : oo->std;
                        else
                            enkf_quit("programming error");
                    }
                    free(v);
                } else {
                    float*** v = alloc3d(nk, nj, ni, sizeof(float));
                    int ksurf = grid_getsurflayerid(g);

                    read3dfield(fname, std->varname, ni, nj, nk, v[0][0]);
                    for (o = nobs0; o < obs->nobs; ++o) {
                        observation* oo = &obs->data[o];
                        float vv;

                        if (oo->status != STATUS_OK)
                            continue;

                        vv = (float) interpolate3d(oo->fi, oo->fj, oo->fk, ni, nj, nk, ksurf, v, numlevels, isperiodic_i);
                        if (std->op == ARITHMETIC_EQ)
                            oo->std = vv;
                        else if (std->op == ARITHMETIC_PLUS)
                            oo->std = sqrt(oo->std * oo->std + vv * vv);
                        else if (std->op == ARITHMETIC_MULT)
                            oo->std *= vv;
                        else if (std->op == ARITHMETIC_MIN)
                            oo->std = (oo->std < vv) ? vv : oo->std;
                        else if (std->op == ARITHMETIC_MAX)
                            oo->std = (oo->std > vv) ? vv : oo->std;
                        else
                            enkf_quit("programming error");
                    }
                    free(v);
                }
            }
        }
    }

    /*
     * report time range 
     */
    if (obs->nobs - nobs0 > 0) {
        double date_min = DBL_MAX;
        double date_max = -DBL_MAX;
        int i;

        for (i = nobs0; i < obs->nobs; ++i) {
            observation* o = &obs->data[i];

            if (!isnan(o->date))
                o->date -= obs->da_date;
            else
                o->date = 0.0;
            if (o->status != STATUS_OK)
                continue;
            if (o->date < date_min)
                date_min = o->date;
            if (o->date > date_max)
                date_max = o->date;
        }
        if (date_min <= date_max) {
            enkf_printf("      min date = %.3f\n", date_min);
            enkf_printf("      max date = %.3f\n", date_max);
        }
    }
    fflush(stdout);

    for (i = nobs0; i < obs->nobs; ++i) {
        if (obs_badob(obs, i)) {
            enkf_printf("        bad observation detected: ");
            obs_printob(obs, i);
            enkf_quit("bad observation");
        }
    }
}

/**
 */
int obs_checkforland(observations* obs, model* m)
{
    int hasland = 0;
    int i;

    for (i = 0; i < obs->nobs; ++i) {
        observation* o = &obs->data[i];
        obstype* ot = &obs->obstypes[o->type];
        int vid = model_getvarid(m, ot->varnames[0], 1);
        grid* g = model_getvargrid(m, vid);
        int ni, nj;

        grid_getdims(g, &ni, &nj, NULL);
        if (island(o->fi, o->fj, ni, nj, grid_getnumlevels(g), grid_isperiodic_i(g))) {
            o->status = STATUS_LAND;
            hasland = 1;
        }
    }

    return hasland;
}

/**
 */
void get_obsfiles(obsmeta* meta, int* nfiles, char*** fnames)
{
    int i;

    for (i = 0; i < meta->nfiles; ++i)
        find_files(meta->fnames[i], nfiles, fnames);
}

/**
 */
void print_obsstats(observations* obs, observations* sobs)
{
    int i;

    enkf_printf("    type    #used    #dropped #out_grd #out_obs #out_wnd #land    #shallow #badbatch#badvalue#superobs\n");
    enkf_printf("    --------------------------------------------------------------------------------------------------\n");
    for (i = 0; i < obs->nobstypes; ++i) {
        obstype* ot = &obs->obstypes[i];

        enkf_printf("    %-7s %-8d %-8d %-8d %-8d %-8d %-8d %-8d %-8d %-8d %-8d\n", ot->name, ot->ngood, ot->nobs - ot->ngood, ot->noutside_grid, ot->noutside_obsdomain, ot->noutside_obswindow, ot->nland, ot->nshallow, ot->nbadbatch, ot->nrange, sobs->obstypes[i].nobs);
    }
    enkf_printf("    total   %-8d %-8d %-8d %-8d %-8d %-8d %-8d %-8d %-8d %-8d\n", obs->ngood, obs->nobs - obs->ngood, obs->noutside_grid, obs->noutside_obsdomain, obs->noutside_obswindow, obs->nland, obs->nshallow, obs->nbadbatch, obs->nrange, sobs->nobs);
}
