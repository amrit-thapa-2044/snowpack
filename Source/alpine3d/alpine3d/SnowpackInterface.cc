/***********************************************************************************/
/*  Copyright 2009-2015 WSL Institute for Snow and Avalanche Research    SLF-DAVOS      */
/***********************************************************************************/
/* This file is part of Alpine3D.
    Alpine3D is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Alpine3D is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with Alpine3D.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <alpine3d/SnowpackInterface.h>
#include <alpine3d/AlpineMain.h>
#include <alpine3d/MPIControl.h>

#include <errno.h>
#include <algorithm>

using namespace std;
using namespace mio;

//sort the by increasing y and increasing x as a second key
inline bool pair_comparator(const std::pair<double, double>& l, const std::pair<double, double>& r)
{
	if (l.second == r.second)
		return l.first < r.first;

	return l.second < r.second;
}

//convert the POI to grid index representation and warn of duplicates
inline void prepare_pts(const std::vector<Coords>& vec_pts, std::vector< std::pair<size_t,size_t> > &pts)
{
	pts.clear();
	std::vector<size_t> vec_idx;
	for (size_t ii=0; ii<vec_pts.size(); ii++) {
		const size_t ix = vec_pts[ii].getGridI();
		const size_t iy = vec_pts[ii].getGridJ();
		std::pair<size_t,size_t> tmp(ix,iy);
		
		const std::vector< std::pair<size_t,size_t> >::const_iterator it = std::find(pts.begin(), pts.end(), tmp);
		if (it != pts.end()) {
			if (MPIControl::instance().master()) {
				const size_t orig_idx = vec_idx[ it - pts.begin() ];
				std::cout << "[W] POI #" << ii << " " << vec_pts[ii].toString(Coords::CARTESIAN);
				std::cout << " is a duplicate of POI #" <<  orig_idx << " " << vec_pts[orig_idx].toString(Coords::CARTESIAN) << std::endl; //flush for openmp
			}
		} else {
			pts.push_back( tmp );
			vec_idx.push_back( ii ); //in order to be able to find out where a duplicate is originating from
		}
	}
	sort(pts.begin(), pts.end(), pair_comparator);
}

/**
 * @brief Constructs and initialise Snowpack Interface Master. He creates the Worker and
 * Distributes the Data from the other modules to the Worker. Is the acces interface A3D side
 * @param io_cfg is used to init Runoff and to create IOManager, which is used to write the standart output
 * @param nbworkers gives the new values for the wind speed
 * @param dem_in gives the demographic Data. Also tetermines size and position of the geographical modeling scope
 * @param landuse_in gives the landuse Data. Also tetermines size and position of the landuse for modeling scope
 * @param vec_pts gives the spezial points. For this points more output is done then for the others. Calcualtion is the same.
 * @param startTime is the time and date the first simulation step is done
 * @param grids_requirements list of grids that must be prepared for other modules (similar to the Output::GRIDS_PARAMETERS configuration key)
 * @param is_restart_in is used to know which files the worker needs to read to init the pixel
 */
SnowpackInterface::SnowpackInterface(const mio::Config& io_cfg, const size_t& nbworkers,
                                     const mio::DEMObject& dem_in,
                                     const mio::Grid2DObject& landuse_in,
                                     const std::vector<mio::Coords>& vec_pts,
                                     const mio::Date &startTime,
                                     const std::string& grids_requirements,
                                     const bool is_restart_in)
                : run_info(), asciiIO(io_cfg, run_info), smetIO(io_cfg, run_info), dimx(dem_in.getNx()), dimy(dem_in.getNy()), landuse(landuse_in),
                  mns(dem_in, IOUtils::nodata), shortwave(dem_in, IOUtils::nodata), longwave(dem_in, IOUtils::nodata), diffuse(dem_in, IOUtils::nodata),
                  psum(dem_in, IOUtils::nodata), psum_ph(dem_in, IOUtils::nodata), vw(dem_in, IOUtils::nodata), rh(dem_in, IOUtils::nodata), ta(dem_in, IOUtils::nodata),
                  solarElevation(0.), output_grids(),
                  workers(nbworkers), worker_startx(nbworkers), worker_deltax(nbworkers), timer(), nextStepTimestamp(startTime), timeStep(dt_main/86400.),
                  drift(NULL), eb(NULL), da(NULL), runoff(NULL),
                  dataMeteo2D(false), dataDa(false), dataSnowDrift(false), dataRadiation(false),
                  io(io_cfg), outpath(), mask_glaciers(false), mask_dynamic(false), maskGlacier(),
                  glacier_katabatic_flow(false), glaciers(NULL),
                  sn_cfg(io_cfg), dem(dem_in), is_restart(is_restart_in), useCanopy(false), do_io_locally(true), station_name(),
                  soil_temp_depth(IOUtils::nodata), grids_start(0), grids_days_between(0), ts_start(0.), ts_days_between(0.), prof_start(0.), prof_days_between(0.), 
                  grids_write(true), ts_write(false), prof_write(false), snow_write(false), snow_poi_written(false),
                  meteo_outpath(), tz_out(0.), pts()
{
	MPIControl& mpicontrol = MPIControl::instance();
	
	readAndTweakConfig(io_cfg);
	prepare_pts(vec_pts, pts); //convert POI to index representation, sort by y asc, then x asc
	
	std::vector<SnowStation*> snow_stations( readInitalSnowCover());

	if (mpicontrol.master()) {
		std::cout << "[i] SnowpackInterface initializing a total of " << mpicontrol.size();
		if (mpicontrol.size()>1) std::cout << " processes with " << nbworkers;
		else std::cout << " process with " << nbworkers;
		if (nbworkers>1) std::cout << " workers";
		else  std::cout << " worker";
		std::cout << " each using Snowpack " << snowpack::getLibVersion() << "\n";
	}
	
	//create and prepare  the vector of output grids
	if (grids_write) {
		sn_cfg.getValue("GRIDS_PARAMETERS", "output", output_grids);
		uniqueOutputGrids(output_grids);
	}
	//add the grids that are necessary for the other modules
	const std::string all_grids = sn_cfg.get("GRIDS_PARAMETERS", "output", IOUtils::nothrow);
	sn_cfg.addKey("GRIDS_PARAMETERS", "output", all_grids + " " + grids_requirements + " " + getGridsRequirements()); //also consider own requirements
	
	//If MPI is active, every node gets a slice of the DEM to work on
	size_t startx = 0, nx = dimx;
	mpicontrol.getArraySliceParams(dimx, startx, nx);
	
	// construct slices and workers
	#pragma omp parallel for schedule(static)
	for (size_t ii=0; ii<nbworkers; ii++) {
		size_t thread_startx, thread_nx;
		MPIControl::getArraySliceParams(nx, nbworkers, ii, thread_startx, thread_nx);
		const size_t offset = /*startx +*/ thread_startx;

		if (thread_nx>0) {
			const size_t endx = thread_nx+offset-1;

			// handle special points
			const size_t n_pts = pts.size();
			std::vector< std::pair<size_t,size_t> > sub_pts;
			for (size_t kk=0; kk<n_pts; kk++) { // could be optimised... but not really big gain
				if (pts[kk].first>=offset && pts[kk].first<=endx) {
					sub_pts.push_back( pts[kk] );
					sub_pts.back().first -= offset;
				}
			}

			// generate slices
			const DEMObject sub_dem(dem_in, offset, 0, thread_nx, dimy, false);
			const Grid2DObject sub_landuse(landuse_in, offset, 0, thread_nx, dimy);

			// generate workers
			std::vector<SnowStation*> thread_stations;
			for (size_t iy=0; iy<dimy; iy++) {
				//build SnowStation as row major so all threads have their data in cache 
				//(because they all more or less work on the same line at the same time)
				const size_t idx_offset = iy*nx + offset;
				thread_stations.insert (thread_stations.end(), snow_stations.begin()+idx_offset, snow_stations.begin()+idx_offset+thread_nx);
			}
			workers[ii] = new SnowpackInterfaceWorker(sn_cfg, sub_dem, sub_landuse, sub_pts, thread_stations, offset);
			worker_startx[ii] = offset;
			worker_deltax[ii] = thread_nx;

			#pragma omp critical(snowpackWorkers_status)
			std::cout << "[i] SnowpackInterface worker " << ii << " on process " << mpicontrol.rank() << ": X range = [" << offset << "-" << endx << "] \t " << thread_nx << " cells\n";
		}
	}

	// init glacier map (after creating and init workers) for output
	if (mask_glaciers || glacier_katabatic_flow) {
		maskGlacier = getGrid(SnGrids::GLACIER);
		if (glacier_katabatic_flow) {
			glaciers = new Glaciers(io_cfg, dem);
			glaciers->setGlacierMap(maskGlacier);
		}
	}
}

