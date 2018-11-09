/* 
 * Copyright 2014 Kevin M Greenan
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or
 * other materials provided with the distribution.  THIS SOFTWARE IS PROVIDED BY
 * THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * jerasure_rs_cauchy backend implementation
 *
 * vi: set noai tw=79 ts=4 sw=4:
 */

#include <stdio.h>
#include <stdlib.h>
#include <jerasure.h>
#include <cauchy.h>

#include "erasurecode.h"
#include "erasurecode_backend.h"
#include "erasurecode_helpers.h"
#include "erasurecode_helpers_ext.h"

#define JERASURE_RS_CAUCHY_LIB_MAJOR 2
#define JERASURE_RS_CAUCHY_LIB_MINOR 0
#define JERASURE_RS_CAUCHY_LIB_REV   0
#define JERASURE_RS_CAUCHY_LIB_VER_STR "2.0"
#define JERASURE_RS_CAUCHY_LIB_NAME "jerasure_rs_cauchy"
#if defined(__MACOS__) || defined(__MACOSX__) || defined(__OSX__) || defined(__APPLE__)
#define JERASURE_RS_CAUCHY_SO_NAME "libJerasure.dylib"
#else
#define JERASURE_RS_CAUCHY_SO_NAME "libJerasure.so.2"
#endif

/* Forward declarations */
struct ec_backend_op_stubs jerasure_rs_cauchy_ops;
struct ec_backend jerasure_rs_cauchy;
struct ec_backend_common backend_jerasure_rs_cauchy;

typedef int* (*cauchy_original_coding_matrix_func)(int, int, int);
typedef int* (*jerasure_matrix_to_bitmatrix_func)(int, int, int, int *);
typedef int** (*jerasure_smart_bitmatrix_to_schedule_func)
    (int, int, int, int *);
typedef void (*jerasure_bitmatrix_encode_func)
    (int, int, int, int *, char **, char **, int, int);
typedef int (*jerasure_bitmatrix_decode_func)
    (int, int, int, int *, int, int *,char **, char **, int, int);
typedef int * (*jerasure_erasures_to_erased_func)(int, int, int *);
typedef int (*jerasure_make_decoding_bitmatrix_func)
    (int, int, int, int *, int *, int *, int *);
typedef void (*jerasure_bitmatrix_dotprod_func)
    (int, int, int *, int *, int,char **, char **, int, int);
typedef void (*galois_uninit_field_func)(int);

/*
 * ToDo (KMG): Should we make this a parameter, or is that
 * exposing too much b.s. to the client?
 */
#define PYECC_CAUCHY_PACKETSIZE sizeof(long) * 128

struct jerasure_rs_cauchy_descriptor {
    /* calls required for init */
    cauchy_original_coding_matrix_func cauchy_original_coding_matrix;
    jerasure_matrix_to_bitmatrix_func jerasure_matrix_to_bitmatrix;
    jerasure_smart_bitmatrix_to_schedule_func jerasure_smart_bitmatrix_to_schedule;

    /* calls required for free */
    galois_uninit_field_func galois_uninit_field;

    /* calls required for encode */
    jerasure_bitmatrix_encode_func jerasure_bitmatrix_encode;
                            
    
    /* calls required for decode */
    jerasure_bitmatrix_decode_func jerasure_bitmatrix_decode;
                            
    
    /* calls required for reconstruct */
    jerasure_erasures_to_erased_func jerasure_erasures_to_erased;
    jerasure_make_decoding_bitmatrix_func jerasure_make_decoding_bitmatrix;
    jerasure_bitmatrix_dotprod_func jerasure_bitmatrix_dotprod;

    /* fields needed to hold state */
    int *matrix;
    int *bitmatrix;
    int **schedule;
    int k;
    int m;
    int w;
};
static void free_rs_cauchy_desc(
        struct jerasure_rs_cauchy_descriptor *jerasure_desc );


static int jerasure_rs_cauchy_encode(void *desc, char **data, char **parity,
        int blocksize)
{
    struct jerasure_rs_cauchy_descriptor *jerasure_desc = 
        (struct jerasure_rs_cauchy_descriptor*) desc;

    /* FIXME - make jerasure_bitmatrix_encode return a value */
    jerasure_desc->jerasure_bitmatrix_encode(jerasure_desc->k, jerasure_desc->m,
                                jerasure_desc->w, jerasure_desc->bitmatrix,
                                data, parity, blocksize,
                                PYECC_CAUCHY_PACKETSIZE);

    return 0;
}

static int jerasure_rs_cauchy_decode(void *desc, char **data, char **parity,
        int *missing_idxs, int blocksize)
{
    struct jerasure_rs_cauchy_descriptor *jerasure_desc = 
        (struct jerasure_rs_cauchy_descriptor*)desc;

    return jerasure_desc->jerasure_bitmatrix_decode(jerasure_desc->k, 
                                             jerasure_desc->m, 
                                             jerasure_desc->w, 
                                             jerasure_desc->bitmatrix, 
                                             0, 
                                             missing_idxs,
                                             data, 
                                             parity, 
                                             blocksize, 
                                             PYECC_CAUCHY_PACKETSIZE); 
}

