#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

#include <gmp.h>

struct numlist {
    size_t len;   /* The actual number of elements */
    size_t alloc; /* The number of allocated elements */
    mpz_t *num;
};

struct prodtree {
    size_t height;
    struct numlist **level;
};

struct list_worker_thread {
    struct numlist *d, *s;      /* For the multiplication worker */
    struct numlist *X, *R, *nR; /* For the square/mod worker */
    struct numlist *G, *N;      /* For the divgcd worker */
    int tnum;
    int num_threads;
};


size_t intlog2(size_t num) {

    /* returns the log base-2 value of num rounded up to the nearest int */
    /* 0 -> 0
     * 1 -> 0
     * 2 -> 1
     * 3 -> 2
     * 4 -> 2
     * 5 -> 3
     * 8 -> 3
     * 9 -> 4
     * ...
     */
    size_t l2 = 0;
    for (size_t i = 1; i < num; i *= 2) {
        l2++;
    }

    return l2;
}


struct numlist * makenumlist(size_t len) {
    struct numlist *nlist = (struct numlist *)malloc(sizeof(struct numlist));
    assert(nlist != NULL);
    nlist->len = len;

    if (nlist->len == 0) {
        nlist->alloc = 1;
    } else {
        nlist->alloc = nlist->len;
    }

    nlist->num = (mpz_t *)calloc(sizeof(mpz_t), nlist->alloc);
    assert(nlist->num != NULL);

    for (size_t i = 0; i < nlist->alloc; i++) {
        mpz_init(nlist->num[i]);
    }

    return nlist;
}


void grownumlist(struct numlist *nlist) {
    assert(nlist != NULL);
    assert(nlist->num != NULL);

    nlist->alloc *= 2;
    nlist->num = (mpz_t *)reallocarray(nlist->num, sizeof(mpz_t), nlist->alloc);
    assert(nlist->num != NULL);

    for (size_t i = nlist->len; i < nlist->alloc; i++) {
        mpz_init(nlist->num[i]);
    }
}


void push_numlist(struct numlist *nlist, mpz_t n) {
    assert(nlist != NULL);
    assert(nlist->num != NULL);

    if (nlist->len >= nlist->alloc) {
        grownumlist(nlist);
    }

    mpz_set(nlist->num[nlist->len], n);
    nlist->len += 1;
}


struct numlist * copynumlist(struct numlist *nlist) {
    assert(nlist != NULL);
    assert(nlist->num != NULL);

    struct numlist *newlist = makenumlist(nlist->len);

    for (size_t i = 0; i < nlist->len; i++) {
        mpz_set(newlist->num[i], nlist->num[i]);
    }

    return newlist;
}


void freenumlist(struct numlist *nlist) {
    assert(nlist != NULL);
    assert(nlist->num != NULL);

    for (size_t i = 0; i < nlist->alloc; i++) {
        mpz_clear(nlist->num[i]);
    }

    free(nlist->num);
    free(nlist);
}


void freeprodtree(struct prodtree *ptree) {
    assert(ptree != NULL);
    assert(ptree->level != NULL);

    /* Skips the first level which was just a reference to the
     * initial input list that the product tree was created from
     * in the first place.
     */
    for (size_t l = 1; l < ptree->height; l++) {
        if (ptree->level[l] != NULL) {
            freenumlist(ptree->level[l]);
            ptree->level[l] = NULL;
        }
    }

    free(ptree->level);
    free(ptree);
}


void listmul(struct numlist *d, struct numlist *s) {
    assert(d != NULL);
    assert(d->num != NULL);
    assert(s != NULL);
    assert(s->num != NULL);
    assert(d->len > 0);

    for (size_t i = 0; i < (d->len - 1); i++) {
        mpz_mul(d->num[i], s->num[i * 2], s->num[(i * 2) + 1]);
    }

    /* Either copy or mul the last single / pair */
    if ((s->len & 1) == 0) {
        /* even source len so mul */
        mpz_mul(d->num[d->len - 1], s->num[s->len - 1], s->num[s->len - 2]);
    } else {
        /* odd source len so copy */
        mpz_set(d->num[d->len - 1], s->num[s->len - 1]);
    }
}


void * listmul_worker(void *lwtarg) {

    struct list_worker_thread *lwt = (struct list_worker_thread *)lwtarg;

    for (size_t i = lwt->tnum; i < (lwt->d->len - 1); i += lwt->num_threads) {
        mpz_mul(lwt->d->num[i], lwt->s->num[i * 2], lwt->s->num[(i * 2) + 1]);
    }

    return NULL;
}