SnowpackInterface& SnowpackInterface::operator=(const SnowpackInterface& source) {
	if (this != &source) {
		run_info = source.run_info;
		asciiIO = source.asciiIO;
		smetIO = source.smetIO;
		dimx = source.dimx;
		dimy = source.dimy;
		landuse = source.landuse;
		mns = source.mns;
		shortwave = source.shortwave;
		longwave = source.longwave;
		diffuse = source.diffuse;
		psum = source.psum;
		psum_ph = source.psum_ph;
		vw = source.vw;
		rh = source.rh;
		ta = source.ta;
		solarElevation = source.solarElevation;
		output_grids = source.output_grids;
		workers = source.workers;
		worker_startx = source.worker_startx;
		worker_deltax = source.worker_deltax;
		timer = source.timer;
		nextStepTimestamp = source.nextStepTimestamp;
		timeStep = source.timeStep;

		drift = source.drift;
		eb = source.eb;
		da = source.da;
		runoff = source.runoff;
		dataMeteo2D = source.dataMeteo2D;
		dataDa = source.dataDa;
		dataSnowDrift = source.dataSnowDrift;
		dataRadiation = source.dataRadiation;

		//io = source.io;

		outpath = source.outpath;
		mask_glaciers = source.mask_glaciers;
		mask_dynamic = source.mask_dynamic;
		maskGlacier = source.maskGlacier;

		glacier_katabatic_flow = source.glacier_katabatic_flow;
		glaciers = source.glaciers;

		sn_cfg = source.sn_cfg;
		//dem = source.dem;
		is_restart = source.is_restart;
		useCanopy = source.useCanopy;
		do_io_locally = source.do_io_locally;
		station_name = source.station_name;

		soil_temp_depth = source.soil_temp_depth;
		grids_start = source.grids_start;
		grids_days_between = source.grids_days_between;
		ts_start = source.ts_start;
		ts_days_between = source.ts_days_between;
		prof_start = source.prof_start;
		prof_days_between = source.prof_days_between;
		grids_write = source.grids_write;
		ts_write = source.ts_write;
		prof_write = source.prof_write;
		snow_write = source.snow_write;
		snow_poi_written = source.snow_poi_written;
		meteo_outpath = source.meteo_outpath;
		tz_out = source.tz_out;
		pts = source.pts;
	}
	return *this;
}

std::string SnowpackInterface::getGridsRequirements() const
{
	if (glacier_katabatic_flow) {
		return "GLACIER TSS HS";
	}
	return "";
}

/** @brief Make sure all requested grids only appear once
 * @param output_grids vector of requeste grids to sort and filter
 */
void SnowpackInterface::uniqueOutputGrids(std::vector<std::string>& output_grids)
{
	for (size_t ii = 0; ii<output_grids.size(); ++ii) 
		IOUtils::toUpper(output_grids[ii]);
	
	std::sort (output_grids.begin(), output_grids.end()); 
	const std::vector<std::string>::iterator it = std::unique (output_grids.begin(), output_grids.end());
	output_grids.resize( std::distance(output_grids.begin(),it) );
}