static int jerasure_rs_cauchy_reconstruct(void *desc, char **data, char **parity,
        int *missing_idxs, int destination_idx, int blocksize)
{
    int k, m, w;                  /* erasure code paramters */
    int ret = 0;                  /* return code */
    int *decoding_row = NULL;     /* decoding matrix row for decode */
    int *erased = NULL;           /* k+m length list of erased frag ids */
    int *dm_ids = NULL;           /* k length list of fragment ids */
    int *decoding_matrix = NULL;  /* matrix for decoding */

    struct jerasure_rs_cauchy_descriptor *jerasure_desc = 
        (struct jerasure_rs_cauchy_descriptor*) desc;
    k = jerasure_desc->k;
    m = jerasure_desc->m;
    w = jerasure_desc->w;

    if (destination_idx < k) {
        dm_ids = (int *) alloc_zeroed_buffer(sizeof(int) * k);
        decoding_matrix = (int *) alloc_zeroed_buffer(sizeof(int *) * k * k * w * w);
        erased = jerasure_desc->jerasure_erasures_to_erased(k, m, missing_idxs);
        if (NULL == decoding_matrix || NULL == dm_ids || NULL == erased) {
            goto out;
        }

        ret = jerasure_desc->jerasure_make_decoding_bitmatrix(k, m, w, 
                                               jerasure_desc->bitmatrix,
                                               erased, decoding_matrix, dm_ids);
        if (ret == 0) {
            decoding_row = decoding_matrix + (destination_idx * k * w * w);
   
            jerasure_desc->jerasure_bitmatrix_dotprod(jerasure_desc->k, jerasure_desc->w, 
                                   decoding_row, dm_ids, destination_idx,
                                   data, parity, blocksize, PYECC_CAUCHY_PACKETSIZE);
        } else {
           /*
            * ToDo (KMG) I know this is not needed, but keeping to prevent future 
            * memory leaks, as this function will be better optimized for decoding 
            * missing parity 
            */
            goto out;
        }
    } else {
        /*
         * If it is parity we are reconstructing, then just call decode.
         * ToDo (KMG): We can do better than this, but this should perform just
         * fine for most cases.  We can adjust the decoding matrix like we
         * did with ISA-L.
         */
        jerasure_desc->jerasure_bitmatrix_decode(k, m, w,
                                             jerasure_desc->bitmatrix, 
                                             0, 
                                             missing_idxs,
                                             data, 
                                             parity, 
                                             blocksize, 
                                             PYECC_CAUCHY_PACKETSIZE); 
    }

out:
    free(erased);
    free(decoding_matrix);
    free(dm_ids);
    
    return ret;
}

/*
 * Caller will allocate an array of size k for fragments_needed
 * 
 */
static int jerasure_rs_cauchy_min_fragments(void *desc, int *missing_idxs,
        int *fragments_to_exclude, int *fragments_needed)
{
    struct jerasure_rs_cauchy_descriptor *jerasure_desc = 
        (struct jerasure_rs_cauchy_descriptor*)desc;
    uint64_t exclude_bm = convert_list_to_bitmap(fragments_to_exclude);
    uint64_t missing_bm = convert_list_to_bitmap(missing_idxs) | exclude_bm;
    int i;
    int j = 0;
    int ret = -1;

    for (i = 0; i < (jerasure_desc->k + jerasure_desc->m); i++) {
        if (!(missing_bm & (1 << i))) {
            fragments_needed[j] = i;
            j++;
        }
        if (j == jerasure_desc->k) {
            ret = 0;
            fragments_needed[j] = -1;
            break;
        }
    }

    return ret;
}

#define DEFAULT_W 4
static void * jerasure_rs_cauchy_init(struct ec_backend_args *args,
        void *backend_sohandle)
{
    struct jerasure_rs_cauchy_descriptor *desc = NULL;
    int k, m, w;
    
    desc = (struct jerasure_rs_cauchy_descriptor *)
           malloc(sizeof(struct jerasure_rs_cauchy_descriptor));
    if (NULL == desc) {
        return NULL;
    }

    /* validate the base EC arguments */
    k = args->uargs.k;
    m = args->uargs.m;
    if (args->uargs.w <= 0)
        args->uargs.w = DEFAULT_W;
    w = args->uargs.w;

    /* store the base EC arguments in the descriptor */
    desc->k = k;
    desc->m = m;
    desc->w = w;

    /* validate EC arguments */
    {
        long long max_symbols;
        max_symbols = 1LL << w;
        if ((k + m) > max_symbols) {
            goto error;
        }
    }

    /* fill in function addresses */
    desc->jerasure_bitmatrix_encode = jerasure_bitmatrix_encode;
    desc->jerasure_bitmatrix_decode = jerasure_bitmatrix_decode;
    desc->cauchy_original_coding_matrix = cauchy_original_coding_matrix;
    desc->jerasure_matrix_to_bitmatrix = jerasure_matrix_to_bitmatrix;
    desc->jerasure_smart_bitmatrix_to_schedule = jerasure_smart_bitmatrix_to_schedule;
    desc->jerasure_make_decoding_bitmatrix = jerasure_make_decoding_bitmatrix;
    desc->jerasure_bitmatrix_dotprod = jerasure_bitmatrix_dotprod;
    desc->jerasure_erasures_to_erased = jerasure_erasures_to_erased;
    desc->galois_uninit_field = (galois_uninit_field_func)galois_uninit_field;

    /* setup the Cauchy matrices and schedules */
    desc->matrix = desc->cauchy_original_coding_matrix(k, m, w);
    if (NULL == desc->matrix) {
        goto error;
    }
    desc->bitmatrix = desc->jerasure_matrix_to_bitmatrix(k, m, w, desc->matrix);
    if (NULL == desc->bitmatrix) {
        goto bitmatrix_error;
    }
    desc->schedule = desc->jerasure_smart_bitmatrix_to_schedule(k, m, w, desc->bitmatrix);
    if (NULL == desc->schedule) {
        goto schedule_error;
    }

    return desc;

schedule_error:
    free(desc->bitmatrix);
bitmatrix_error:
    free(desc->matrix);
error:
    free(desc);
    return NULL;
}

