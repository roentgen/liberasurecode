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
 * jerasure_rs_vand backend implementation
 *
 * vi: set noai tw=79 ts=4 sw=4:
 */

#include <stdio.h>
#include <stdlib.h>
#include <jerasure.h>
#include <reed_sol.h>

#include "erasurecode.h"
#include "erasurecode_backend.h"
#include "erasurecode_helpers.h"
#include "erasurecode_helpers_ext.h"

#define JERASURE_RS_VAND_LIB_MAJOR 2
#define JERASURE_RS_VAND_LIB_MINOR 0
#define JERASURE_RS_VAND_LIB_REV   0
#define JERASURE_RS_VAND_LIB_VER_STR "2.0"
#define JERASURE_RS_VAND_LIB_NAME "jerasure_rs_vand"
#if defined(__MACOS__) || defined(__MACOSX__) || defined(__OSX__) || defined(__APPLE__)
#define JERASURE_RS_VAND_SO_NAME "libJerasure.dylib"
#else
#define JERASURE_RS_VAND_SO_NAME "libJerasure.so.2"
#endif

/* Forward declarations */
struct ec_backend_op_stubs jerasure_rs_vand_ops;
struct ec_backend jerasure_rs_vand;
struct ec_backend_common backend_jerasure_rs_vand;

typedef int* (*reed_sol_vandermonde_coding_matrix_func)(gf2_t*, int, int, int);
typedef void (*jerasure_matrix_encode_func)(gf2_t*, int, int, int, int*, char **, char **, int); 
typedef int (*jerasure_matrix_decode_func)(gf2_t*, int, int, int, int *, int, int*, char **, char **, int);
typedef int (*jerasure_make_decoding_matrix_func)(gf2_t*, int, int, int, int *, int *, int *, int *);
typedef int * (*jerasure_erasures_to_erased_func)(int, int, int *);
typedef void (*jerasure_matrix_dotprod_func)(gf2_t*, int, int, int *,int *, int,char **, char **, int);
typedef void (*galois_uninit_field_func)(gf2_t*, int);

struct jerasure_rs_vand_descriptor {
    /* calls required for init */
    reed_sol_vandermonde_coding_matrix_func reed_sol_vandermonde_coding_matrix;

    /* calls required for free */
    galois_uninit_field_func galois_uninit_field;

    /* calls required for encode */
    jerasure_matrix_encode_func jerasure_matrix_encode;
    
    /* calls required for decode */
    jerasure_matrix_decode_func jerasure_matrix_decode;
    
    /* calls required for reconstruct */
    jerasure_make_decoding_matrix_func jerasure_make_decoding_matrix;
    jerasure_erasures_to_erased_func jerasure_erasures_to_erased;
    jerasure_matrix_dotprod_func jerasure_matrix_dotprod;

    /* fields needed to hold state */
    gf2_t* g;
    int *matrix;
    int k;
    int m;
    int w;
};

static int jerasure_rs_vand_encode(void *desc, char **data, char **parity,
        int blocksize)
{
    struct jerasure_rs_vand_descriptor *jerasure_desc = 
        (struct jerasure_rs_vand_descriptor*) desc;

    /* FIXME - make jerasure_matrix_encode return a value */
    jerasure_desc->jerasure_matrix_encode(jerasure_desc->g, jerasure_desc->k, jerasure_desc->m,
            jerasure_desc->w, jerasure_desc->matrix, data, parity, blocksize);

    return 0;
}

static int jerasure_rs_vand_decode(void *desc, char **data, char **parity,
        int *missing_idxs, int blocksize)
{
    struct jerasure_rs_vand_descriptor *jerasure_desc = 
        (struct jerasure_rs_vand_descriptor*)desc;

    /* FIXME - make jerasure_matrix_decode return a value */
    jerasure_desc->jerasure_matrix_decode(jerasure_desc->g, jerasure_desc->k,
            jerasure_desc->m, jerasure_desc->w,
            jerasure_desc->matrix, 1, missing_idxs, data, parity, blocksize);

    return 0;
}

