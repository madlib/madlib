/*!
 * \file fm.c
 *
 * \brief Flajolet-Martin sketch implementation
 */

/*!
 * \defgroup fmsketch FM (Flajolet-Martin)
 * \ingroup sketches
 * \about
 * Flajolet-Martin's distinct count estimation
 * implemented as a user-defined aggregate.
 *
 * \implementation
 * In a nutshell, the FM sketch
 * is based on the idea of a bitmap whose bits are "turned on" by hashes of
 * values in the domain.  It is arranged so that
 * as you move left-to-right in that bitmap, the expected number of domain values
 * that can turn on the bit decreases exponentially.
 * After hashing all the values this way, the location of the first 0 from the
 * left of the bitmap is correlated with the
 * number of distinct values.  This idea is smoothed across a number of
 * trials using multiple independent hash functions on multiple bitmaps.
 *
 * The FM sketch technique works poorly with small inputs, so we
 * explicitly count the first 12K distinct values in a main-memory
 * data structure before switching over to sketching.
 *
 * See the paper mentioned below
 * for detailed explanation, formulae, and pseudocode.
 *
 * \usage
 *   <c>fmsketch_dcount(col anytype)</c> is a UDA that can be run on any column of any type.  It returns an approximation to the number of distinct values in the column (a la <c>COUNT(DISTINCT x)</c>, but faster and approximate).  Like any aggregate, it can be combined with a GROUP BY clause to do distinct counts per group.  Example:\code
 *    SELECT pronargs, madlib.fmsketch_dcount(proname) AS distinct_hat, count(proname)
 *      FROM pg_proc
 *  GROUP BY pronargs;
 *    \endcode
 *
 * \literature
 * - P. Flajolet and N.G. Martin.  Probabilistic counting algorithms for data base applications, Journal of Computer and System Sciences 31(2), pp 182-209, 1985.  http://algo.inria.fr/flajolet/Publications/FlMa85.pdf
 */

/* THIS CODE MAY NEED TO BE REVISITED TO ENSURE ALIGNMENT! */

#include "postgres.h"
#include "utils/array.h"
#include "utils/elog.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "libpq/md5.h"
#include "nodes/execnodes.h"
#include "fmgr.h"
#include "sketch_support.h"
#include "sortasort.h"
#include <ctype.h>

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define NMAP 256
#define FMSKETCH_SZ (VARHDRSZ + NMAP*(MD5_HASHLEN_BITS)/CHAR_BIT)

/*!
 * For FM, empirically, estimates seem to fall below 1% error around 12k
 * distinct vals
 */
#define MINVALS 1024*12

/*!
 * initial size for a sortasort: we'll guess at 8 bytes per string.
 * sortasort will grow dynamically if we guessed too low
 */
#define SORTASORT_INITIAL_STORAGE  sizeof(sortasort) + MINVALS*sizeof(uint32) + \
    8*MINVALS

typedef enum {SMALL, BIG} fmstatus;

/*!
 * \internal
 * \brief transition value struct for FM sketches
 *
 * because FM sketches work poorly on small numbers of values,
 * our transval can be in one of two modes.
 * for "SMALL" numbers of values (<=MINVALS), the storage array
 * is a "sortasort" data structure containing an array of input values.
 * for "BIG" datasets (>MINVAL), it is an array of FM sketch bitmaps.
 * \endinternal
 */
typedef struct {
    fmstatus status;
    char storage[0];
} fmtransval;

Datum __fmsketch_trans_c(bytea *, char *);
Datum __fmsketch_count_distinct_c(bytea *);
Datum __fmsketch_trans(PG_FUNCTION_ARGS);
Datum __fmsketch_count_distinct(PG_FUNCTION_ARGS);
Datum __fmsketch_merge(PG_FUNCTION_ARGS);
Datum big_or(bytea *bitmap1, bytea *bitmap2);
bytea *fmsketch_sortasort_insert(bytea *, char *);
bytea *fm_new(void);

PG_FUNCTION_INFO_V1(__fmsketch_trans);

