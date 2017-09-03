/**********************************************************************************
 This file is part of Phase-field Accelerator Benchmarks, written by Trevor Keller
 and available from https://github.com/usnistgov/phasefield-accelerator-benchmarks.

 This software was developed at the National Institute of Standards and Technology
 by employees of the Federal Government in the course of their official duties.
 Pursuant to title 17 section 105 of the United States Code this software is not
 subject to copyright protection and is in the public domain. NIST assumes no
 responsibility whatsoever for the use of this software by other parties, and makes
 no guarantees, expressed or implied, about its quality, reliability, or any other
 characteristic. We would appreciate acknowledgement if the software is used.

 This software can be redistributed and/or modified freely provided that any
 derivative works bear some notice that they are derived from it, and any modified
 versions bear some notice that they have been modified.

 Questions/comments to Trevor Keller (trevor.keller@nist.gov)
 **********************************************************************************/

/**
 \file  tbb_discretization.cpp
 \brief Implementation of boundary condition functions with TBB threading
*/

#include <math.h>
#include <tbb/tbb.h>
#include <tbb/task_scheduler_init.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#include <tbb/blocked_range2d.h>
#include "boundaries.h"
#include "discretization.h"
#include "numerics.h"
#include "timer.h"

void compute_convolution(fp_t** conc_old, fp_t** conc_lap, fp_t** mask_lap,
                         int nx, int ny, int nm)
{
	/* Lambda function executed on each thread, solving convolution	*/
	tbb::parallel_for(tbb::blocked_range2d<int>(nm/2, nx-nm/2, nm/2, ny-nm/2),
		[=](const tbb::blocked_range2d<int>& r) {
			for (int j = r.cols().begin(); j != r.cols().end(); j++) {
				for (int i = r.rows().begin(); i != r.rows().end(); i++) {
					fp_t value = 0.0;
					for (int mj = -nm/2; mj < nm/2+1; mj++) {
						for (int mi = -nm/2; mi < nm/2+1; mi++) {
							value += mask_lap[mj+nm/2][mi+nm/2] * conc_old[j+mj][i+mi];
						}
					}
					conc_lap[j][i] = value;
				}
			}
		}
	);
}

void solve_diffusion_equation(fp_t** conc_old, fp_t** conc_new, fp_t** conc_lap,
                              fp_t** mask_lap, int nx, int ny, int nm,
                              fp_t bc[2][2], fp_t D, fp_t dt, fp_t* elapsed,
                              struct Stopwatch* sw)
{
	double start_time=0.;

	apply_boundary_conditions(conc_old, nx, ny, nm, bc);

	start_time = GetTimer();
	compute_convolution(conc_old, conc_lap, mask_lap, nx, ny, nm);
	sw->conv += GetTimer() - start_time;

	start_time = GetTimer();
	/* Lambda function executed on each thread, updating diffusion equation */
	tbb::parallel_for(tbb::blocked_range2d<int>(nm/2, nx-nm/2, nm/2, ny-nm/2),
		[=](const tbb::blocked_range2d<int>& r) {
			for (int j = r.cols().begin(); j != r.cols().end(); j++) {
				for (int i = r.rows().begin(); i != r.rows().end(); i++) {
					conc_new[j][i] = conc_old[j][i] + dt * D * conc_lap[j][i];
				}
			}
		}
	);

	*elapsed += dt;
	sw->step += GetTimer() - start_time;
}

class Reduction2D {
	/* Local pointer to \a conc_new	*/
	fp_t** my_conc;

	public:
		/**
		 Local copy of variable to be reduced (summed) over my_conc
		*/
		fp_t my_sum;

		/**
		 Initializing constructor using \a conc_new
		*/
		Reduction2D(fp_t** conc) : my_conc(conc), my_sum(0.0) {}

		/**
		 Copy constructor for dividing workload
		*/
		Reduction2D(Reduction2D& a, tbb::split)
		                      : my_conc(a.my_conc), my_sum(0.0) {}

		/**
		 Lambda function executed on each thread, summing local values
		*/
		void operator()(const tbb::blocked_range2d<int>& r)
		{
			fp_t** conc = my_conc;
			fp_t sum = my_sum;

			for (int j = r.cols().begin(); j != r.cols().end(); j++) {
				for (int i = r.rows().begin(); i != r.rows().end(); i++) {
					sum += conc[j][i];
				}
			}
			my_sum = sum;
		}

		/**
		 Parallel reduction, combining values from threads as they finish
		*/
		void join(const Reduction2D& a)
		{
			my_sum += a.my_sum;
		}
};

void check_solution(fp_t** conc_new, fp_t** conc_lap, int nx, int ny,
                    fp_t dx, fp_t dy, int nm, fp_t elapsed, fp_t D,
                    fp_t bc[2][2], fp_t* rss)
{
	/* Lambda function executed on each thread, checking local values */
	tbb::parallel_for(tbb::blocked_range2d<int>(nm/2, nx-nm/2, nm/2, ny-nm/2),
		[=](const tbb::blocked_range2d<int>& r) {
			for (int j = r.cols().begin(); j != r.cols().end(); j++) {
				for (int i = r.rows().begin(); i != r.rows().end(); i++) {
					fp_t r, cal, car, ca, cn;

					/* numerical solution */
					cn = conc_new[j][i];

					/* shortest distance to left-wall source */
					r = distance_point_to_segment(dx * (nm/2), dy * (nm/2),
					                              dx * (nm/2), dy * (ny/2),
					                              dx * i, dy * j);
					analytical_value(r, elapsed, D, bc, &cal);

					/* shortest distance to right-wall source */
					r = distance_point_to_segment(dx * (nx-1-nm/2), dy * (ny/2),
					                              dx * (nx-1-nm/2), dy * (ny-1-nm/2),
					                              dx * i, dy * j);
					analytical_value(r, elapsed, D, bc, &car);

					/* superposition of analytical solutions */
					ca = cal + car;

					/* residual sum of squares (RSS) */
					conc_lap[j][i] = (ca - cn) * (ca - cn) / (fp_t)((nx-1-nm/2) * (ny-1-nm/2));
				}
			}
		}
	);

	Reduction2D R(conc_lap);
	tbb::parallel_reduce(tbb::blocked_range2d<int>(nm/2, nx-nm/2, nm/2, ny-nm/2), R);

	*rss = R.my_sum;
}