static int jerasure_rs_vand_reconstruct(void *desc, char **data, char **parity,
        int *missing_idxs, int destination_idx, int blocksize)
{
    int ret = 0;                  /* return code */
    int *decoding_row;            /* decoding matrix row for decode */
    int *erased = NULL;           /* k+m length list of erased frag ids */
    int *dm_ids = NULL;           /* k length list of frag ids */
    int *decoding_matrix = NULL;  /* matrix for decoding */

    struct jerasure_rs_vand_descriptor *jerasure_desc = 
        (struct jerasure_rs_vand_descriptor*) desc;
    
    if (destination_idx < jerasure_desc->k) {
        dm_ids = (int *) alloc_zeroed_buffer(sizeof(int) * jerasure_desc->k);
        decoding_matrix = (int *)
            alloc_zeroed_buffer(sizeof(int*) * jerasure_desc->k * jerasure_desc->k);
        erased = jerasure_desc->jerasure_erasures_to_erased(jerasure_desc->k,
                jerasure_desc->m, missing_idxs);
        if (NULL == decoding_matrix || NULL == dm_ids || NULL == erased) {
            goto out;
        }

        ret = jerasure_desc->jerasure_make_decoding_matrix(jerasure_desc->g, jerasure_desc->k,
                jerasure_desc->m, jerasure_desc->w, jerasure_desc->matrix,
                erased, decoding_matrix, dm_ids);

        decoding_row = decoding_matrix + (destination_idx * jerasure_desc->k);
    
        if (ret == 0) {
            jerasure_desc->jerasure_matrix_dotprod(jerasure_desc->g, jerasure_desc->k,
                    jerasure_desc->w, decoding_row, dm_ids, destination_idx,
                    data, parity, blocksize);
        } else {
            /*
             * ToDo (KMG) I know this is not needed, but keeping to prevent future 
             *  memory leaks, as this function will be better optimized for decoding 
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
        jerasure_desc->jerasure_matrix_decode(jerasure_desc->g, jerasure_desc->k,
                        jerasure_desc->m, jerasure_desc->w,
                        jerasure_desc->matrix, 1, missing_idxs, data, parity, blocksize);
        goto parity_reconstr_out;
    }

out:
    free(erased);
    free(decoding_matrix);
    free(dm_ids);

parity_reconstr_out:
    return ret;
}

static int jerasure_rs_vand_min_fragments(void *desc, int *missing_idxs,
        int *fragments_to_exclude, int *fragments_needed)
{
    struct jerasure_rs_vand_descriptor *jerasure_desc = 
        (struct jerasure_rs_vand_descriptor*)desc;

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

#define DEFAULT_W 16
static void * jerasure_rs_vand_init(struct ec_backend_args *args,
        void *backend_sohandle)
{
    struct jerasure_rs_vand_descriptor *desc = NULL;
    
    desc = (struct jerasure_rs_vand_descriptor *)
           malloc(sizeof(struct jerasure_rs_vand_descriptor));
    if (NULL == desc) {
        return NULL;
    }

    desc->k = args->uargs.k;
    desc->m = args->uargs.m;

    if (args->uargs.w <= 0)
        args->uargs.w = DEFAULT_W;

    /* store w back in args so upper layer can get to it */
    desc->w = args->uargs.w;

    /* validate EC arguments */
    {
        long long max_symbols;
        if (desc->w != 8 && desc->w != 16 && desc->w != 32) {
            goto error;
        }
        max_symbols = 1LL << desc->w;
        if ((desc->k + desc->m) > max_symbols) {
            goto error;
        }
     }

    /* fill in function addresses */
    desc->jerasure_matrix_encode = jerasure_matrix_encode;
    desc->jerasure_matrix_decode = jerasure_matrix_decode;
    desc->jerasure_make_decoding_matrix = jerasure_make_decoding_matrix;
    desc->jerasure_matrix_dotprod = jerasure_matrix_dotprod;
    desc->jerasure_erasures_to_erased = jerasure_erasures_to_erased;
    desc->reed_sol_vandermonde_coding_matrix = reed_sol_vandermonde_coding_matrix;
    desc->galois_uninit_field = (galois_uninit_field_func)galois_uninit_field;

    desc->g = galois_init_empty();
    if (NULL == desc->g) {
        goto error;
    }

    desc->matrix = desc->reed_sol_vandermonde_coding_matrix(
            desc->g, desc->k, desc->m, desc->w);
    if (NULL == desc->matrix) {
        goto error;
    }

    return desc;

error:
    free(desc);

    return NULL;
}

/**
 * Return the element-size, which is the number of bits stored
 * on a given device, per codeword.  For Vandermonde, this is
 * 'w'.  For somthing like cauchy, this is packetsize * w.
 *
 * Returns the size in bits!
 */
static int
jerasure_rs_vand_element_size(void* desc)
{
    struct jerasure_rs_vand_descriptor *jerasure_desc =
        (struct jerasure_rs_vand_descriptor*)desc;

    /* Note that cauchy will return pyeclib_handle->w * PYECC_CAUCHY_PACKETSIZE * 8 */
    return jerasure_desc->w;
}

static int jerasure_rs_vand_exit(void *desc)
{
    struct jerasure_rs_vand_descriptor *jerasure_desc = NULL;
    
    jerasure_desc = (struct jerasure_rs_vand_descriptor*) desc;

    /*
     * jerasure allocates some internal data structures for caching
     * fields. It will allocate one for w, and if we do anything that
     * needs to xor a region >= 16 bytes, it will also allocate one
     * for 32. Fortunately we can safely uninit any value; if it
     * wasn't inited it will be ignored.
     */
    jerasure_desc->galois_uninit_field(jerasure_desc->g, jerasure_desc->w);
    jerasure_desc->galois_uninit_field(jerasure_desc->g, 32);
    galois_destroy(jerasure_desc->g);
    free(jerasure_desc->matrix);
    free(jerasure_desc);

    return 0;
}

/*
 * For the time being, we only claim compatibility with versions that
 * match exactly
 */
static bool jerasure_rs_vand_is_compatible_with(uint32_t version) {
    return version == backend_jerasure_rs_vand.ec_backend_version;
}

struct ec_backend_op_stubs jerasure_rs_vand_op_stubs = {
    .INIT                       = jerasure_rs_vand_init,
    .EXIT                       = jerasure_rs_vand_exit,
    .ENCODE                     = jerasure_rs_vand_encode,
    .DECODE                     = jerasure_rs_vand_decode,
    .FRAGSNEEDED                = jerasure_rs_vand_min_fragments,
    .RECONSTRUCT                = jerasure_rs_vand_reconstruct,
    .ELEMENTSIZE                = jerasure_rs_vand_element_size,
    .ISCOMPATIBLEWITH           = jerasure_rs_vand_is_compatible_with,
    .GETMETADATASIZE            = get_backend_metadata_size_zero,
    .GETENCODEOFFSET            = get_encode_offset_zero,
};

struct ec_backend_common backend_jerasure_rs_vand = {
    .id                         = EC_BACKEND_JERASURE_RS_VAND,
    .name                       = JERASURE_RS_VAND_LIB_NAME,
    .soname                     = JERASURE_RS_VAND_SO_NAME,
    .soversion                  = JERASURE_RS_VAND_LIB_VER_STR,
    .ops                        = &jerasure_rs_vand_op_stubs,
    .ec_backend_version         = _VERSION(JERASURE_RS_VAND_LIB_MAJOR,
                                           JERASURE_RS_VAND_LIB_MINOR,
                                           JERASURE_RS_VAND_LIB_REV),
};