void threaded_listmul(struct numlist *d, struct numlist *s, int num_threads) {
    assert(d != NULL);
    assert(d->num != NULL);
    assert(s != NULL);
    assert(s->num != NULL);
    assert(d->len > 0);
    assert(num_threads > 0);

    pthread_t *thread_ids = (pthread_t *)calloc(sizeof(pthread_t), num_threads);
    assert(thread_ids != NULL);
    pthread_attr_t *thread_attrs = (pthread_attr_t *)calloc(sizeof(pthread_attr_t), num_threads);
    assert(thread_attrs != NULL);
    struct list_worker_thread *lwts = (struct list_worker_thread *)calloc(sizeof(struct list_worker_thread), num_threads);
    assert(lwts != NULL);

    int err;
    for (int t = 0; t < num_threads; t++) {
        err = pthread_attr_init(&(thread_attrs[t]));
        if (err) {
            fprintf(stderr, "%s: error initializing attributes for thread %d\n", strerror(err), t);
            exit(255);
        } else {
            /*fprintf(stderr, "Creating multiplication worker thread %d\n", t);*/
        }

        lwts[t].d = d;
        lwts[t].s = s;
        lwts[t].tnum = t;
        lwts[t].num_threads = num_threads;

        err = pthread_create(&(thread_ids[t]), &(thread_attrs[t]), listmul_worker, &(lwts[t]));
        if (err) {
            fprintf(stderr, "%s: error initializing mulworker thread %d\n", strerror(err), t);
            exit(255);
        }
    }


    /* Either copy or mul the last single / pair */
    if ((s->len & 1) == 0) {
        /* even source len so mul */
        mpz_mul(d->num[d->len - 1], s->num[s->len - 1], s->num[s->len - 2]);
    } else {
        /* odd source len so copy */
        mpz_set(d->num[d->len - 1], s->num[s->len - 1]);
    }

    for (int t = 0; t < num_threads; t++) {
        pthread_join(thread_ids[t], NULL);
        if (err) {
            fprintf(stderr, "%s: error joining mulworker thread %d\n", strerror(err), t);
            exit(255);
        }
    }

    free(thread_ids);
    free(thread_attrs);
    free(lwts);
}


void * listsqmod_worker(void *lwtarg) {

    struct list_worker_thread *lwt = (struct list_worker_thread *)lwtarg;

    mpz_t sq;
    mpz_init(sq);
    for (size_t i = lwt->tnum; i < lwt->X->len; i += lwt->num_threads) {
        mpz_mul(sq, lwt->X->num[i], lwt->X->num[i]);
        mpz_mod(lwt->nR->num[i], lwt->R->num[i / 2], sq);
    }
    mpz_clear(sq);

    return NULL;
}


void threaded_listsqmod(struct numlist *X, struct numlist *R, struct numlist *nR, int num_threads) {
    assert(X != NULL);
    assert(X->num != NULL);
    assert(R != NULL);
    assert(R->num != NULL);
    assert(nR != NULL);
    assert(nR->num != NULL);
    assert(num_threads > 0);

    pthread_t *thread_ids = (pthread_t *)calloc(sizeof(pthread_t), num_threads);
    assert(thread_ids != NULL);
    pthread_attr_t *thread_attrs = (pthread_attr_t *)calloc(sizeof(pthread_attr_t), num_threads);
    assert(thread_attrs != NULL);
    struct list_worker_thread *lwts = (struct list_worker_thread *)calloc(sizeof(struct list_worker_thread), num_threads);
    assert(lwts != NULL);

    int err;
    for (int t = 0; t < num_threads; t++) {
        err = pthread_attr_init(&(thread_attrs[t]));
        if (err) {
            fprintf(stderr, "%s: error initializing attributes for sqmod thread %d\n", strerror(err), t);
            exit(255);
        } else {
            /*fprintf(stderr, "Creating multiplication worker thread %d\n", t);*/
        }

        lwts[t].X = X;
        lwts[t].R = R;
        lwts[t].nR = nR;
        lwts[t].tnum = t;
        lwts[t].num_threads = num_threads;

        err = pthread_create(&(thread_ids[t]), &(thread_attrs[t]), listsqmod_worker, &(lwts[t]));
        if (err) {
            fprintf(stderr, "%s: error initializing sqmodworker thread %d\n", strerror(err), t);
            exit(255);
        }
    }

    for (int t = 0; t < num_threads; t++) {
        pthread_join(thread_ids[t], NULL);
        if (err) {
            fprintf(stderr, "%s: error joining sqmodworker thread %d\n", strerror(err), t);
            exit(255);
        }
    }

    free(thread_ids);
    free(thread_attrs);
    free(lwts);
}


