#ifndef SPLATT_SVD_H
#define SPLATT_SVD_H

#include "base.h"

#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

#define left_singulars splatt_left_singulars
EXTERNC void left_singulars(
    val_t * inmat,
    val_t * outmat,
    idx_t const nrows,
    idx_t const ncols,
    idx_t const rank);

#define make_core splatt_make_core
EXTERNC void make_core(
    val_t * ttmc,
    val_t * lastmat,
    val_t * core,
    idx_t const nmodes,
    idx_t const mode,
    idx_t const * const nfactors,
    idx_t const nlongrows);


#undef EXTERNC

#endif