void SnowpackInterface::readAndTweakConfig(const mio::Config& io_cfg)
{
	//force some keys
	double calculation_step_length;
	sn_cfg.getValue("CALCULATION_STEP_LENGTH", "Snowpack", calculation_step_length);
	std::stringstream ss;
	ss << calculation_step_length;
	sn_cfg.addKey("METEO_STEP_LENGTH", "Snowpack", ss.str());
	sn_cfg.addKey("ALPINE3D", "SnowpackAdvanced", "true");
	sn_cfg.addKey("PERP_TO_SLOPE", "SnowpackAdvanced", "true");
	
	std::string adjust_wind = io_cfg.get("ADJUST_HEIGHT_OF_WIND_VALUE", "SnowpackAdvanced", IOUtils::nothrow);
	if (adjust_wind.empty()) adjust_wind = "true";
	sn_cfg.addKey("ADJUST_HEIGHT_OF_WIND_VALUE", "SnowpackAdvanced", adjust_wind);
	/*string adjust_meteo= io_cfg.get("ADJUST_HEIGHT_METEO_VALUE", "SnowpackAdvanced", IOUtils::nothrow);
	if (adjust_meteo.empty()) adjust_meteo = "true";
	sn_cfg.addKey("ADJUST_HEIGHT_METEO_VALUE", "SnowpackAdvanced", adjust_meteo);*/

	io_cfg.getValue("LOCAL_IO", "General", do_io_locally, IOUtils::nothrow);
	sn_cfg.getValue("GRID2DPATH", "Output", outpath);
	io_cfg.getValue("MASK_GLACIERS", "Output", mask_glaciers, IOUtils::nothrow);
	io_cfg.getValue("MASK_DYNAMIC", "Output", mask_dynamic, IOUtils::nothrow);
	io_cfg.getValue("GLACIER_KATABATIC_FLOW", "Snowpack", glacier_katabatic_flow, IOUtils::nothrow);
	io_cfg.getValue("SOIL_TEMPERATURE_DEPTH", "Output", soil_temp_depth, IOUtils::nothrow);
	
	sn_cfg.getValue("GRIDS_WRITE", "Output", grids_write);
	sn_cfg.getValue("GRIDS_START", "Output", grids_start);
	sn_cfg.getValue("GRIDS_DAYS_BETWEEN", "Output", grids_days_between);
	sn_cfg.getValue("TS_WRITE", "Output", ts_write);
	sn_cfg.getValue("TS_START", "Output", ts_start);
	sn_cfg.getValue("TS_DAYS_BETWEEN", "Output", ts_days_between);
	sn_cfg.getValue("PROF_WRITE", "Output", prof_write);
	sn_cfg.getValue("PROF_START", "Output", prof_start);
	sn_cfg.getValue("PROF_DAYS_BETWEEN", "Output", prof_days_between);
	
	sn_cfg.getValue("METEOPATH", "Output", meteo_outpath);
	sn_cfg.getValue("TIME_ZONE", "Output", tz_out, IOUtils::nothrow);
	sn_cfg.getValue("EXPERIMENT", "Output", station_name, IOUtils::dothrow);
	
	sn_cfg.getValue("SNOW_WRITE", "Output", snow_write);
	sn_cfg.getValue("CANOPY", "Snowpack", useCanopy);
}

/**
 * @brief Destructor of SnowpackInterface Master. Handels special cases with POP-C++ and
 * also free correctly runoff and workers.
 */
SnowpackInterface::~SnowpackInterface()
{
	if (glacier_katabatic_flow) delete glaciers;
	//if (runoff) delete runoff;
	while (!workers.empty()) delete workers.back(), workers.pop_back();
}


/**
 * @brief get time who was used to exchange Data with Workers and run on each Pixel
 * the Snowpack Model throught workers.
 */
double SnowpackInterface::getTiming() const
{
	return timer.getElapsed();
}


/**
 * @brief Internal in Snowpack Interface Master used method to write standard
 * results in output files.
 * Attantion: to have old format files output, set in ini file following key:
 * **A3D_VIEW	= true**
 * 
 * @param date is the date for which the output is done
 */
void SnowpackInterface::writeOutput(const mio::Date& date)
{
	MPIControl& mpicontrol = MPIControl::instance();
	const bool isMaster = mpicontrol.master();
	
	if (do_grid_output(date)) {
		//no OpenMP pragma here, otherwise multiple threads might call an MPI allreduce_sum()
		for (size_t ii=0; ii<output_grids.size(); ii++) {
			const size_t SnGrids_idx = SnGrids::getParameterIndex( output_grids[ii] );
			mio::Grid2DObject grid( getGrid( static_cast<SnGrids::Parameters>(SnGrids_idx)) );
			
			if (isMaster) {
				if (mask_glaciers) grid *= maskGlacier;
				const size_t meteoGrids_idx = MeteoGrids::getParameterIndex( output_grids[ii] );
				if (meteoGrids_idx!=IOUtils::npos) { //for this, the grid plugins must be thread-safe!
					io.write2DGrid(grid, static_cast<MeteoGrids::Parameters>(meteoGrids_idx), date);
				} else {
					const std::string name( date.toString(Date::NUM) + "_" + output_grids[ii] + ".asc" );
					io.write2DGrid(grid, name);
				}
			}
		}
	}
	
	// Output Runoff: at each time step
	if (isMaster) {
		if (runoff) runoff->output(date, psum, ta);
	}
}

/**
 * @brief Method tells if on given date, gridded output should be done (read this out of snowpack ini)
 * @param date is the date object which is controlled, if output needs to be done
 */
bool SnowpackInterface::do_grid_output(const mio::Date &date) const
{
	return (grids_write && booleanTime(date.getJulian(), grids_days_between, grids_start, dt_main/60.));
}

/**
 * @brief commands worker to write .sno files. Is triggered by Alpine Control
 *
 * Hack --> Find better software architecture to do this then SnowpackInterface also
 * does output for other modules of A3D here..
 * @param date is the date witch the output is done
 */
void SnowpackInterface::writeOutputSNO(const mio::Date& date)
{
	MPIControl& mpicontrol = MPIControl::instance();

	vector<SnowStation*> snow_station;

	for (size_t ii=0; ii<workers.size(); ii++)
		workers[ii]->getOutputSNO(snow_station);
	
	if (mpicontrol.master()) {
		std::cout << "[i] Writing SNO output for process " << mpicontrol.master_rank() << "\n";
		writeSnowCover(date, snow_station); //local data

		//Now gather all elements on the master node
		for (size_t ii=0; ii<mpicontrol.size(); ii++) {
			if (ii == mpicontrol.master_rank() || do_io_locally) continue;
			std::cout << "[i] Writing SNO output for process " << ii << "\n";
			vector<SnowStation*> snow_station_tmp;

			mpicontrol.receive(snow_station_tmp, ii);
			writeSnowCover(date, snow_station_tmp);
			for (size_t jj=0; jj<snow_station_tmp.size(); jj++) delete snow_station_tmp[jj];
		}
	} else {
		if (do_io_locally) {
			std::cout << "[i] Writing SNO output for process " << mpicontrol.rank() << "\n";
			writeSnowCover(date, snow_station); //local data
		} else {
			mpicontrol.send(snow_station, mpicontrol.master_rank());
		}
	}
}

