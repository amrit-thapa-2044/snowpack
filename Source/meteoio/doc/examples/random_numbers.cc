// SPDX-License-Identifier: LGPL-3.0-or-later
/***********************************************************************************/
/*  Copyright 2018 Michael Reisecker                                               */
/***********************************************************************************/
/* This file is part of MeteoIO.
    MeteoIO is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MeteoIO is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with MeteoIO.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Showcase program for MeteoIO's random numbers generator.
 */

#include <ctime>
#include <inttypes.h>
#include <iomanip> //for std::setprecision()
#include <iostream>
#include <vector>

#include <meteoio/meteoStats/RandomNumberGenerator.h>

int main(int /*argc*/, char** /*argv*/)
{
	////random integer:
	//    mio::RandomNumberGenerator RNG;
	//    int n = RNG.int64();
	////random double with Gauss distribution:
	//    RNG.setDistribution(mio::RandomNumberGenerator::RNG_GAUSS)
	//    double r = RNG.doub();

	std::cout << "--- MeteoIO random number generator suite showcase" << std::endl;
	mio::RandomNumberGenerator RNG; //init generator with all default values

	//print 1st seed that was generated by the RNG:
	if (RNG.getHardwareSeedSuccess() == true) 
		std::cout << "--- Grabbed 64 bit hardware noise for seeding" << std::endl;
	else
		std::cout << "--- Had to resort to 64 bit time mixer for seeding (Windows?)" << std::endl;
	std::vector<uint64_t> vec_seed;
	RNG.getState(vec_seed);
	std::cout << "SEED: " << vec_seed.at(0) << std::endl;
	
	std::cout << "--- Using Xor, Shift and Multiply generator" << std::endl;	
	//generate two 64 bit integers:
	const uint64_t na = RNG.int64();
	const uint64_t nb = RNG.int64();
	std::cout << "XOR int64: " << na << ", " << nb << std::endl;
	std::cout << std::setprecision(16);
	//easiest call:
	std::cout << "XOR double: " << RNG.draw() << std::endl; //always uniform
	std::cout << std::endl;

	//create other generator type and seed it with those int64s:
	std::cout << "--- Initializing Permuted linear congruential generator" << std::endl;
	mio::RandomNumberGenerator PCG(mio::RandomNumberGenerator::RNG_PCG);
	vec_seed.clear();
	vec_seed.push_back(na);
	vec_seed.push_back(nb);
	PCG.setState(vec_seed);
	const uint32_t np = PCG.int32();
	std::cout << "PCG int32: " << np << std::endl;
	std::cout << std::endl;

	double mu = 5.; //set mean and standard deviation like this
	double sigma = 2.;
	RNG.setDistribution(mio::RandomNumberGenerator::RNG_GAUSS); //only used for doubles!
	RNG.setDistributionParameter("mean", mu);
	RNG.setDistributionParameter("sigma", sigma);

	double mu_read = RNG.getDistributionParameter("mean");
	double sigma_read = RNG.getDistributionParameter("sigma");
	std::cout << "--- Drawing a Gaussian distribution (mu=" << mu_read << ", s=" << sigma_read << ")" << std::endl; 
	const unsigned int NN = 1e4; //draw this many numbers
	const unsigned int OO = 100; //limit output
	unsigned int hist[10] = {0};
	for (size_t i = 0; i < NN; ++i) {
 		const double rg = RNG.doub();
		if ( (rg >= 0.) && (rg < 10.) ) ++hist[(unsigned int)rg];
	}
	for (size_t i = 0; i < 10; ++i) {
		std::cout << i << "-" << ((i+1) % 10) << ": ";
		std::cout << std::string(hist[i] * OO/NN, '*') << std::endl;
	}
	std::cout << std::endl;

	std::cout << "--- Picking another random number with this distribution" << std::endl;
	const double rd = RNG.doub();
	std::cout << "Drew: " << rd << std::endl;
	std::cout << std::setprecision(4) << "Probability to hit a number close to this one: "
		  << RNG.pdf(rd)*100 << " %" << std::endl;
	std::cout << "Probability to hit below this number: " << RNG.cdf(rd)*100 << " %" << std::endl;
	std::cout << std::endl << std::setprecision(16);

	std::cout << "--- Switching back to XOR generator" << std::endl;
	const unsigned int nmax = 1e7;
	std::cout << "--- Discarding 10 million random 64 bit numbers..." << std::endl;
	const std::clock_t start_time = std::clock();
	for (size_t i = 0; i < nmax; ++i)
		RNG.int64();
	const double duration = (std::clock() - start_time) / (double)CLOCKS_PER_SEC;
	std::cout << "(This took " << (int)(duration*100)/100. << "s)" << std::endl;
	std::cout << std::endl;
	
	std::cout << "--- Saving this state and transferring it" << std::endl;
	std::vector<uint64_t> out_seed;
	RNG.getState(out_seed);
	std::cout << "Generator A draws: " << RNG.int64() << std::endl;
	mio::RandomNumberGenerator RN2;
	RN2.setState(out_seed);
	std::cout << "Generator B draws: " << RN2.int64() << std::endl;
	std::cout << "(Should be the same)" << std::endl;
	std::cout << std::endl;

	//get doubles between 0 and 1 with boundaries:
	std::cout << "Interval [0, 1): " << RN2.doub(mio::RandomNumberGenerator::RNG_AINCBEXC) << std::endl;
	std::cout << "Unrounded double in [0, 1]: " << RN2.doub(mio::RandomNumberGenerator::RNG_AINCBINC, true) << std::endl;
	uint32_t rt = 0.;
	const bool true_range_success = RN2.trueRange32(100, 3000, rt); //50/50 chance with these params
	std::cout << "Uniform in range [100, 3000]: " << rt << " (strictly uniform? "
	          << (true_range_success? "yes" : "no") << ")" << std::endl;
	std::cout << std::endl;
	
	std::cout << "--- Some info about the generator" << std::endl;
	std::cout << RN2.toString();
	std::cout << std::endl;
	
	std::cout << "--- Done" << std::endl;

	return 0;
}

/* SAMPLE OUTPUT
--- MeteoIO random number generator suite showcase
--- Grabbed 64 bit hardware noise for seeding
SEED: 5520393934181262797
--- Using Xor, Shift and Multiply generator
XOR int64: 3991435975534660133, 13774088105010089468
XOR double: 0.5910076849060738

--- Initializing Permuted linear congruential generator
PCG int32: 733097987

--- Drawing a Gaussian distribution (mu=5, s=2)
0-1: *
1-2: ****
2-3: *********
3-4: ***************
4-5: *******************
5-6: *******************
6-7: **************
7-8: *********
8-9: ****
9-0: *

--- Picking another random number with this distribution
Drew: 2.788120466118931
Probability to hit a number close to this one: 10.82 %
Probability to hit below this number: 13.44 %

--- Switching back to XOR generator
--- Discarding 10 million random 64 bit numbers...
(This took 0.72s)

--- Saving this state and transferring it
Generator A draws: 3839671297151779116
Generator B draws: 3839671297151779116
(Should be the same)

Interval [0, 1): 0.3668081657647495
Unrounded double in [0, 1]: 0.4480409555115005
Uniform in range [100, 3000]: 2152 (strictly uniform? yes)

--- Some info about the generator
Name: RNG_XOR
Family: Xor, shift, multiply
Size: 64 bit
Period: ~3.138*10^57
Hardware seeded: yes
Distribution: uniform
*/
