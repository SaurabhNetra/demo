/* omp_mc.c --
 *
 * This code is a prototype Monte Carlo computation (though right now
 * it simply computes the expected value of the uniform generator, which
 * is a little silly).  It has the following interesting features:
 *
 * 1.  The pseudorandom numbers are generated by independently-seeded
 *     instances of the Mersenne twister RNG (where the seeds are
 *     generated on a single thread via the system random() function).  
 *     Note that this generator is thread-safe because the state
 *     variable is an explicit argument at each step.  This is not
 *     always the case!  Also, note that the random number generator
 *     is often the most subtle part of a parallel Monte Carlo code.
 *
 * 2.  The code uses adaptive error estimation to terminate as soon as
 *     it has enough data to get the 1-sigma error bars below some relative
 *     tolerance.  Unlike an a priori decision (i.e. "run a million trials
 *     and then take stock"), this termination criterion involves some
 *     coordination between the threads.  The coordination can be made
 *     relatively inexpensive by only updating global counts after doing
 *     a large enough batch on each thread.
 *
 * 3.  Timing is done using the omp_get_wtime function, which returns the
 *     wall clock time (as opposed to the CPU time for a particular
 *     process or thread).
 *
 * 4.  The code uses the getopt library to process the arguments.  While
 *     this has nothing in particular to do with the numerics or the parallel
 *     operation, it's still a good thing to know about.
 * 
 * In timing experiments on my laptop, this code gets very good speedup
 * on two processors (as it should).
 */

#pragma offload_attribute(push,target(mic)) //{

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>
#include <omp.h>
#include "mt19937p.h"


/* Parameters for termination criterion */
double rtol      = 1e-2;
long   maxtrials = 1000000;
int    nbatch    = 500;


int is_converged(double sum_X, double sum_X2, double ntrials, 
                 double rtol, long maxtrials)
{
    double EX  = sum_X / ntrials;
    double EX2 = sum_X2 / ntrials;
    double varX   = EX2-EX*EX;
    return (varX/(EX*EX)/ntrials < rtol*rtol || ntrials > maxtrials);
}


double run_trial(struct mt19937p* mt)
{
    double X = 0;
    X = genrand(mt);  /* Generate [0,1] rand */
    return X;
}

#pragma offload_attribute(pop) //}

void process_args(int argc, char** argv)
{
    int c;
    while ((c = getopt(argc, argv, "p:t:n:b:")) != -1) {
        switch (c) {
        case 'p':
            break;
        case 't':
            rtol = atof(optarg);
            if (rtol < 0) {
                fprintf(stderr, "rtol must be positive\n");
                exit(-1);
            }
            break;
        case 'n':
            maxtrials = atol(optarg);
            if (maxtrials < 1) {
                fprintf(stderr, "maxtrials must be positive\n");
                exit(-1);
            }
            break;
        case 'b':
            nbatch = atoi(optarg);
            if (nbatch < 1) {
                fprintf(stderr, "nbatch must be positive\n");
                exit(-1);
            }
            break;
        case '?':
            if (optopt == 'p' || optopt == 't' || 
                optopt == 'n' || optopt == 'b')
                fprintf(stderr, "Option -%c requires argument\n", optopt);
            else 
                fprintf(stderr, "Unknown option '-%c'.\n", optopt);
            exit(-1);
            break;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "No non-option arguments allowed\n");
        exit(-1);
    }
}


void print_params()
{
    printf("--- Run parameters:\n");
    printf("rtol: %e\n", rtol);
    printf("maxtrials: %ld\n", maxtrials);
    printf("nbatch: %d\n", nbatch);
}


int main(int argc, char** argv)
{
    int nthreads = -1;
    double EX, EX2, stdX, t_elapsed;
    double t1, t2;
    int i;

    /* Monte Carlo results */
    double all_sum_X   = 0;
    double all_sum_X2  = 0;
    long   all_ntrials = 0;

    /* Private state */
    struct mt19937p mt;
    int done_flag;
    int t, tid, tnbatch;
    double sum_X, sum_X2;
    long seed;

    srandom(clock());
    process_args(argc, argv);

    t1 = omp_get_wtime();
#pragma offload target(mic)
#pragma omp parallel \
    shared(all_sum_X, all_sum_X2, all_ntrials, nbatch)         \
    private(mt, seed, done_flag, t, tid, sum_X, sum_X2)
    {
        nthreads = omp_get_max_threads();

#pragma omp critical
        seed = random();

        tnbatch = nbatch;
        tid = omp_get_thread_num();
        sgenrand(seed, &mt);
        done_flag = 0;
        
        do {
            /* Run batch of experiments */
            sum_X = 0;
            sum_X2 = 0;
            for (t = 0; t < tnbatch; ++t) {
                double X = run_trial(&mt);
                sum_X  += X;
                sum_X2 += X*X;
            }
            
            /* Update global counts and test for termination */
#pragma omp critical 
            {
                done_flag = (done_flag || 
                             is_converged(all_sum_X, all_sum_X2, all_ntrials,
                                          rtol, maxtrials));
                all_sum_X  += sum_X;
                all_sum_X2 += sum_X2;
                all_ntrials += tnbatch;
                done_flag = (done_flag ||
                             is_converged(all_sum_X, all_sum_X2, all_ntrials,
                                          rtol, maxtrials));
            }
            
        } while (!done_flag);
    }

    t2 = omp_get_wtime();

    /* Compute expected value and 1 sigma error bars */
    EX   = all_sum_X / all_ntrials;
    EX2  = all_sum_X2 / all_ntrials;
    stdX = sqrt((EX2-EX*EX) / all_ntrials);
    
    /* Output value, error bar, and elapsed time */
    t_elapsed = t2-t1;
    print_params();
    printf("%d threads (OpenMP): %g (%g): %e s, %ld trials\n", 
           nthreads, EX, stdX, t_elapsed, all_ntrials);

    return 0;
}