void SnowpackInterface::writeSnowCover(const mio::Date& date, const std::vector<SnowStation*>& snow_station)
{
	for (size_t jj=0; jj<snow_station.size(); jj++)
		smetIO.writeSnowCover(date, *(snow_station[jj]), ZwischenData());
}

/* %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
   Methods to set references to other methodes
  %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%% */
/**
 * @brief Set reference to SnowDrift module, to comunicate with it.
 * @param mydrift is the reference to the SnowDrift module
 */
void SnowpackInterface::setSnowDrift(SnowDriftA3D& mydrift)
{
	drift = &mydrift;

	if (drift) {
		for (size_t i = 0; i < workers.size(); i++) workers[i]->setUseDrift(true);

		// Provide initial snow parameters to SnowDrift
		const Grid2DObject cH( getGrid(SnGrids::HS) );
		const Grid2DObject sp( getGrid(SnGrids::SP) );
		const Grid2DObject rg( getGrid(SnGrids::RG) );
		const Grid2DObject N3( getGrid(SnGrids::N3) );
		const Grid2DObject rb( getGrid(SnGrids::RB) );
		drift->setSnowSurfaceData(cH, sp, rg, N3, rb);
	}
}

/**
 * @brief Set reference to EnergyBalance module, to comunicate with it.
 * @param myeb is the reference to the EnergyBalance module
 */
void SnowpackInterface::setEnergyBalance(EnergyBalance& myeb)
{
	eb = &myeb;

	if (eb) {
		for (size_t i = 0; i < workers.size(); i++) workers[i]->setUseEBalance(true);

		// Provide initial albedo to EnergyBalance
		const Grid2DObject alb( getGrid(SnGrids::TOP_ALB) );
		eb->setAlbedo(alb);
	}
}

/**
 * @brief Set reference to DataAssimilation module, to comunicate with it.
 * @param init_da is the reference to the DataAssimilation module
 */
void SnowpackInterface::setDataAssimilation(DataAssimilation& init_da)
{
	da = &init_da;
}

void SnowpackInterface::setRunoff(Runoff& init_runoff)
{
	runoff = &init_runoff;
}

/**
 * @brief Interface that DataAssimilation can push the data to the SnowpackInterface
 * This is currently never used...
 * @param daData are the data from the DataAssimilation module
 * @param timestamp for controlle if data from DataAssimilation are form the correct simulation step
 */
void SnowpackInterface::assimilate(const Grid2DObject& /*daData*/, const mio::Date& timestamp)
{

	if (nextStepTimestamp != timestamp) {
		throw InvalidArgumentException("Assimilation and snowpack time steps don't match", AT);
	}

	cout << "Updating state variables...\n";
	/*for (size_t iy = 0; iy < dimy; iy++) {
		for (size_t ix = 0; ix < dimx; ix++) {
			const size_t i = dimy*iy + ix;
			if ( daData.grid2D(ix,iy) == 1.0 ) {
				//if DA-data DOES NOT have snow
				if (sn_Xdata[i].cH-sn_Xdata[i].Ground > 0.) {
					//and snowpack DOES have snow
					//clear the pixel
					store(ix,iy) = 0.;
					sn_Xdata[i].cH = sn_Xdata[i].Ground;
					sn_Xdata[i].mH = sn_Xdata[i].Ground;
					sn_Xdata[i].nElems = sn_Xdata[i].SoilNode;
					sn_Xdata[i].nNodes = sn_Xdata[i].nElems + 1;
				}
			} else {
				if ( daData.grid2D(ix,iy) == 4 ) {
					//if DA-data DOES have snow
					if (sn_Xdata[i].cH-sn_Xdata[i].Ground < 1e-15) {
						//if snowpack DOES NOT have snow -> add some snow
						store(ix,iy) += 40*M_TO_H(calculation_step_length);
					}
				}
			}
		}
	}*/

	dataDa = true;
	calcNextStep();
}

/**
 * @brief Interface that SnowDrift can push the data to the SnowpackInterface
 * @param new_mns are the data about the new Snow masses from the DataAssimilation module
 * @param timestamp for controlle if data from DataAssimilation are form the correct simulation step
 */
void SnowpackInterface::setSnowMassChange(const mio::Grid2DObject& new_mns, const mio::Date& timestamp)
{
	if (nextStepTimestamp != timestamp) {
		if (MPIControl::instance().master()) {
			std::cerr << "Providing drift snow mass at " << timestamp.toString(Date::ISO);
			std::cerr << " for Snowpack timestamp " << nextStepTimestamp.toString(Date::ISO) << "\n";
		}
		throw InvalidArgumentException("Snowdrift and snowpack steps don't match", AT);
	}

	if (!new_mns.isSameGeolocalization(dem)) {
		std::ostringstream ss;
		ss << "Trying to set snow mass changes from a (" << new_mns.getNx() << "," << new_mns.getNy() << ") grid ";
		ss << "when the dem is (" << dem.getNx() << "," << dem.getNy() << ")";
		throw IndexOutOfBoundsException(ss.str(), AT);
	}
	
	mns = new_mns;
	dataSnowDrift = true;
	calcNextStep();
}

/**
 * @brief get Meteo changes from AlpineControl or SnowDrift module
 * @param new_psum gives the new values for new Water High
 * @param new_psum_ph gives the new values for the precipitation phase
 * @param new_vw gives the new values for the wind speed
 * @param new_rh gives the new values for the realtiv Humidity
 * @param new_ta gives the new values for the Aire temperature
 * @param timestamp is the time of the calculation step from which this new values are comming
 */
