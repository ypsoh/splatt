

/******************************************************************************
 * INCLUDES
 *****************************************************************************/
#include "completion.h"
#include "../io.h"
#include "../util.h"

#include <math.h>
#include <omp.h>


/******************************************************************************
 * PRIVATE FUNCTIONS
 *****************************************************************************/

/**
* @brief Predict a value for a three-way tensor.
*
* @param model The model to use for the prediction.
* @param test The test tensor which gives us the model rows.
* @param index The nonzero to draw the row indices from. test[0][index] ...
*
* @return The predicted value.
*/
static inline val_t p_predict_val3(
    tc_model const * const model,
    sptensor_t const * const test,
    idx_t const index)
{
  val_t est = 0;
  idx_t const nfactors = model->rank;

  assert(test->nmodes == 0);

  idx_t const i = test->ind[0][index];
  idx_t const j = test->ind[1][index];
  idx_t const k = test->ind[2][index];

  val_t const * const restrict A = model->factors[0] + (i * nfactors);
  val_t const * const restrict B = model->factors[1] + (j * nfactors);
  val_t const * const restrict C = model->factors[2] + (k * nfactors);

  for(idx_t f=0; f < nfactors; ++f) {
    est += A[f] * B[f] * C[f];
  }

  return est;
}



/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

val_t tc_rmse(
    sptensor_t const * const test,
    tc_model const * const model,
    tc_ws * const ws)
{
  return sqrt(tc_loss_sq(test, model, ws) / test->nnz);
}





val_t tc_loss_sq(
    sptensor_t const * const test,
    tc_model const * const model,
    tc_ws * const ws)
{
  val_t loss_obj = 0.;
  val_t const * const restrict test_vals = test->vals;

  #pragma omp parallel reduction(+:loss_obj)
  {
    val_t * buffer = ws->thds[omp_get_thread_num()].scratch[0];

    #pragma omp for schedule(static)
    for(idx_t x=0; x < test->nnz; ++x) {
      val_t const err = test_vals[x] - tc_predict_val(model, test, x, buffer);
      loss_obj += err * err;
    }
  }

  return loss_obj;
}



val_t tc_frob_sq(
    tc_model const * const model,
    tc_ws const * const ws)
{
  idx_t const nfactors = model->rank;

  val_t reg_obj = 0.;

  #pragma omp parallel reduction(+:reg_obj)
  {

    for(idx_t m=0; m < model->nmodes; ++m) {
      val_t accum = 0;
      val_t const * const restrict mat = model->factors[m];

      #pragma omp for schedule(static) nowait
      for(idx_t x=0; x < model->dims[m] * nfactors; ++x) {
        accum += mat[x] * mat[x];
      }
      reg_obj += ws->regularization[m] * accum;
    }
  } /* end omp parallel */

  return reg_obj;
}



val_t tc_predict_val(
    tc_model const * const model,
    sptensor_t const * const test,
    idx_t const index,
    val_t * const restrict buffer)
{
  idx_t const nfactors = model->rank;

  if(test->nmodes == 3) {
    return p_predict_val3(model, test, index);
  }

  /* initialize accumulation of each latent factor with the first row */
  idx_t const row_id = test->ind[0][index];
  val_t const * const init_row = model->factors[0] + (row_id * nfactors);
  for(idx_t f=0; f < nfactors; ++f) {
    buffer[f] = init_row[f];
  }

  /* now multiply each factor by A(i,:), B(j,:) ... */
  idx_t const nmodes = model->nmodes;
  for(idx_t m=1; m < nmodes; ++m) {
    idx_t const row_id = test->ind[m][index];
    val_t const * const row = model->factors[m] + (row_id * nfactors);
    for(idx_t f=0; f < nfactors; ++f) {
      buffer[f] *= row[f];
    }
  }

  /* finally, sum the factors to form the final estimated value */
  val_t est = 0;
  for(idx_t f=0; f < nfactors; ++f) {
    est += buffer[f];
  }

  return est;
}



/******************************************************************************
 * WORKSPACE FUNCTIONS
 *****************************************************************************/

tc_model * tc_model_alloc(
    sptensor_t const * const train,
    idx_t const rank,
    splatt_tc_type const which)
{
  tc_model * model = splatt_malloc(sizeof(*model));

  model->which = which;
  model->rank = rank;
  model->nmodes = train->nmodes;
  for(idx_t m=0; m < train->nmodes; ++m) {
    model->dims[m] = train->dims[m];

    idx_t const size = model->dims[m] * rank;
    model->factors[m] = splatt_malloc(size * sizeof(**(model->factors)));
    fill_rand(model->factors[m], size);
  }

  return model;
}


void tc_model_free(
    tc_model * model)
{
  for(idx_t m=0; m < model->nmodes; ++m) {
    splatt_free(model->factors[m]);
  }

  splatt_free(model);
}


tc_ws * tc_ws_alloc(
    tc_model const * const model,
    idx_t nthreads)
{
  tc_ws * ws = splatt_malloc(sizeof(*ws));

  idx_t const nmodes = model->nmodes;
  ws->nmodes = nmodes;

  /* some reasonable defaults */
  ws->learn_rate = 0.001;

  ws->max_its = 1000;
  for(idx_t m=0; m < nmodes; ++m) {
    ws->regularization[m] = 0.02;
  }

  idx_t const rank = model->rank;

  /* allocate gradients */
  for(idx_t m=0; m < nmodes; ++m) {
    if(model->which == SPLATT_TC_GD) {
      ws->gradients[m] = splatt_malloc(model->dims[m] * rank *
          sizeof(**(ws->gradients)));
    } else {
      ws->gradients[m] = NULL;
    }
  }

  /* allocate thread structures */
  ws->nthreads = nthreads;
  switch(model->which) {
  case SPLATT_TC_GD:
    ws->thds = thd_init(nthreads, 1, rank * sizeof(val_t));
    break;
  case SPLATT_TC_SGD:
    ws->thds = thd_init(nthreads, 1, rank * sizeof(val_t));
    break;
  case SPLATT_TC_ALS:
    ws->thds = thd_init(nthreads, 3,
        rank * sizeof(val_t),           /* prediction buffer */
        rank * sizeof(val_t),           /* MTTKRP buffer */
        rank * rank * sizeof(val_t)); /* normal equations */
    break;
  case SPLATT_TC_NALGS:
    ws->thds = NULL;
    fprintf(stderr, "SPLATT: completion algorithm not recognized.\n");
    break;
  }

  return ws;
}


void tc_ws_free(
    tc_ws * ws)
{
  thd_free(ws->thds, ws->nthreads);
  for(idx_t m=0; m < ws->nmodes; ++m) {
    splatt_free(ws->gradients[m]);
  }
  splatt_free(ws);
}