/*! UDA transition function for the fmsketch aggregate. */
Datum __fmsketch_trans(PG_FUNCTION_ARGS)
{
    bytea *     transblob = (bytea *)PG_GETARG_BYTEA_P(0);
    fmtransval *transval;
    Oid         element_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
    Oid         funcOid;
    bool        typIsVarlena;
    char *      string;
    Datum       retval;

    if (!OidIsValid(element_type))
        elog(ERROR, "could not determine data type of input");

    /*
     * This is Postgres boilerplate for UDFs that modify the data in their own context.
     * Such UDFs can only be correctly called in an agg context since regular scalar
     * UDFs are essentially stateless across invocations.
     */
    if (!(fcinfo->context &&
          (IsA(fcinfo->context, AggState)
    #ifdef NOTGP
           || IsA(fcinfo->context, WindowAggState)
    #endif
          )))
        elog(
            ERROR,
            "UDF call to a function that only works for aggs (destructive pass by reference)");


    /* get the provided element, being careful in case it's NULL */
    if (!PG_ARGISNULL(1)) {
        /* figure out the outfunc for this type */
        getTypeOutputInfo(element_type, &funcOid, &typIsVarlena);

        /*
         * convert input to a cstring
         * we'll pfree it before we exit
         */
        string = OidOutputFunctionCall(funcOid, PG_GETARG_DATUM(1));

        /*
         * if this is the first call, initialize transval to hold a sortasort
         * on the first call, we should have the empty string (if the agg was declared properly!)
         */
        if (VARSIZE(transblob) <= VARHDRSZ) {
            size_t blobsz = VARHDRSZ + sizeof(fmtransval) +
                            SORTASORT_INITIAL_STORAGE;
            transblob = (bytea *)palloc0(blobsz);
            SET_VARSIZE(transblob, blobsz);
            transval = (fmtransval *)VARDATA(transblob);
            transval->status = SMALL;
            sortasort_init((sortasort *)transval->storage,
                           MINVALS,
                           SORTASORT_INITIAL_STORAGE);
        }
        else {
            /* extract the existing transval from the transblob */
            transval = (fmtransval *)VARDATA(transblob);
        }

        /*
         * if we've seen < MINVALS distinct values, place string into the sortasort
         * XXXX Would be cleaner to try the sortasort insert and if it fails, then continue.
         */
        if (transval->status == SMALL
            && ((sortasort *)(transval->storage))->num_vals <
            MINVALS) {
            retval =
                PointerGetDatum(fmsketch_sortasort_insert(
                                    transblob,
                                    string));
            pfree(string);
            PG_RETURN_DATUM(retval);
        }

        /*
         * if we've seen exactly MINVALS distinct values, create FM bitmaps
         * and load the contents of the sortasort into the FM sketch
         */
        else if (transval->status == SMALL
                 && ((sortasort *)(transval->storage))->num_vals ==
                 MINVALS) {
            int        i;
            sortasort *s = (sortasort *)(transval->storage);
            bytea *    newblob = fm_new();
            transval = (fmtransval *)VARDATA(newblob);

            /*
             * "catch up" on the past as if we were doing FM from the beginning:
             * apply the FM sketching algorithm to each value previously stored in the sortasort
             */
            for (i = 0; i < MINVALS; i++)
                __fmsketch_trans_c(newblob, SORTASORT_GETVAL(s,i));

            /*
             * XXXX would like to pfree the old transblob, but the memory allocator doesn't like it
             * XXXX Meanwhile we know that this memory "leak" is of fixed size and will get
             * XXXX deallocated "soon" when the memory context is destroyed.
             */
            /* drop through to insert the current string in "BIG" mode */
            transblob = newblob;
        }

        /*
         * if we're here we've seen >=MINVALS distinct values and are in BIG mode.
         * Just for sanity, let's check.
         */
        if (transval->status != BIG)
            elog(
                ERROR,
                "FM sketch failed internal sanity check");

        /* Apply FM algorithm to this string */
        retval = __fmsketch_trans_c(transblob, string);
        pfree(string);
        PG_RETURN_DATUM(retval);
    }
    else PG_RETURN_NULL();
}

/*!
 * generate a bytea holding a transval in BIG mode, with the right amount of
 * zero bits for an empty FM sketch.
 */
bytea *fm_new()
{
    int         fmsize = VARHDRSZ + sizeof(fmtransval) + FMSKETCH_SZ;
    /* use palloc0 to make sure it's initialized to 0 */
    bytea *     newblob = (bytea *)palloc0(fmsize);
    fmtransval *transval;

    SET_VARSIZE(newblob, fmsize);
    transval = (fmtransval *)VARDATA(newblob);
    transval->status = BIG;

    SET_VARSIZE((bytea *)transval->storage, FMSKETCH_SZ);
    return(newblob);
}

/*!
 * Main logic of Flajolet and Martin's sketching algorithm.
 * For each call, we get an md5 hash of the value passed in.
 * First we use the hash as a random number to choose one of
 * the NMAP bitmaps at random to update.
 * Then we find the position "rmost" of the rightmost 1 bit in the hashed value.
 * We then turn on the "rmost"-th bit FROM THE LEFT in the chosen bitmap.
 * \param transblob the transition value packed into a bytea
 * \param input a textual representation of the value to hash
 */
