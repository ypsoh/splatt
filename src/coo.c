

/******************************************************************************
 * INCLUDES
 *****************************************************************************/
#include "coo.h"
#include "matrix.h"
#include "sort.h"
#include "io.h"
#include "timer.h"

#include <math.h>


/******************************************************************************
 * PRIVATE FUNCTONS
 *****************************************************************************/
static inline int p_same_coord(
  splatt_coo const * const tt,
  idx_t const i,
  idx_t const j)
{
  idx_t const nmodes = tt->nmodes;
  if(nmodes == 3) {
    return (tt->ind[0][i] == tt->ind[0][j]) &&
           (tt->ind[1][i] == tt->ind[1][j]) &&
           (tt->ind[2][i] == tt->ind[2][j]);
  } else {
    for(idx_t m=0; m < nmodes; ++m) {
      if(tt->ind[m][i] != tt->ind[m][j]) {
        return 0;
      }
    }
    return 1;
  }
}




/******************************************************************************
 * PUBLIC FUNCTONS
 *****************************************************************************/

val_t coo_frobsq(
    splatt_coo const * const tensor)
{
  /* accumulate into double to help with some precision loss */
  double norm = 0;
  idx_t const nnz = tensor->nnz;
  val_t const * const restrict vals = tensor->vals;
  #pragma omp parallel for reduction(+:norm)
  for(idx_t n=0; n < nnz; ++n) {
    norm += vals[n] * vals[n];
  } /* end omp parallel */

  return (val_t) norm;
}


double tt_density(
  splatt_coo const * const tt)
{
  double root = pow((double)tt->nnz, 1./(double)tt->nmodes);
  double density = 1.0;
  for(idx_t m=0; m < tt->nmodes; ++m) {
    density *= root / (double)tt->dims[m];
  }

  return density;
}


idx_t * tt_get_slices(
  splatt_coo const * const tt,
  idx_t const m,
  idx_t * nunique)
{
  /* get maximum number of unique slices */
  idx_t minidx = tt->dims[m];
  idx_t maxidx = 0;

  idx_t const nnz = tt->nnz;
  idx_t const * const inds = tt->ind[m];

  /* find maximum number of uniques */
  for(idx_t n=0; n < nnz; ++n) {
    minidx = SS_MIN(minidx, inds[n]);
    maxidx = SS_MAX(maxidx, inds[n]);
  }
  /* +1 because maxidx is inclusive, not exclusive */
  idx_t const maxrange = 1 + maxidx - minidx;

  /* mark slices which are present and count uniques */
  idx_t * slice_mkrs = calloc(maxrange, sizeof(*slice_mkrs));
  idx_t found = 0;
  for(idx_t n=0; n < nnz; ++n) {
    assert(inds[n] >= minidx);
    idx_t const idx = inds[n] - minidx;
    if(slice_mkrs[idx] == 0) {
      slice_mkrs[idx] = 1;
      ++found;
    }
  }
  *nunique = found;

  /* now copy unique slices */
  idx_t * slices = splatt_malloc(found * sizeof(*slices));
  idx_t ptr = 0;
  for(idx_t i=0; i < maxrange; ++i) {
    if(slice_mkrs[i] == 1) {
      slices[ptr++] = i + minidx;
    }
  }

  free(slice_mkrs);

  return slices;
}


idx_t * tt_get_hist(
  splatt_coo const * const tt,
  idx_t const mode)
{
  idx_t * restrict hist = splatt_malloc(tt->dims[mode] * sizeof(*hist));
  memset(hist, 0, tt->dims[mode] * sizeof(*hist));

  idx_t const * const restrict inds = tt->ind[mode];
  #pragma omp parallel for schedule(static)
  for(idx_t x=0; x < tt->nnz; ++x) {
    #pragma omp atomic
    ++hist[inds[x]];
  }

  return hist;
}


idx_t tt_remove_dups(
  splatt_coo * const tt)
{
  tt_sort(tt, 0, NULL);

  idx_t const nmodes = tt->nmodes;

  idx_t newnnz = 0;
  for(idx_t nnz = 1; nnz < tt->nnz; ++nnz) {
    /* if the two nnz are the same, average them */
    if(p_same_coord(tt, newnnz, nnz)) {
      tt->vals[newnnz] += tt->vals[nnz];
    } else {
      /* new another nnz */
      ++newnnz;
      for(idx_t m=0; m < nmodes; ++m) {
        tt->ind[m][newnnz] = tt->ind[m][nnz];
      }
      tt->vals[newnnz] = tt->vals[nnz];
    }
  }
  ++newnnz;

  idx_t const diff = tt->nnz - newnnz;
  tt->nnz = newnnz;
  return diff;
}