void SnowpackInterface::setMeteo(const Grid2DObject& new_psum, const Grid2DObject& new_psum_ph, const Grid2DObject& new_vw, const Grid2DObject& new_rh, const Grid2DObject& new_ta, const mio::Date& timestamp)
{
	if (nextStepTimestamp != timestamp) {
		if (MPIControl::instance().master()) {
			std::cerr << "Providing meteo fields at " << timestamp.toString(Date::ISO);
			std::cerr << " for Snowpack timestamp " << nextStepTimestamp.toString(Date::ISO) << "\n";
		}
		throw InvalidArgumentException("Meteo and snowpack time steps don't match", AT);
	}

	psum = new_psum;
	psum_ph = new_psum_ph;
	vw = new_vw;
	rh = new_rh;
	if (mask_dynamic) maskGlacier = getGrid(SnGrids::GLACIER); //so the updated glacier map is available for all
	
	if (!glacier_katabatic_flow) {
		ta = new_ta;
	} else {
		if (mask_dynamic) glaciers->setGlacierMap(maskGlacier);
		const Grid2DObject TSS( getGrid(SnGrids::TSS) );
		const Grid2DObject cH( getGrid(SnGrids::HS) );
		ta = glaciers->correctTemperatures(cH, TSS, new_ta);
	}
	
	dataMeteo2D = true;
	calcNextStep();
}

/**
 * @brief get values from Energy Balance
 * @param shortwave_in is the 2D double Array map of ISWR [W m-2]
 * @param longwave_in is the 2D double Array map of ILWR [W m-2]
 * @param diff_in is the 2D double Array map of Diffuse radiation from the sky [W m-2]
 * @param solarElevation_in double of Solar elevation to be used for Canopy (in dec)
 * @param timestamp is the time of the calculation step from which this new values are comming
 */
void SnowpackInterface::setRadiationComponents(const mio::Array2D<double>& shortwave_in, const mio::Array2D<double>& longwave_in, const mio::Array2D<double>& diff_in, const double& solarElevation_in, const mio::Date& timestamp)
{
	if (nextStepTimestamp != timestamp) {
		if (MPIControl::instance().master()) {
			std::cerr << "Providing radiation fields at " << timestamp.toString(Date::ISO);
			std::cerr << " for Snowpack timestamp " << nextStepTimestamp.toString(Date::ISO) << "\n";
		}
		throw InvalidArgumentException("Radiation and snowpack time steps don't match", AT);
	}

	shortwave.grid2D = shortwave_in;
	longwave.grid2D = longwave_in;
	diffuse.grid2D = diff_in;
	solarElevation = solarElevation_in;
	
	dataRadiation = true;
	calcNextStep();
}

/**
 * @brief Request specific grid by parameter type
 * @param param parameter
 * @return 2D output grid (empty if the requested parameter was not available)
 */
mio::Grid2DObject SnowpackInterface::getGrid(const SnGrids::Parameters& param)
{
	//special case for the meteo forcing grids
	switch (param) {
		case SnGrids::TA:
			return ta;
		case SnGrids::RH:
			return rh;
		case SnGrids::VW:
			return vw;
		case SnGrids::PSUM:
			return psum;
		case SnGrids::PSUM_PH:
			return psum_ph;
		case SnGrids::ISWR:
			return shortwave;
		case SnGrids::ILWR:
			return longwave;
		default: ; //so compilers do not complain about missing conditions
	}
	
	mio::Grid2DObject o_grid2D(dem, 0.); //so the allreduce_sum works
	size_t errCount = 0;
	#pragma omp parallel for schedule(dynamic) reduction(+: errCount)
	for (size_t ii = 0; ii < workers.size(); ii++) {
		const mio::Grid2DObject tmp( workers[ii]->getGrid(param) );
		if (!tmp.empty())
			o_grid2D.grid2D.fill(tmp.grid2D, worker_startx[ii], 0, worker_deltax[ii], dimy);
		else
			errCount++;
	}

	MPIControl::instance().allreduce_sum(o_grid2D);
	//with some MPI implementations, when transfering large amounts of data, the buffers might get full and lead to a crash
	
	if (errCount>0) {
		std::cerr << "[W] Requested " << SnGrids::getParameterName( param ) << " but this was not available in the workers\n";
		o_grid2D.clear(); //the requested parameter was not available
	}
	return o_grid2D;
}

/**
 * @brief Get data from other modules and run one simulation step.
 * Once the simulation step has been performed, the data are pushed to ther other modules.
 */
void SnowpackInterface::calcNextStep()
{
	//Control if all data are present
	if (!dataMeteo2D) {
		return;
	} else {
		if (drift != NULL && !dataSnowDrift) return;
		if (da != NULL && !dataDa) return;
		if (eb != NULL && !dataRadiation) return;
	}
	// control if the necessary data are available
	if (!dataRadiation) {
		throw NoDataException("Radiation data not available", AT);
	}

	dataDa = dataMeteo2D = dataSnowDrift = dataRadiation = false; //the external modules will turn them back to true when pushing their data

	// timing
	timer.restart();

	size_t errCount = 0;
	#pragma omp parallel for schedule(static) reduction(+: errCount)
	for (size_t ii = 0; ii < workers.size(); ii++) { // make slices
		const mio::Grid2DObject tmp_psum(psum, worker_startx[ii], 0, worker_deltax[ii], dimy);
		const mio::Grid2DObject tmp_psum_ph(psum_ph, worker_startx[ii], 0, worker_deltax[ii], dimy);
		const mio::Grid2DObject tmp_rh(rh, worker_startx[ii], 0, worker_deltax[ii], dimy);
		const mio::Grid2DObject tmp_ta(ta, worker_startx[ii], 0, worker_deltax[ii], dimy);
		const mio::Grid2DObject tmp_vw(vw, worker_startx[ii], 0, worker_deltax[ii], dimy);
		const mio::Grid2DObject tmp_mns(mns, worker_startx[ii], 0, worker_deltax[ii], dimy);
		const mio::Grid2DObject tmp_shortwave(shortwave, worker_startx[ii], 0, worker_deltax[ii], dimy);
		const mio::Grid2DObject tmp_diffuse(diffuse, worker_startx[ii], 0, worker_deltax[ii], dimy);
		const mio::Grid2DObject tmp_longwave(longwave, worker_startx[ii], 0, worker_deltax[ii], dimy);

		// run model, process exceptions in a way that is compatible with openmp
		try {
			workers[ii]->runModel(nextStepTimestamp, tmp_psum, tmp_psum_ph, tmp_rh, tmp_ta, tmp_vw, tmp_mns, tmp_shortwave, tmp_diffuse, tmp_longwave, solarElevation);
		} catch(const std::exception& e) {
			++errCount;
			cout << e.what() << std::endl;
			fflush( stdout );
		}
	}

	//Retrieve special points data and write files
	if (!pts.empty()) write_special_points();

	if (errCount>0)  //something wrong took place, quitting. At least we tried writing the special points out
		std::abort(); //force core dump

	// Gather data if needed and make exchange for SnowDrift
	if (drift) {
		const Grid2DObject cH( getGrid(SnGrids::HS) );
		const Grid2DObject sp( getGrid(SnGrids::SP) );
		const Grid2DObject rg( getGrid(SnGrids::RG) );
		const Grid2DObject N3( getGrid(SnGrids::N3) );
		const Grid2DObject rb( getGrid(SnGrids::RB) );
		drift->setSnowSurfaceData(cH, sp, rg, N3, rb);
	}
	
	// Gather data if needed and make exchange for EnergyBalance
	if (eb) {
		const Grid2DObject alb( getGrid(SnGrids::TOP_ALB) );
		eb->setAlbedo(alb);
	}

	//make output
	writeOutput(nextStepTimestamp);

	timer.stop();
	if (MPIControl::instance().master())
		cout << "[i] Snowpack simulations done for " << nextStepTimestamp.toString(Date::ISO) << "\n";
	nextStepTimestamp = nextStepTimestamp + timeStep;
}

