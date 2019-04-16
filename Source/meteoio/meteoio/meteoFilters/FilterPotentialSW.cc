/***********************************************************************************/
/*  Copyright 2009 WSL Institute for Snow and Avalanche Research    SLF-DAVOS      */
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
#include <meteoio/meteoFilters/FilterPotentialSW.h>
#include <meteoio/meteoLaws/Sun.h>

using namespace std;

namespace mio {

FilterPotentialSW::FilterPotentialSW(const std::vector< std::pair<std::string, std::string> >& vecArgs, const std::string& name)
          : ProcessingBlock(vecArgs, name), min_coeff(0.03), max_coeff(1.1), is_soft(false), use_toa(true)
{
	parse_args(vecArgs);
	properties.stage = ProcessingProperties::both; //for the rest: default values
}

void FilterPotentialSW::process(const unsigned int& param, const std::vector<MeteoData>& ivec,
                        std::vector<MeteoData>& ovec)
{
	ovec = ivec;
	if (ovec.empty()) return;

	SunObject Sun;
	for (size_t ii=0; ii<ovec.size(); ii++) { //now correct all timesteps
		double& value = ovec[ii](param);
		if (value == IOUtils::nodata) continue; //preserve nodata values

		const Coords position( ovec[ii].meta.position );
		Sun.setLatLon(position.getLat(), position.getLon(), position.getAltitude()); //if they are constant, nothing will be recomputed
		Sun.setDate(ovec[ii].date.getJulian(true), 0.); //quicker: we stick to gmt

		double albedo = 1.; //needed if we are dealing with RSWR
		if (param==MeteoData::RSWR) {
			const double HS = ovec[ii](MeteoData::HS);
			if (HS!=IOUtils::nodata) //no big deal if we can not adapt the albedo
				albedo = (HS>=snow_thresh)? snow_albedo : soil_albedo;
			else
				albedo = 0.5;
		}

		//if we don't have TA and RH, set them so the reduced precipitable water will get an average value
		double TA = ovec[ii](MeteoData::TA);
		double RH = ovec[ii](MeteoData::RH);
		const double P = ovec[ii](MeteoData::P);
		if (TA==IOUtils::nodata || RH==IOUtils::nodata) {
			TA = 274.98;
			RH = 0.666;
		}

		Sun.calculateRadiation(TA, RH, P, albedo);

		double toa_h, direct_h, diffuse_h;
		Sun.getHorizontalRadiation(toa_h, direct_h, diffuse_h);

		if (use_toa && (value/albedo<min_coeff*toa_h)) //top of atmosphere comparison
			value = is_soft? min_coeff*toa_h*albedo : IOUtils::nodata;
		else if (value/albedo<min_coeff*(direct_h+diffuse_h)) //ground comparison
			value = is_soft? min_coeff*(direct_h+diffuse_h)*albedo : IOUtils::nodata;
		else if (value/albedo>max_coeff*(direct_h+diffuse_h)) //for ISWR, albedo==1
			value = is_soft? max_coeff*(direct_h+diffuse_h)*albedo : IOUtils::nodata;

	}
}

void FilterPotentialSW::parse_args(const std::vector< std::pair<std::string, std::string> >& vecArgs)
{
	const std::string where( "Filters::"+block_name );
	std::string run_mode("");
	for (size_t ii=0; ii<vecArgs.size(); ii++) {
		if (vecArgs[ii].first=="MAX_COEFF") {
			IOUtils::parseArg(vecArgs[ii], where, max_coeff);
		} else if (vecArgs[ii].first=="MIN_COEFF") {
			IOUtils::parseArg(vecArgs[ii], where, min_coeff);
		} else if (vecArgs[ii].first=="SOFT") {
			IOUtils::parseArg(vecArgs[ii], where, is_soft);
		} else if (vecArgs[ii].first=="MODE") {
			IOUtils::parseArg(vecArgs[ii], where, run_mode);
			if (run_mode=="GROUND")
				use_toa = false;
		}
	}

}

} //end namespace