void * listdivgcd_worker(void *lwtarg) {

    struct list_worker_thread *lwt = (struct list_worker_thread *)lwtarg;

    mpz_t d;
    mpz_init(d);
    for (size_t i = lwt->tnum; i < lwt->G->len; i += lwt->num_threads) {
        mpz_divexact(d, lwt->R->num[i], lwt->N->num[i]);
        mpz_gcd(lwt->G->num[i], d, lwt->N->num[i]);
    }
    mpz_clear(d);

    return NULL;
}


void threaded_listdivgcd(struct numlist *G, struct numlist *R, struct numlist *N, int num_threads) {
    assert(G != NULL);
    assert(G->num != NULL);
    assert(R != NULL);
    assert(R->num != NULL);
    assert(N != NULL);
    assert(N->num != NULL);
    assert(num_threads > 0);

    pthread_t *thread_ids = (pthread_t *)calloc(sizeof(pthread_t), num_threads);
    assert(thread_ids != NULL);
    pthread_attr_t *thread_attrs = (pthread_attr_t *)calloc(sizeof(pthread_attr_t), num_threads);
    assert(thread_attrs != NULL);
    struct list_worker_thread *lwts = (struct list_worker_thread *)calloc(sizeof(struct list_worker_thread), num_threads);
    assert(lwts != NULL);

    int err;
    for (int t = 0; t < num_threads; t++) {
        err = pthread_attr_init(&(thread_attrs[t]));
        if (err) {
            fprintf(stderr, "%s: error initializing attributes for divgcd thread %d\n", strerror(err), t);
            exit(255);
        } else {
            /*fprintf(stderr, "Creating multiplication worker thread %d\n", t);*/
        }

        lwts[t].G = G;
        lwts[t].R = R;
        lwts[t].N = N;
        lwts[t].tnum = t;
        lwts[t].num_threads = num_threads;

        err = pthread_create(&(thread_ids[t]), &(thread_attrs[t]), listdivgcd_worker, &(lwts[t]));
        if (err) {
            fprintf(stderr, "%s: error initializing divgcdworker thread %d\n", strerror(err), t);
            exit(255);
        }
    }

    for (int t = 0; t < num_threads; t++) {
        pthread_join(thread_ids[t], NULL);
        if (err) {
            fprintf(stderr, "%s: error joining divgcdworker thread %d\n", strerror(err), t);
            exit(255);
        }
    }

    free(thread_ids);
    free(thread_attrs);
    free(lwts);
}


struct prodtree * producttree(struct numlist *nlist) {
    assert(nlist != NULL);
    assert(nlist->num != NULL);

    struct prodtree *ptree = (struct prodtree *)malloc(sizeof(struct prodtree));
    assert(ptree != NULL);

    ptree->height = intlog2(nlist->len) + 1;
    ptree->level = (struct numlist **)calloc(sizeof(struct numlist *), ptree->height);
    assert(ptree->level != NULL);

    /* The first level of the product tree is the list of integers we
     * were passed in the first place */
    ptree->level[0] = nlist; /*copynumlist(nlist);*/
    for (size_t l = 1; l < ptree->height; l++) {
        ptree->level[l] = makenumlist((ptree->level[l - 1]->len + 1) / 2);
        threaded_listmul(ptree->level[l], ptree->level[l - 1], 4);
    }

    return ptree;
}

struct numlist * fast_batchgcd(struct numlist *nlist) {
    assert(nlist != NULL);
    assert(nlist->num != NULL);

    /* def batchgcd_faster(X): */
    /*     prods = producttree(X) */
    /*     R = prods.pop() */
    /*     while prods: */
    /*         X = prods.pop() */
    /*         R = [R[floor(i/2)] % X[i]**2 for i in range(len(X))] */
    /*     return [gcd(r/n,n) for r,n in zip(R,X)] */

    struct prodtree *ptree = producttree(nlist);

    struct numlist *Rlist = ptree->level[ptree->height - 1]; /*copynumlist(ptree->level[ptree->height - 1]);*/
    struct numlist *newRlist;

    /*freenumlist(ptree->level[ptree->height - 1]);
      ptree->level[ptree->height - 1] = NULL;*/