void SnowpackInterface::write_special_points()
{
	MPIControl& mpicontrol = MPIControl::instance();

	std::vector<SnowStation*> snow_pixel;
	std::vector<CurrentMeteo*> meteo_pixel;
	std::vector<SurfaceFluxes*> surface_flux;

	// note: do not parallelize this with OpenMP
	for (size_t ii=0; ii<workers.size(); ii++)
		workers[ii]->getOutputSpecialPoints(snow_pixel, meteo_pixel, surface_flux);

	if (do_io_locally) {
		writeOutputSpecialPoints(nextStepTimestamp, snow_pixel, meteo_pixel, surface_flux);
		if (!snow_write && !snow_poi_written) {
			writeSnowCover(nextStepTimestamp, snow_pixel); //also write the .sno of the special points
			snow_poi_written = true;
		}
	} else { // data has to be sent to the master process
		if (mpicontrol.master()) {
			// Write out local data first and then gather data from all processes
			writeOutputSpecialPoints(nextStepTimestamp, snow_pixel, meteo_pixel, surface_flux);
			if (!snow_write && !snow_poi_written) {
				writeSnowCover(nextStepTimestamp, snow_pixel); //also write the .sno of the special points
			}

			for (size_t ii=0; ii<mpicontrol.size(); ii++) {
				if (ii == mpicontrol.master_rank()) continue;
				snow_pixel.clear(); meteo_pixel.clear(); surface_flux.clear();

				mpicontrol.receive(snow_pixel, ii);
				mpicontrol.receive(meteo_pixel, ii);
				mpicontrol.receive(surface_flux, ii);

				writeOutputSpecialPoints(nextStepTimestamp, snow_pixel, meteo_pixel, surface_flux);
				if (!snow_write && !snow_poi_written) {
					writeSnowCover(nextStepTimestamp, snow_pixel); //also write the .sno of the special points
				}
			}
			snow_poi_written = true;
		} else {
			mpicontrol.send(snow_pixel, mpicontrol.master_rank());
			mpicontrol.send(meteo_pixel, mpicontrol.master_rank());
			mpicontrol.send(surface_flux, mpicontrol.master_rank());
		}
	}

	#pragma omp parallel for schedule(static)
	for (size_t ii=0; ii<workers.size(); ii++) workers[ii]->clearSpecialPointsData();
}

/**
 * @brief Write the output which is asked to have more for the special points
 * @param date Output date
 * @param snow_pixel The SnowStation data for all the special points
 * @param meteoPixel The CurrentMeteo data for all the special points
 * @param surface_flux The SurfaceFlux data for all the special points
 */
void SnowpackInterface::writeOutputSpecialPoints(const mio::Date& date, const std::vector<SnowStation*>& snow_pixel, const std::vector<CurrentMeteo*>& meteo_pixel,
                                                 const std::vector<SurfaceFluxes*>& surface_flux)
{
	const bool TS = (ts_write && booleanTime(date.getJulian(), ts_days_between, ts_start, dt_main/60.));
	const bool PR = (prof_write && booleanTime(date.getJulian(), prof_days_between, prof_days_between, dt_main/60));

	const ProcessDat Hdata; // empty ProcessDat, get it from where ??
	for (size_t ii=0; ii<snow_pixel.size(); ii++) {
		write_SMET(*meteo_pixel[ii], snow_pixel[ii]->meta, *surface_flux[ii]);
		if (TS) asciiIO.writeTimeSeries(*snow_pixel[ii], *surface_flux[ii], *meteo_pixel[ii], Hdata, 0.);
		if (PR) asciiIO.writeProfile(date, *snow_pixel[ii]);
	}
}


/**
 * @brief Write header of SMET file for specific point
 * @param meta StationData for the SMET header initialization
 */