idx_t tt_remove_empty(
  splatt_coo * const tt)
{
  idx_t dim_sizes[MAX_NMODES];

  idx_t nremoved = 0;

  /* Allocate indmap */
  idx_t const nmodes = tt->nmodes;
  idx_t const nnz = tt->nnz;

  idx_t maxdim = 0;
  for(idx_t m=0; m < tt->nmodes; ++m) {
    maxdim = tt->dims[m] > maxdim ? tt->dims[m] : maxdim;
  }
  /* slice counts */
  idx_t * scounts = splatt_malloc(maxdim * sizeof(*scounts));

  for(idx_t m=0; m < nmodes; ++m) {
    dim_sizes[m] = 0;
    memset(scounts, 0, maxdim * sizeof(*scounts));

    /* Fill in indmap */
    for(idx_t n=0; n < tt->nnz; ++n) {
      /* keep track of #unique slices */
      if(scounts[tt->ind[m][n]] == 0) {
        scounts[tt->ind[m][n]] = 1;
        ++dim_sizes[m];
      }
    }

    /* move on if no remapping is necessary */
    if(dim_sizes[m] == tt->dims[m]) {
      tt->indmap[m] = NULL;
      continue;
    }

    nremoved += tt->dims[m] - dim_sizes[m];

    /* Now scan to remove empty slices */
    idx_t ptr = 0;
    for(idx_t i=0; i < tt->dims[m]; ++i) {
      if(scounts[i] == 1) {
        scounts[i] = ptr++;
      }
    }

    tt->indmap[m] = splatt_malloc(dim_sizes[m] * sizeof(**tt->indmap));

    /* relabel all indices in mode m */
    tt->dims[m] = dim_sizes[m];
    for(idx_t n=0; n < tt->nnz; ++n) {
      idx_t const global = tt->ind[m][n];
      idx_t const local = scounts[global];
      assert(local < dim_sizes[m]);
      tt->indmap[m][local] = global; /* store local -> global mapping */
      tt->ind[m][n] = local;
    }
  }

  splatt_free(scounts);
  return nremoved;
}



/******************************************************************************
 * PUBLIC FUNCTONS
 *****************************************************************************/
splatt_coo * tt_read(
  char const * const ifname)
{
  return tt_read_file(ifname);
}


splatt_coo * tt_alloc(
  idx_t const nnz,
  idx_t const nmodes)
{
  splatt_coo * tt = (splatt_coo*) splatt_malloc(sizeof(*tt));
  tt->tiled = SPLATT_NOTILE;

  tt->nnz = nnz;
  tt->vals = splatt_malloc(nnz * sizeof(*tt->vals));

  tt->nmodes = nmodes;
  tt->dims = splatt_malloc(nmodes * sizeof(*tt->dims));
  tt->ind  = splatt_malloc(nmodes * sizeof(*tt->ind));
  for(idx_t m=0; m < nmodes; ++m) {
    tt->ind[m] = splatt_malloc(nnz * sizeof(**tt->ind));
    tt->indmap[m] = NULL;
  }

  return tt;
}


void tt_fill(
  splatt_coo * const tt,
  idx_t const nnz,
  idx_t const nmodes,
  idx_t ** const inds,
  val_t * const vals)
{
  tt->tiled = SPLATT_NOTILE;
  tt->nnz = nnz;
  tt->vals = vals;
  tt->ind = inds;

  tt->nmodes = nmodes;
  tt->dims = splatt_malloc(nmodes * sizeof(*tt->dims));
  for(idx_t m=0; m < nmodes; ++m) {
    tt->indmap[m] = NULL;

    tt->dims[m] = 1 + inds[m][0];
    for(idx_t i=1; i < nnz; ++i) {
      tt->dims[m] = SS_MAX(tt->dims[m], 1 + inds[m][i]);
    }
  }
}



void tt_free(
  splatt_coo * tt)
{
  tt->nnz = 0;
  for(idx_t m=0; m < tt->nmodes; ++m) {
    splatt_free(tt->ind[m]);
    splatt_free(tt->indmap[m]);
  }
  tt->nmodes = 0;
  splatt_free(tt->dims);
  splatt_free(tt->ind);
  splatt_free(tt->vals);
  splatt_free(tt);
}

spmatrix_t * tt_unfold(
  splatt_coo * const tt,
  idx_t const mode)
{
  idx_t nrows = tt->dims[mode];
  idx_t ncols = 1;

  for(idx_t m=1; m < tt->nmodes; ++m) {
    ncols *= tt->dims[(mode + m) % tt->nmodes];
  }

  /* sort tt */
  tt_sort(tt, mode, NULL);

  /* allocate and fill matrix */
  spmatrix_t * mat = spmat_alloc(nrows, ncols, tt->nnz);
  idx_t * const rowptr = mat->rowptr;
  idx_t * const colind = mat->colind;
  val_t * const mvals  = mat->vals;

  /* make sure to skip ahead to the first non-empty slice */
  idx_t row = 0;
  for(idx_t n=0; n < tt->nnz; ++n) {
    /* increment row and account for possibly empty ones */
    while(row <= tt->ind[mode][n]) {
      rowptr[row++] = n;
    }
    mvals[n] = tt->vals[n];

    idx_t col = 0;
    idx_t mult = 1;
    for(idx_t m = 0; m < tt->nmodes; ++m) {
      idx_t const off = tt->nmodes - 1 - m;
      if(off == mode) {
        continue;
      }
      col += tt->ind[off][n] * mult;
      mult *= tt->dims[off];
    }

    colind[n] = col;
  }
  /* account for any empty rows at end, too */
  for(idx_t r=row; r <= nrows; ++r) {
    rowptr[r] = tt->nnz;
  }

  return mat;
}

