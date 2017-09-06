/***********************************************************************************/
/*  Copyright 2012 WSL Institute for Snow and Avalanche Research    SLF-DAVOS      */
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
#include <meteoio/meteoFilters/ProcUndercatch_Hamon.h>
#include <meteoio/meteoLaws/Atmosphere.h>
#include <cmath>

using namespace std;

namespace mio {

ProcUndercatch_Hamon::ProcUndercatch_Hamon(const std::vector<std::string>& vec_args, const std::string& name)
                     : ProcessingBlock(name), type(sh)
{
	parse_args(vec_args);
	properties.stage = ProcessingProperties::first; //for the rest: default values
}

void ProcUndercatch_Hamon::process(const unsigned int& param, const std::vector<MeteoData>& ivec,
                        std::vector<MeteoData>& ovec)
{
	if (param!=MeteoData::PSUM)
		throw InvalidArgumentException("Trying to use "+getName()+" filter on " + MeteoData::getParameterName(param) + " but it can only be applied to precipitation!!" + getName(), AT);
	ovec = ivec;

	for (size_t ii=0; ii<ovec.size(); ii++){
		double& tmp = ovec[ii](param);
		double VW = ovec[ii](MeteoData::VW);
		if (VW==IOUtils::nodata) continue; //we MUST have wind speed in order to filter
		VW = Atmosphere::windLogProfile(VW, 10., 2.); //impact seems minimal
		double t = ovec[ii](MeteoData::TA);
		if (t==IOUtils::nodata) continue; //we MUST have air temperature in order to filter
		t = IOUtils::K_TO_C(t); //t in celsius
		double k=0.;

		if (tmp == IOUtils::nodata || tmp==0.) {
			continue; //preserve nodata values and no precip or purely liquid precip
		} else if (type==unsh) {
			if (t>1.67) k=0.0146;
			else if (t>0.) k=0.0294;
			else if (t>-5.) k=0.0527;
			else k=0.0889;
		} else if (type==sh) {
			if (t>1.67) k=0.0060;
			else if (t>0.) k=0.0121;
			else if (t>-5.) k=0.0217;
			else k=0.0366;
		} else if (type==hellmannsh) {
			if (t>1.2) k=0.;
			else if (t>0.) k=0.0294;
			else if (t>-5.) k=0.0527;
			else k=0.0889;
		}

		tmp *= exp(k*VW);
	}
}

void ProcUndercatch_Hamon::parse_args(std::vector<std::string> filter_args)
{
	if (filter_args.size()!=1)
		throw InvalidArgumentException("Wrong number of arguments for filter " + getName() + ", please provide the rain gauge type!", AT);

	for (size_t ii=0; ii<filter_args.size(); ii++) {
		IOUtils::toLower(filter_args[ii]);
	}

	if (filter_args[0]=="sh") {
		type=sh;
	} else if (filter_args[0]=="unsh") {
		type=unsh;
	} else if (filter_args[0]=="hellmannsh") {
		type=hellmannsh;
	} else {
		throw InvalidArgumentException("Rain gauge type \""+ filter_args[0] +"\" unknown for filter "+getName(), AT);
	}
}

} //end namespace