/**
 * Return the element-size, which is the number of bits stored 
 * on a given device, per codeword.  
 * 
 * Returns the size in bits!
 */
static int
jerasure_rs_cauchy_element_size(void* desc)
{
    struct jerasure_rs_cauchy_descriptor *jerasure_desc = 
        (struct jerasure_rs_cauchy_descriptor*)desc;

    return jerasure_desc->w * PYECC_CAUCHY_PACKETSIZE * 8;
}

static void free_rs_cauchy_desc(
        struct jerasure_rs_cauchy_descriptor *jerasure_desc )
{
    int i = 0;
    int **schedule = NULL;
    bool end_of_array = false;

    if (jerasure_desc == NULL) {
        return;
    }

    /*
     * jerasure allocates some internal data structures for caching
     * fields. It will allocate one for w, and if we do anything that
     * needs to xor a region >= 16 bytes, it will also allocate one
     * for 32. Fortunately we can safely uninit any value; if it
     * wasn't inited it will be ignored.
     */
    jerasure_desc->galois_uninit_field(jerasure_desc->w);
    jerasure_desc->galois_uninit_field(32);
    free(jerasure_desc->matrix);
    free(jerasure_desc->bitmatrix);

    // NOTE, based on an inspection of the jerasure code used to build the
    // the schedule array, it appears that the sentinal used to signal the end
    // of the array is a value of -1 in the first int field in the dereferenced
    // value. We use this determine when to stop free-ing elements. See the
    // jerasure_smart_bitmatrix_to_schedule and 
    // jerasure_dumb_bitmatrix_to_schedule functions in jerasure.c for the
    // details.
    schedule = jerasure_desc->schedule;
    if (schedule != NULL) {
        while (!end_of_array) {
            if (schedule[i] == NULL || schedule[i][0] == -1) {
                end_of_array = true;
            }
            free(schedule[i]);
            i++;
        }
    }

    free(schedule);
    free(jerasure_desc);
}

static int jerasure_rs_cauchy_exit(void *desc)
{
    struct jerasure_rs_cauchy_descriptor *jerasure_desc = 
        (struct jerasure_rs_cauchy_descriptor*)desc;
    free_rs_cauchy_desc(jerasure_desc);
    return 0;
}

/*
 * For the time being, we only claim compatibility with versions that
 * match exactly
 */
static bool jerasure_rs_cauchy_is_compatible_with(uint32_t version) {
    return version == backend_jerasure_rs_cauchy.ec_backend_version;
}

struct ec_backend_op_stubs jerasure_rs_cauchy_op_stubs = {
    .INIT                       = jerasure_rs_cauchy_init,
    .EXIT                       = jerasure_rs_cauchy_exit,
    .ENCODE                     = jerasure_rs_cauchy_encode,
    .DECODE                     = jerasure_rs_cauchy_decode,
    .FRAGSNEEDED                = jerasure_rs_cauchy_min_fragments,
    .RECONSTRUCT                = jerasure_rs_cauchy_reconstruct,
    .ELEMENTSIZE                = jerasure_rs_cauchy_element_size,
    .ISCOMPATIBLEWITH           = jerasure_rs_cauchy_is_compatible_with,
    .GETMETADATASIZE            = get_backend_metadata_size_zero,
    .GETENCODEOFFSET            = get_encode_offset_zero,
};

struct ec_backend_common backend_jerasure_rs_cauchy = {
    .id                         = EC_BACKEND_JERASURE_RS_CAUCHY,
    .name                       = JERASURE_RS_CAUCHY_LIB_NAME,
    .soname                     = JERASURE_RS_CAUCHY_SO_NAME,
    .soversion                  = JERASURE_RS_CAUCHY_LIB_VER_STR,
    .ops                        = &jerasure_rs_cauchy_op_stubs,
    .ec_backend_version         = _VERSION(JERASURE_RS_CAUCHY_LIB_MAJOR,
                                           JERASURE_RS_CAUCHY_LIB_MINOR,
                                           JERASURE_RS_CAUCHY_LIB_REV),
};