    int needfree = 0;
    for (size_t up = 2; up <= ptree->height; up++) {
        struct numlist *Xlist = ptree->level[ptree->height - up];

        newRlist = makenumlist(Xlist->len);
        threaded_listsqmod(Xlist, Rlist, newRlist, 4);

        if (up != 2) {
            freenumlist(Rlist);
        }
        Rlist = newRlist;
        needfree = 1;

        /*freenumlist(Xlist);
          ptree->level[ptree->height - up] = NULL;*/
    }

    struct numlist *gcdlist = makenumlist(nlist->len);
    threaded_listdivgcd(gcdlist, Rlist, nlist, 4);

    if (needfree == 1) {
        freenumlist(Rlist);
    }
    freeprodtree(ptree);

    return gcdlist;
}


struct numlist * factor_coprimes(struct numlist *nlist, struct numlist *gcdlist) {
    assert(nlist != NULL);
    assert(nlist->num != NULL);
    assert(gcdlist != NULL);
    assert(gcdlist->num != NULL);

    /* The smallest co-prime list */
    struct numlist *cplist = makenumlist(nlist->len);

    size_t weak_count = 0; /* A count of the weak moduli */
    uint64_t *weakidx = calloc(nlist->len, sizeof(uint64_t));
    assert(weakidx != NULL);

    size_t weak_gcd_count = 0; /* A count of the number of weak moduli needing more GCD work */
    uint64_t *weakidx_gcd = calloc(nlist->len, sizeof(uint64_t));
    assert(weakidx_gcd != NULL);

    mpz_t q;
    mpz_init(q);

    /* Each GCD that isn't 1 is vulnerable and needs factoring */
    for (size_t i = 0; i < nlist->len; i++) {
        if (mpz_cmp_ui(gcdlist->num[i], 1) != 0) {
            /* This moduli is weak
             *
             * Case 1: The GCD is less than the moduli meaning
             * we already have factored the moduli. In that case
             * find the smaller of the two co-prime factors and store
             * it in the cplist.
             *
             * Case 2: The GCD is equal to moduli meaning this moduli
             * shares all factors with other moduli and it needs further
             * GCD efforts to separate out the co-primes.
             */

            /* Check case 1 that GCD is already less than moduli */
            if (mpz_cmp(gcdlist->num[i], nlist->num[i]) != 0) {
                /* Store this index */
                weakidx[weak_count] = i;
                weak_count++;

                /* Find the other factor */
                mpz_div(q, nlist->num[i], gcdlist->num[i]);

                /* Set co-prime factor to lesser of the two */
                if (mpz_cmp(gcdlist->num[i], q) < 0) {
                    mpz_set(cplist->num[i], gcdlist->num[i]);
                } else {
                    mpz_set(cplist->num[i], q);
                }
            } else {
                /* We're in case 2 where further GCD work is needed */

                /* Store this index */
                weakidx[weak_count] = i;
                weak_count++;

                /* Also track that we need to do more GCD work */
                weakidx_gcd[weak_gcd_count] = i;
                weak_gcd_count++;
            }

        } else {
            /* The smallest co-prime of a moduli that
             * doesn't share a prime with another moduli
             * is the moduli itself
             */
                mpz_set(cplist->num[i], nlist->num[i]);
        }
    }

    /* Now report on work remaining */
    fprintf(stderr, "Found %lu weak moduli out of %ld.\n", weak_count, nlist->len);
    fprintf(stderr, "Still need to perform GCD co-factoring on %lu weak moduli.\n", weak_gcd_count);
    fprintf(stderr, "Work still to do: O(%lu * %lu) == O(%lu)\n", weak_count, weak_gcd_count, weak_count * weak_gcd_count);

    /* To separate out the remaining co-primes we just do trial GCD on the remaining
     * weak moduli until we find a pair that only share one co-prime.
     * This makes the step quadratic in the number of weak moduli
     */
    mpz_t gcd;
    mpz_init(gcd);
    size_t weak_gcd_success = 0;
    for (size_t wgi = 0; wgi < weak_gcd_count; wgi++) {
        for (size_t wi = 0; wi < weak_count; wi++) {

            int idx_w = weakidx[wi];
            int idx_g = weakidx_gcd[wgi];

            /* Skip when these are the same index */
            if (idx_w == idx_g) {
                continue;
            }

            /* GCD this pair */
            mpz_gcd(gcd, nlist->num[idx_w], nlist->num[idx_g]);

            if (mpz_cmp_ui(gcd, 1) != 0) {
                /* Okay they share at least one factor */
                if (mpz_cmp(gcd, nlist->num[idx_g]) != 0) {
                    /* They only shared one factor */
                    mpz_div(q, nlist->num[idx_g], gcd);

                    /* Set co-prime factor to lesser of the two */
                    if (mpz_cmp(gcd, q) < 0) {
                        mpz_set(cplist->num[idx_g], gcd);
                    } else {
                        mpz_set(cplist->num[idx_g], q);
                    }

                    weak_gcd_success++;
                    break;
                }
            }
        }
    }