Datum __fmsketch_trans_c(bytea *transblob, char *input)
{
    fmtransval * transval = (fmtransval *) VARDATA(transblob);
    bytea *      bitmaps = (bytea *)transval->storage;
    uint64       index;
    uint8 *      c;
    int          rmost;
    Datum        result;

    c = (uint8 *)VARDATA(DatumGetByteaP(md5_datum(input)));

    /*
     * During the insertion we insert each element
     * in one bitmap only (a la Flajolet pseudocode, page 16).
     * Choose the bitmap by taking the 64 high-order bits worth of hash value mod NMAP
     */
    index = (*(uint64 *)c) % NMAP;

    /*
     * Find index of the rightmost non-0 bit.  Turn on that bit (from left!) in the sketch.
     */
    rmost = rightmost_one(c, 1, MD5_HASHLEN_BITS, 0);

    /*
     * last argument must be the index of the bit position from the right.
     * i.e. position 0 is the rightmost.
     * so to set the bit at rmost from the left, we subtract from the total number of bits.
     */
    result =
        array_set_bit_in_place(bitmaps, NMAP, MD5_HASHLEN_BITS, index,
                               (MD5_HASHLEN_BITS - 1) - rmost);
    return PointerGetDatum(transblob);
}

PG_FUNCTION_INFO_V1(__fmsketch_count_distinct);

/*! UDA final function to get count(distinct) out of an FM sketch */
Datum __fmsketch_count_distinct(PG_FUNCTION_ARGS)
{
    fmtransval *transval = (fmtransval *)VARDATA((PG_GETARG_BYTEA_P(0)));

    if (VARSIZE((PG_GETARG_BYTEA_P(0))) == VARHDRSZ)
        /* nothing was ever aggregated! */
        return (0);

    /* if status is not BIG then get count from sortasort */
    if (transval->status == SMALL)
        return ((sortasort *)(transval->storage))->num_vals;
    /* else get count via fm */
    else if (transval->status != BIG) {
        elog(ERROR, "FM transval neither SMALL nor BIG");
        return(0);
    }
    else     /* transval->status == BIG */
        return __fmsketch_count_distinct_c((bytea *)transval->storage);
}

/*!
 * Finish up the Flajolet-Martin approximation.
 * We sum up the number of leading 1 bits across all bitmaps in the sketch.
 * Then we use the FM magic formula to estimate the distinct count.
 * \params bitmaps the FM Sketch
 */
Datum __fmsketch_count_distinct_c(bytea *bitmaps)
{
/*  int R = 0; // Flajolet/Martin's R is handled by leftmost_zero */
    uint32        S = 0;
    static double phi = 0.77351;     /*
                                      * the magic constant
                                      * char out[NMAP*MD5_HASHLEN_BITS];
                                      */
    int    i;
    uint32 lz;

    for (i = 0; i < NMAP; i++)
    {
        lz = leftmost_zero((uint8 *)VARDATA(
                               bitmaps), NMAP, MD5_HASHLEN_BITS, i);
        S = S + lz;
    }

    PG_RETURN_INT64(ceil( ((double)NMAP/
                           phi) * pow(2.0, (double)S/(double)NMAP) ));
}


PG_FUNCTION_INFO_V1(__fmsketch_merge);

/*!
 * Greenplum "prefunc": a function to merge 2 transvals computed at different machines.
 * For simple FM, this is trivial: just OR together the two arrays of bitmaps.
 * But we have to deal with cases where one or both transval is SMALL: i.e. it
 * holds a sortasort, not an FM sketch.
 *
 * XXX  TESTING: Ensure we exercise all branches!
 */