void SnowpackInterface::write_SMET_header(const mio::StationData& meta, const double& landuse_code) const
{
	const std::string filename( meteo_outpath + "/" + meta.stationName + ".smet" );
	if (!FileUtils::validFileAndPath(filename)) throw InvalidNameException(filename,AT);
	errno = 0;
	
	std::ofstream smet_out; //Output file streams
	smet_out.open(filename.c_str());
	if (smet_out.fail()) throw AccessException(filename.c_str(), AT);

	smet_out << "SMET 1.1 ASCII\n";
	smet_out << "[HEADER]\n";
	smet_out << "station_name = " << meta.stationName << "\n";
	smet_out << "station_id   = " << meta.stationID << "\n";
	smet_out << std::right;
	smet_out << std::fixed;

	smet_out << "altitude     = " << std::setw(11)  << std::setprecision(1) << meta.position.getAltitude() << "\n";
	smet_out << "latitude     = " << std::setw(11) << std::setprecision(8) << meta.position.getLat() << "\n";
	smet_out << "longitude    = " << std::setw(11) << std::setprecision(8) << meta.position.getLon() << "\n";
	smet_out << "easting      = " << std::setw(11) << std::setprecision(1) << meta.position.getEasting() << "\n";
	smet_out << "northing     = " << std::setw(11) << std::setprecision(1) << meta.position.getNorthing() << "\n";
	smet_out << "epsg         = " << std::setw(11) << std::setprecision(0) << meta.position.getEPSG() << "\n";
	smet_out << "slope        = " << std::setw(11)  << std::setprecision(1) << meta.getSlopeAngle() << "\n";
	smet_out << "azimuth      = " << std::setw(11)  << std::setprecision(1) << meta.getAzimuth() << "\n";
	smet_out << "landuse      = " << std::setw(11)  << std::setprecision(0) << SnowpackInterfaceWorker::round_landuse( landuse_code ) << "\n";
	smet_out << "nodata       = " << std::setw(11)  << std::setprecision(0) << mio::IOUtils::nodata << "\n";
	smet_out << "tz           = " << std::setw(11)  << std::setprecision(0) << tz_out << "\n";
	smet_out << "source       = " <<  "Alpine3D version " << A3D_VERSION << " run by " << run_info.user << "\n";
	smet_out << "creation     = " << run_info.computation_date.toString(Date::ISO) << "\n";
	if (useCanopy) smet_out << "comment      = " << "ISWR/RSWR are above the canopy, ISWR_can/RSWR_can and PSUM/PSUM_PH are below the canopy\n";

	smet_out << "fields       = timestamp TA TSS TSG VW DW VW_MAX ISWR OSWR ILWR PSUM PSUM_PH HS RH";
	if (soil_temp_depth!=IOUtils::nodata) smet_out << " TSOIL";
	if (useCanopy) smet_out << " ISWR_can RSWR_can";
	smet_out << "\n[DATA]\n";

	smet_out.close();
}

/**
 * @brief Write the meteorogical data for the current step into the SMET file for the respective point
 * @param met The CurrentMeteo data for one special point
 * @param ix is the x-coordiante of the special point
 * @param iy is the y-coordiante of the special point
 */
void SnowpackInterface::write_SMET(const CurrentMeteo& met, const mio::StationData& meta, const SurfaceFluxes& surf) const
{
	const std::string filename( meteo_outpath + "/" + meta.stationName + ".smet" );
	if (!FileUtils::validFileAndPath(filename)) throw InvalidNameException(filename,AT);
	
	std::ofstream smet_out; //Output file streams
	smet_out.open(filename.c_str(), std::ios::out | std::ios::app );
	if (smet_out.fail()) throw AccessException(filename.c_str(), AT);

	// write line
	smet_out.fill(' ');
	smet_out << std::right;
	smet_out << std::fixed;
	smet_out << met.date.toString(mio::Date::ISO) << " ";
	smet_out << std::setw(8) << std::setprecision(2) << met.ta << " ";
	smet_out << std::setw(8) << std::setprecision(2) << met.tss << " ";
	smet_out << std::setw(8) << std::setprecision(2) << met.ts0 << " ";
	smet_out << std::setw(6) << std::setprecision(1) << met.vw << " ";
	smet_out << std::setw(5) << std::setprecision(0) << met.dw << " ";
	smet_out << std::setw(6) << std::setprecision(1) << met.vw_max << " ";
	smet_out << std::setw(6) << std::setprecision(0) << met.iswr << " ";
	smet_out << std::setw(6) << std::setprecision(0) << met.rswr << " ";
	smet_out << std::setw(6) << std::setprecision(3) << mio::Atmosphere::blkBody_Radiation(met.ea, met.ta) << " ";
	smet_out << std::setw(6) << std::setprecision(3) << met.psum << " ";
	smet_out << std::setw(6) << std::setprecision(3) << met.psum_ph << " ";
	smet_out << std::setw(8) << std::setprecision(3) << met.hs / cos(meta.getSlopeAngle()*Cst::to_rad) << " ";
	smet_out << std::setw(7) << std::setprecision(3) << met.rh << " ";
	if (!met.ts.empty()) 
		smet_out << std::setw(8) << std::setprecision(2) << met.ts[0] << " ";
	if (useCanopy) {
		smet_out << std::setw(6) << std::setprecision(0) << surf.sw_in << " ";
		smet_out << std::setw(6) << std::setprecision(0) << surf.sw_out << " ";
	}
	smet_out << "\n";

	smet_out.close();
}

/**
 * @page reading_snow_files Reading initial snow cover
 * The initial snow cover consist of an instantaneous snow/soil profile from which the time evolution will be computed. 
 * When this is for a normal "cold" start, the file names are built based on the landuse code. For restarts, the 
 * file names are built based on the cell (ii,jj) indices, for example:
 * 	+ {station_name}_{landuse_code}.{ext} for a "cold" start;
 * 	+ {ii}_{jj}_{station_name}.{ext} for a restart;
 * 
 * The station name is given in the [Output] section as "EXPERIMENT" key. The other keys controlling the process (including
 * the file extension) are:
 * 	+ in the [Snowpack] section:
 * 		+ CANOPY: should the pixels enable the canopy module?
 * 		+ SNP_SOIL: should the pixels use soil layers?
 * 	+ in the [Input] section:
 * 		+ SNOW: file format of the "sno" files, either SMET or SNOOLD (default: SMET);
 * 		+ COORDSYS, COORDPARAM: in order to convert (ii,jj) coordinates to geographic coordinates so each pixel's metadata
 * can be reused (for example in order to rerun a \ref poi_outputs "Point Of Interest" offline in the SNOWPACK standalone model).
 */