    fprintf(stderr, "Further found co-factors for %lu weak moduli.\n", weak_gcd_success);

    free(weakidx);
    free(weakidx_gcd);
    mpz_clear(q);
    mpz_clear(gcd);

    return cplist;
}




void print_numlist(struct numlist *nlist) {
    assert(nlist != NULL);
    assert(nlist->num != NULL);
    assert(nlist->len > 0);

    for (size_t i = 0; i < (nlist->len - 1); i++) {
        gmp_fprintf(stderr, "%Zd", nlist->num[i]);
        fprintf(stderr, ", ");
    }
    gmp_fprintf(stderr, "%Zd", nlist->num[nlist->len - 1]);
}


void print_prodtree(struct prodtree *ptree) {
    assert(ptree != NULL);
    assert(ptree->level != NULL);
    assert(ptree->height > 0);

    fprintf(stderr, "[");
    for (size_t i = 0; i < (ptree->height - 1); i++) {
        fprintf(stderr, "[");
        print_numlist(ptree->level[i]);
        fprintf(stderr, "], ");
    }
    fprintf(stderr, "[");
    print_numlist(ptree->level[ptree->height - 1]);
    fprintf(stderr, "]");

    fprintf(stderr, "]\n");
}


int main (void) {

    /* log2 test */
    /*for (size_t n = 0; n < 10; n++) {
     * fprintf(stderr, "intlog2(%ld) = %ld\n", n, intlog2(n));
     * }*/

    /* mpz_t mpz_temp; */
    /* mpz_init(mpz_temp); */

    /* prodtree test */
    /* struct numlist *nlist = makenumlist(0); */
    /* for (int n = 10; n <= 100; n += 10) { */
    /*     mpz_set_ui(mpz_temp, n); */
    /*     push_numlist(nlist, mpz_temp); */
    /* } */
    /* struct prodtree *ptree = producttree(nlist); */
    /* print_prodtree(ptree); */


    /* fast_batchgcd test */
    /* struct numlist *nlist = makenumlist(0); */
    /* for (int n = 1; n <= 50; n++) { */
    /*     mpz_set_ui(mpz_temp, n * 2 + 1); */
    /*     push_numlist(nlist, mpz_temp); */
    /* } */

    /* fprintf(stderr, "["); */
    /* print_numlist(nlist); */
    /* fprintf(stderr, "]\n"); */

    /* struct numlist *gcdlist = fast_batchgcd(nlist); */

    /* fprintf(stderr, "["); */
    /* print_numlist(gcdlist); */
    /* fprintf(stderr, "]\n"); */

    /* mpz_clear(mpz_temp); */
    /* freenumlist(nlist); */
    /* freenumlist(gcdlist); */

    /* Read lines from stdin where each line is a modulus in hex */
    struct numlist *nlist = makenumlist(0);
    mpz_t mpz_temp;
    mpz_init(mpz_temp);
    int done = 0;
    int line = 0;
    while (done == 0) {
        line++;
        int ret = gmp_fscanf(stdin, "%Zx\n", mpz_temp);
        if (ret == EOF) {
            done = 1;
        } else if (ret != 1) {
            fprintf(stderr, "Invalid modilus input on line %d\n", line);
        } else {
            push_numlist(nlist, mpz_temp);
        }
    }
    struct numlist *gcdlist = fast_batchgcd(nlist);

    struct numlist *cplist = factor_coprimes(nlist, gcdlist);

    for (size_t i = 0; i < nlist->len; i++) {
        if (mpz_cmp_ui(gcdlist->num[i], 1) != 0) {
            fprintf(stderr, "Found vulnerable modulus on line %lu: ", i + 1);
            gmp_fprintf(stderr, "%Zx", nlist->num[i]);
            fprintf(stderr, " with smallest co-factor ");
            gmp_fprintf(stderr, "%Zx", cplist->num[i]);
            fprintf(stderr, "\n");
        }
    }

    mpz_clear(mpz_temp);
    freenumlist(nlist);
    freenumlist(gcdlist);
    freenumlist(cplist);

    return 0;
}