Datum __fmsketch_merge(PG_FUNCTION_ARGS)
{
    bytea *     transblob1 = (bytea *)PG_GETARG_BYTEA_P(0);
    bytea *     transblob2 = (bytea *)PG_GETARG_BYTEA_P(1);
    fmtransval *transval1, *transval2;
    sortasort * s1, *s2;
    sortasort * sortashort, *sortabig;
    bytea *     tblob_big, *tblob_small;
    uint32      i;

    /* deal with the case where one or both items is the initial value of '' */
    if (VARSIZE(transblob1) == VARHDRSZ) {
        PG_RETURN_DATUM(PointerGetDatum(transblob2));
    }
    if (VARSIZE(transblob2) == VARHDRSZ) {
        PG_RETURN_DATUM(PointerGetDatum(transblob1));
    }

    transval1 = (fmtransval *)VARDATA(transblob1);
    transval2 = (fmtransval *)VARDATA(transblob2);

    if (transval1->status == BIG && transval2->status == BIG) {
        /* easy case: merge two FM sketches via bitwise OR. */
        PG_RETURN_DATUM(big_or(transblob1, transblob2));
    }
    else if (transval1->status == SMALL && transval2->status == SMALL) {
        s1 = (sortasort *)(transval1->storage);
        s2 = (sortasort *)(transval2->storage);
        tblob_big =
            (s1->num_vals > s2->num_vals) ? transblob1 : transblob2;
        tblob_small =
            (s1->num_vals > s2->num_vals) ? transblob2 : transblob1;
        sortashort =
            (sortasort *)(((fmtransval *)(tblob_small))->storage);
        sortabig = (sortasort *)(((fmtransval *)(tblob_big))->storage);
        if (sortabig->num_vals + sortashort->num_vals <=
            sortabig->capacity) {
            /*
             * we have room in sortabig
             * one could imagine a more efficient (merge-based) sortasort merge,
             * but for now we just copy the values from the smaller sortasort into
             * the bigger one.
             */
            for (i = 0; i < sortashort->num_vals; i++)
                tblob_big = fmsketch_sortasort_insert(
                    tblob_big,
                    SORTASORT_GETVAL(
                        sortashort,i));
            PG_RETURN_DATUM(PointerGetDatum(tblob_big));
        }
        /* else drop through. */
    }

    /*
     * if we got here, then at most one transval is BIG, i.e. one or both transvals is SMALL.
     * need to form an FM sketch and populate with the SMALL transval(s)
     */
    if (transval1->status == SMALL && transval2->status == SMALL)
        tblob_big = fm_new();
    else
        tblob_big =
            (transval1->status == BIG) ? transblob1 : transblob2;

    if (transval1->status == SMALL) {
        s1 = (sortasort *)(transval1->storage);
        for(i = 0; i < s1->num_vals; i++) {
            __fmsketch_trans_c(tblob_big, SORTASORT_GETVAL(s1,i));
        }
    }
    if (transval2->status == SMALL) {
        s2 = (sortasort *)(transval2->storage);
        for(i = 0; i < s2->num_vals; i++) {
            __fmsketch_trans_c(tblob_big, SORTASORT_GETVAL(s2,i));
        }
    }
    PG_RETURN_DATUM(PointerGetDatum(tblob_big));
}

/*! OR of two big bitmaps, for gathering sketches computed in parallel. */
Datum big_or(bytea *bitmap1, bytea *bitmap2)
{
    bytea * out;
    uint32  i;

    if (VARSIZE(bitmap1) != VARSIZE(bitmap2))
        elog(ERROR,
             "attempting to OR two different-sized bitmaps: %d, %d",
             VARSIZE(bitmap1),
             VARSIZE(bitmap2));

    out = (bytea *)palloc(VARSIZE(bitmap1));
    SET_VARSIZE(out, VARSIZE(bitmap1));

    /* could probably be more efficient doing this 32 or 64 bits at a time */
    for (i=0; i < VARSIZE(bitmap1) - VARHDRSZ; i+=8)
        ((char *)(VARDATA(out)))[i] = ((char *)(VARDATA(bitmap1)))[i] |
                                      ((char *)(VARDATA(bitmap2)))[i];

    PG_RETURN_BYTEA_P(out);
}

/*!
 * wrapper for insertion into a sortasort. calls sorasort_try_insert and if that fails it
 * makes more space for insertion (double or more the size) and tries again.
 * \param transblob the current transition value packed into a bytea
 * \param v the value to be inserted
 */
bytea *fmsketch_sortasort_insert(bytea *transblob, char *v)
{
    sortasort *s_in =
        (sortasort *)((fmtransval *)VARDATA(transblob))->storage;
    bytea *    newblob;
    bool       success = FALSE;
    size_t     new_storage_sz;
    size_t     newsize;

    if (s_in->num_vals >= s_in->capacity)
        elog(ERROR, "attempt to insert into full sortasort");

    success = sortasort_try_insert(s_in, v);
    if (success < 0)
        elog(ERROR, "insufficient directory capacity in sortasort");

    if (success == TRUE) return (transblob);

    /* XXX  THIS WHILE LOOP WILL SUCCEED THE FIRST TRY ... REMOVE IT. */
    while (!success) {
        /*
         * else insufficient space
         * allocate a fmtransval with double-big storage area plus room for v
         * should work 2nd time around the loop.
         * we can't use repalloc because it fails trying to free the old transblob
         */

        new_storage_sz = s_in->storage_sz*2 + strlen(v);
        /* XXX THIS POINTER ARITHMETIC SHOULD BE HIDDEN BY A MACRO IN THE SORTASORT LIBRARY! */
        newsize = VARHDRSZ + sizeof(fmtransval) + sizeof(sortasort)
                  + s_in->capacity*sizeof(s_in->dir[0]) +
                  new_storage_sz;
        newblob = (bytea *)palloc(newsize);
        memcpy(newblob, transblob, VARSIZE(transblob));
        SET_VARSIZE(newblob, newsize);
        s_in = (sortasort *)((fmtransval *)VARDATA(newblob))->storage;
        s_in->storage_sz = new_storage_sz;
        /*
         * Can't figure out how to make pfree happy with transblob
         * pfree(transblob);
         */
        transblob = newblob;
        success = sortasort_try_insert(s_in, v);
    }
    return(transblob);
}