std::vector<SnowStation*> SnowpackInterface::readInitalSnowCover()
{//HACK: with nextStepTimestamp, check that the snow cover is older than the start timestep!
	std::vector<SnowStation*> snow_stations;
	
	if (MPIControl::instance().master() || do_io_locally) {
		const bool useSoil = sn_cfg.get("SNP_SOIL", "Snowpack");
		std::string sno_type("SMET");
		sn_cfg.getValue("SNOW", "Input", sno_type, IOUtils::nothrow);
		const std::string coordsys = sn_cfg.get("COORDSYS", "Input");
		const std::string coordparam = sn_cfg.get("COORDPARAM", "Input", IOUtils::nothrow);
		Coords llcorner_out( dem.llcorner );
		llcorner_out.setProj(coordsys, coordparam);
		const double refX = llcorner_out.getEasting();
		const double refY = llcorner_out.getNorthing();
		const double cellsize = dem.cellsize;

		const size_t nrWorkers = MPIControl::instance().size();
		for (size_t ii=0; ii<nrWorkers; ii++) {
			if (do_io_locally && (ii != MPIControl::instance().rank())) continue; // only read/write points managed by this process
			
			SN_SNOWSOIL_DATA snow_soil;
			size_t startx, deltax;
			MPIControl::instance().getArraySliceParams(dimx, ii, startx, deltax);
			vector<SnowStation*> snow_stations_tmp;
			snow_stations_tmp.reserve( dimy*deltax );

			// read snow cover for all points that are dealt with on this process
			for (size_t iy = 0; iy < dimy; iy++) {
				for (size_t ix = startx; ix < (startx+deltax); ix++) {
					
					if (SnowpackInterfaceWorker::skipThisCell(landuse(ix,iy), dem(ix,iy))) { //skip nodata cells as well as water bodies, etc
						snow_stations_tmp.push_back( NULL );
						continue;
					}
					snow_stations_tmp.push_back( new SnowStation(useCanopy, useSoil) );

					SnowStation& snowPixel = *(snow_stations_tmp.back());
					const bool is_special_point = SnowpackInterfaceWorker::is_special(pts, ix, iy);

					// get potential filenames for initial snow pixel values
					std::stringstream LUS_sno, GRID_sno;
					LUS_sno << station_name << "_" << SnowpackInterfaceWorker::round_landuse(landuse.grid2D(ix,iy));
					GRID_sno << ix << "_" << iy << "_" << station_name;

					// read standard values of pixel
					try {
						ZwischenData zwischenData; //not used by Alpine3D but necessary for Snowpack
						readSnowCover(GRID_sno.str(), LUS_sno.str(), sno_type, is_special_point, snow_soil, zwischenData);
					} catch (exception& e) {
						cout << e.what()<<"\n";
						throw IOException("Can not read snow files", AT);
					}

					// Copy standard values to specific pixel (station) data and init it
					try {
						snowPixel.initialize(snow_soil, 0); //force sector 0
						snowPixel.mH = Constants::undefined;
					} catch (exception&) {
						cout << "[E] Could not intialize cell (" << ix << "," << iy << ")!\n";
						throw IOException("Can not initialize snow pixel", AT);
					}

					// Set proper pixel metadata
					snowPixel.meta.position.setProj(coordsys, coordparam);
					snowPixel.meta.position.setXY(refX+double(ix)*cellsize, refY+double(iy)*cellsize, dem.grid2D(ix,iy)); 
					snowPixel.meta.position.setGridIndex((int)ix, (int)iy, 0, true);
					snowPixel.meta.setSlope(dem.slope(ix,iy), dem.azi(ix,iy));
					snowPixel.cos_sl = cos( snowPixel.meta.getSlopeAngle()*mio::Cst::to_rad );

					// Initialize the station name for the pixel
					stringstream station_idx;
					station_idx << ix << "_" << iy;
					snowPixel.meta.stationName = station_idx.str() + "_" + station_name;
					snowPixel.meta.stationID = station_idx.str();
					if (is_special_point) { //create SMET files for special points
						write_SMET_header(snowPixel.meta, landuse(ix, iy));
					}
				}
			}
			if (ii == MPIControl::instance().master_rank() || do_io_locally) {
				snow_stations = snow_stations_tmp; //simply copy the pointers
			} else {
				MPIControl::instance().send(snow_stations_tmp, ii);
				while (!snow_stations_tmp.empty()) delete snow_stations_tmp.back(), snow_stations_tmp.pop_back();
			}
		}
		std::cout << "[i] Read initial snow cover for process " << MPIControl::instance().rank() << "\n";
	} else {
		MPIControl::instance().receive(snow_stations, MPIControl::instance().master_rank());
	}
	
	return snow_stations;
}

void SnowpackInterface::readSnowCover(const std::string& GRID_sno, const std::string& LUS_sno, const std::string& sno_type,
                                      const bool& is_special_point, SN_SNOWSOIL_DATA &sno, ZwischenData &zwischenData)
{
	// read standard values of pixel
	if (sno_type=="SMET") { //HACK
		if (is_special_point && !is_restart) {
			//special points can come either from LUS snow files or GRID snow files
			if (smetIO.snowCoverExists(GRID_sno, station_name)) {
				smetIO.readSnowCover(GRID_sno, station_name, sno, zwischenData);
			} else {
				smetIO.readSnowCover(LUS_sno, station_name, sno, zwischenData);
			}
		} else {
			if (is_restart) {
				smetIO.readSnowCover(GRID_sno, station_name, sno, zwischenData);
			} else {
				smetIO.readSnowCover(LUS_sno, station_name, sno, zwischenData);
			}
		}
	} else {
		if (is_special_point && !is_restart) {
			//special points can come either from LUS snow files or GRID snow files
			if (asciiIO.snowCoverExists(GRID_sno, station_name)) {
				asciiIO.readSnowCover(GRID_sno, station_name, sno, zwischenData);
			} else {
				asciiIO.readSnowCover(LUS_sno, station_name, sno, zwischenData);
			}
		} else {
			if (is_restart) {
				asciiIO.readSnowCover(GRID_sno, station_name, sno, zwischenData);
			} else {
				asciiIO.readSnowCover(LUS_sno, station_name, sno, zwischenData);
			}
		}
	}

	//check that the layers are older than the start date
	if (sno.nLayers>0 && sno.Ldata.front().depositionDate>nextStepTimestamp) {
		ostringstream ss;
		ss <<  "A layer can not be younger than the start date!";
		if (is_restart)
			ss << " Please check profile '" << GRID_sno << "'";
		else
			ss << " Please check profile '" << LUS_sno << "'";
		throw IOException(ss.str(), AT);
	}
}