/***********************************************************************************/
/*  Copyright 2014 WSL Institute for Snow and Avalanche Research    SLF-DAVOS      */
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
#include <meteoio/plugins/CosmoXMLIO.h>
#include <meteoio/IOUtils.h>
#include <meteoio/FileUtils.h>
#include <meteoio/IOExceptions.h>
#include <meteoio/meteoLaws/Atmosphere.h>

#include <sstream>

#include <libxml/parserInternals.h>
#include <libxml/xpathInternals.h>
#if !defined(LIBXML_XPATH_ENABLED) || !defined(LIBXML_SAX1_ENABLED)
	#error Please enable XPATH and SAX1 in your version of libxml!
#endif

#ifdef _MSC_VER
	#pragma warning(disable:4512) //we don't need any = operator!
#endif

using namespace std;

namespace mio {
/**
 * @page cosmoxml COSMOXML
 * @section cosmoxml_format Format
 * This plugin reads the XML files as generated by <A HREF="http://www.cosmo-model.org/">Cosmo</A>'s <A HREF="http://www.cosmo-model.org/content/support/software/default.htm#fieldextra">FieldExtra</A>.
 * The files are written out by COSMO in Grib format and preprocessed by FieldExtra (MeteoSwiss) to get XML files.
 * It requires <A HREF="http://xmlsoft.org/">libxml2</A> to compile and run. It also assumes that the station IDs are unique
 * (ie two data sets with the same station ID are considered to belong to the same station).
 *
 * @section cosmoxml_cosmo_partners COSMO Group
 * This plugin has been developed primarily for reading XML files produced by COSMO (http://www.cosmo-model.org/) at MeteoSwiss.
 * COSMO (COnsortium for Small scale MOdelling) represents a non-hydrostatic limited-area atmospheric model, to be used both for operational and for research applications by the members of the consortium. The Consortium has the following members:
 *  - Germany, DWD, Deutscher Wetterdienst
 *  - Switzerland, MCH, MeteoSchweiz
 *  - Italy, USAM, Ufficio Generale Spazio Aereo e Meteorologia
 *  - Greece, HNMS, Hellenic National Meteorological Service
 *  - Poland, IMGW, Institute of Meteorology and Water Management
 *  - Romania, NMA, National Meteorological Administration
 *  - Russia, RHM, Federal Service for Hydrometeorology and Environmental Monitoring
 *  - Germany, AGeoBw, Amt für GeoInformationswesen der Bundeswehr
 *  - Italy, CIRA, Centro Italiano Ricerche Aerospaziali
 *  - Italy, ARPA-SIMC, ARPA Emilia Romagna Servizio Idro Meteo Clima
 *  - Italy, ARPA Piemonte, Agenzia Regionale per la Protezione Ambientale Piemonte
 *
 * @section cosmoxml_units Units
 * The units are assumed to be the following:
 * - temperatures in K
 * - relative humidity in %
 * - wind speed in m/s
 * - precipitations in mm/h
 * - radiation in W/m²
 * - snow height in cm
 * - maximal wind speed in m/s
 *
 * @section cosmoxml_keywords Keywords
 * This plugin uses the following keywords:
 * - COORDSYS:  input coordinate system (see Coords) specified in the [Input] section
 * - METEO:     specify COSMOXML for [Input] section
 * - METEOPATH: string containing the path to the xml files to be read, specified in the [Input] section
 * - METEOFILE: specify the xml file to read the data from (optional)
 * - METEO_PREFIX: file name prefix appearing before the date (optional)
 * - METEO_EXT: file extension (default: ".xml", give "none" to get an empty string)
 * - STATION#: ID of the station to read
 * - IMIS_STATIONS: if set to true, all station IDs provided above will be stripped of their number (to match MeteoCH naming scheme)
 * - USE_MODEL_LOC: if set to false, the true station location (lat, lon, altitude) is used. Otherwise, it uses the model location (default)
 * - XML_ENCODING: force the input file encoding, overriding the file's own encoding declaration (optional, see \ref cosmoxml_encoding "XML encoding" below)
 *
 * If no METEOFILE is provided, all ".xml" files in the METEOPATH directory will be read, if they match the METEO_PREFIX and METEO_EXT.
 * They <i>must</i> contain the date of the first data formatted as ISO8601 numerical UTC date in their file name. For example, a file containing simulated
 * meteorological fields from 2014-03-03T12:00 until 2014-03-05T00:00 could be named such as "cosmo_201403031200.xml"
 * If some numbers appear <i>before</i> the numerical date, they must be provided as part of METEO_PREFIX so the plugin can
 * properly extract the date (for MeteoSwiss, this must be set to "VNMH49").
 *
 * Example:
 * @code
 * [Input]
 * COORDSYS	= CH1903
 * METEO	= COSMOXML
 * METEOPATH	= ./input/meteoXMLdata
 * METEOFILE	= cosmo2.xml
 * STATION1	= ATT
 * STATION2	= EGH
 * @endcode
 *
 * @subsection cosmoxml_encoding XML encoding
 * Each XML document should specify its encoding. However this information might sometimes be missing or even worst, be false. This makes the XML document non-compliant.
 * Normally, COSMOXML reads the file encoding in the file itself. If this does not work (one of the two cases given above), it is possible to force the
 * encoding of the input file by using the "XML_ENCODING" option. This option takes one of the following values
 * ("LE" stands for "Little Endian" and "BE" for "Big Endian"):
 *  - for UTF/UCS: UTF-8, UTF-16-LE, UTF-16-BE, UCS-4-LE, UCS-4-BE, UCS-4-2143, UCS-4-3412, UCS-2, EBCDIC
 *  - for ISO-8859: ISO-8859-1, ISO-8859-2, ISO-8859-3, ISO-8859-4, ISO-8859-5, ISO-8859-6, ISO-8859-7, ISO-8859-8, ISO-8859-9
 *  - for Japanses: ISO-2022-JP, SHIFT-JIS, EUC-JP
 *  - for ascii: ASCII
 *
 */

const double CosmoXMLIO::in_tz = 0.; //Plugin specific timezone
const xmlChar* CosmoXMLIO::xml_attribute = (const xmlChar *)"id";
const xmlChar* CosmoXMLIO::xml_namespace = (const xmlChar *)"http://www.meteoswiss.ch/xmlns/modeltemplate/2";
const xmlChar* CosmoXMLIO::xml_namespace_abrev = (const xmlChar*)"ns";
const char* CosmoXMLIO::StationData_xpath = "//ns:datainformation/ns:data-tables/ns:data/ns:row/ns:col";
const char* CosmoXMLIO::MeteoData_xpath = "//ns:valueinformation/ns:values-tables/ns:data/ns:row/ns:col";

CosmoXMLIO::CosmoXMLIO(const std::string& configfile)
           : cache_meteo_files(), xml_stations_id(), input_id(),
             meteo_prefix(), meteo_ext(".xml"), plugin_nodata(-999.), imis_stations(false), use_model_loc(true), in_doc(NULL), in_ctxt(NULL), in_xpathCtx(NULL),
             in_encoding(XML_CHAR_ENCODING_NONE), coordin(), coordinparam()
{
	Config cfg(configfile);
	init(cfg);
}

CosmoXMLIO::CosmoXMLIO(const Config& cfg)
           : cache_meteo_files(), xml_stations_id(), input_id(),
             meteo_prefix(), meteo_ext(".xml"), plugin_nodata(-999.), imis_stations(false), use_model_loc(true), in_doc(NULL), in_ctxt(NULL), in_xpathCtx(NULL),
             in_encoding(XML_CHAR_ENCODING_NONE), coordin(), coordinparam()
{
	init(cfg);
}

void CosmoXMLIO::init(const Config& cfg)
{
	LIBXML_TEST_VERSION //check lib versions and call xmlInitParser()

	std::string coordout, coordoutparam;
	IOUtils::getProjectionParameters(cfg, coordin, coordinparam, coordout, coordoutparam);

	cfg.getValues("STATION", "INPUT", input_id);
	cfg.getValue("IMIS_STATIONS", "INPUT", imis_stations, IOUtils::nothrow);
	cfg.getValue("USE_MODEL_LOC", "INPUT", use_model_loc, IOUtils::nothrow);

	const std::string meteopath = cfg.get("METEOPATH", "INPUT");
	const std::string meteofile = cfg.get("METEOFILE", "INPUT", "");
	cfg.getValue("METEO_PREFIX", "INPUT", meteo_prefix, IOUtils::nothrow);
	cfg.getValue("METEO_EXT", "INPUT", meteo_ext, IOUtils::nothrow);
	if ( IOUtils::strToUpper(meteo_ext)=="NONE" ) meteo_ext="";

	//input encoding forcing
	const std::string tmp = cfg.get("XML_ENCODING", "INPUT", "");
	if (!tmp.empty()) {
		if (tmp=="UTF-8") in_encoding=XML_CHAR_ENCODING_UTF8;
		else if (tmp=="UTF-16-LE") in_encoding=XML_CHAR_ENCODING_UTF16LE;
		else if (tmp=="UTF-16-BE") in_encoding=XML_CHAR_ENCODING_UTF16BE;
		else if (tmp=="UCS-4-LE") in_encoding=XML_CHAR_ENCODING_UCS4LE;
		else if (tmp=="UCS-4-BE") in_encoding=XML_CHAR_ENCODING_UCS4BE;
		else if (tmp=="EBCDIC") in_encoding=XML_CHAR_ENCODING_EBCDIC;
		else if (tmp=="UCS-4-2143") in_encoding=XML_CHAR_ENCODING_UCS4_2143;
		else if (tmp=="UCS-4-3412") in_encoding=XML_CHAR_ENCODING_UCS4_3412;
		else if (tmp=="UCS-2") in_encoding=XML_CHAR_ENCODING_UCS2;
		else if (tmp=="ISO-8859-1") in_encoding=XML_CHAR_ENCODING_8859_1;
		else if (tmp=="ISO-8859-2") in_encoding=XML_CHAR_ENCODING_8859_2;
		else if (tmp=="ISO-8859-3") in_encoding=XML_CHAR_ENCODING_8859_3;
		else if (tmp=="ISO-8859-4") in_encoding=XML_CHAR_ENCODING_8859_4;
		else if (tmp=="ISO-8859-5") in_encoding=XML_CHAR_ENCODING_8859_5;
		else if (tmp=="ISO-8859-6") in_encoding=XML_CHAR_ENCODING_8859_6;
		else if (tmp=="ISO-8859-7") in_encoding=XML_CHAR_ENCODING_8859_7;
		else if (tmp=="ISO-8859-8") in_encoding=XML_CHAR_ENCODING_8859_8;
		else if (tmp=="ISO-8859-9") in_encoding=XML_CHAR_ENCODING_8859_9;
		else if (tmp=="ISO-2022-JP") in_encoding=XML_CHAR_ENCODING_2022_JP;
		else if (tmp=="SHIFT-JIS") in_encoding=XML_CHAR_ENCODING_SHIFT_JIS;
		else if (tmp=="EUC-JP") in_encoding=XML_CHAR_ENCODING_EUC_JP;
		else if (tmp=="ASCII") in_encoding=XML_CHAR_ENCODING_ASCII;
		else
			throw InvalidArgumentException("Encoding \""+tmp+"\" is not supported!", AT);
	}

	if (!meteofile.empty()) {
		const std::string file_and_path( meteopath + "/" + meteofile );
		const std::pair<Date,std::string> tmp_pair(Date(), file_and_path);
		cache_meteo_files.push_back( tmp_pair );
	} else {
		scanMeteoPath(meteopath, cache_meteo_files);
	}
}

CosmoXMLIO& CosmoXMLIO::operator=(const CosmoXMLIO& source) {
	if (this != &source) {
		cache_meteo_files = source.cache_meteo_files;
		xml_stations_id = source.xml_stations_id;
		input_id = source.input_id;
		plugin_nodata = source.plugin_nodata;
		in_doc = NULL;
		in_ctxt = NULL;
		in_xpathCtx = NULL;
		coordin = source.coordin;
		coordinparam = source.coordinparam;
	}
	return *this;
}

CosmoXMLIO::~CosmoXMLIO() throw()
{
	closeIn_XML();
}

void CosmoXMLIO::scanMeteoPath(const std::string& meteopath_in,  std::vector< std::pair<mio::Date,std::string> > &meteo_files) const
{
	meteo_files.clear();
	std::list<std::string> dirlist = FileUtils::readDirectory(meteopath_in, meteo_ext);
	dirlist.sort();

	//Check date in every filename and cache it
	const size_t prefix_len = meteo_prefix.size();
	std::list<std::string>::const_iterator it = dirlist.begin();
	while ((it != dirlist.end())) {
		const std::string& filename( *it );
		const std::string::size_type prefix_pos = (prefix_len==0)? 0 : filename.find_first_of(meteo_prefix);
		if (prefix_pos==string::npos) continue;

		const size_t start_pos = prefix_pos+prefix_len;
		if (start_pos>=filename.size()) continue;

		const std::string::size_type date_pos = filename.find_first_of("0123456789", start_pos);
		Date date;
		IOUtils::convertString(date, filename.substr(date_pos,10), in_tz);
		const std::pair<Date,std::string> tmp(date, meteopath_in+"/"+filename);

		meteo_files.push_back(tmp);
		it++;
	}
}

void CosmoXMLIO::openIn_XML(const std::string& in_meteofile)
{
	if (in_doc!=NULL) return; //the file has already been read

	xmlInitParser();
	xmlKeepBlanksDefault(0);

	if (!FileUtils::fileExists(in_meteofile)) throw AccessException(in_meteofile, AT); //prevent invalid filenames
	
	if (in_encoding==XML_CHAR_ENCODING_NONE) {
		in_doc = xmlParseFile(in_meteofile.c_str());
	} else {
		in_ctxt = xmlCreateFileParserCtxt( in_meteofile.c_str() );
		xmlSwitchEncoding( in_ctxt, in_encoding);
		xmlParseDocument( in_ctxt);
		in_doc = in_ctxt->myDoc;
	}

	if (in_doc == NULL) {
		throw NotFoundException("Could not open/parse file \""+in_meteofile+"\"", AT);
	}

	if (in_xpathCtx != NULL) xmlXPathFreeContext(in_xpathCtx); //free variable if this was not freed before
	in_xpathCtx = xmlXPathNewContext(in_doc);
	if (in_xpathCtx == NULL) {
		closeIn_XML();
		throw IOException("Unable to create new XPath context", AT);
	}

	if (xmlXPathRegisterNs(in_xpathCtx,  xml_namespace_abrev, xml_namespace) != 0) {
		throw IOException("Unable to register namespace with prefix", AT);
	}
}

void CosmoXMLIO::closeIn_XML() throw()
{
	if (in_xpathCtx!=NULL) {
		xmlXPathFreeContext(in_xpathCtx);
		in_xpathCtx = NULL;
	}
	if (in_doc!=NULL) {
		xmlFreeDoc(in_doc);
		in_doc = NULL;
	}
	if (in_ctxt!=NULL) {
		xmlFreeParserCtxt(in_ctxt);
		in_ctxt = NULL;
	}
	xmlCleanupParser();
}

bool CosmoXMLIO::parseStationData(const std::string& station_id, const xmlXPathContextPtr& xpathCtx, StationData &sd)
{
	//match something like "//ns:valueinformation/ns:values-tables/ns:data/ns:row/ns:col[@id='station_abbreviation' and text()='ATT']/.."
	//the namespace "ns" has been previously defined
	const std::string xpath_id = (imis_stations)? station_id.substr(0, station_id.find_first_of("0123456789")) : station_id;
	const std::string xpath( std::string(StationData_xpath)+"[@id='station_abbreviation' and text()='"+xpath_id+"']/.." ); //and we take the parent node <row>

	xmlXPathObjectPtr xpathObj( xmlXPathEvalExpression((const xmlChar*)xpath.c_str(), xpathCtx) );
	if (xpathObj == NULL) return false;

	//check the number of matches
	const xmlNodeSetPtr &metadata = xpathObj->nodesetval;
	const int nr_metadata = (metadata) ? metadata->nodeNr : 0;
	if (nr_metadata==0)
		throw NoDataException("No metadata found for station \""+station_id+"\"", AT);
	if (nr_metadata>1)
		throw InvalidFormatException("Multiple definition of metadata for station \""+station_id+"\"", AT);

	//collect all the data fields
	std::string xml_id;
	double altitude = IOUtils::nodata, latitude = IOUtils::nodata, longitude = IOUtils::nodata;
	//start from the first child until the last one
	for (xmlNode *cur_node = metadata->nodeTab[0]->children; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE) {
			xmlChar *att = xmlGetProp(cur_node, xml_attribute);
			const std::string field( (const char*)(att) );
			xmlFree(att);

			if (cur_node->children->type == XML_TEXT_NODE) {
				const std::string value( (const char*)(cur_node->children->content) );

				if (field=="identifier") xml_id = value;
				//else if (field=="station_abbreviation") sd.stationID = value;
				else if (field=="station_name") sd.stationName = value;
				else if (field=="missing_value_code") IOUtils::convertString(plugin_nodata, value);

				if (use_model_loc) {
					if (field=="station_height") IOUtils::convertString(altitude, value);
					else if (field=="station_latitude") IOUtils::convertString(latitude, value);
					else if (field=="station_longitude") IOUtils::convertString(longitude, value);
				} else {
					if (field=="model_station_height") IOUtils::convertString(altitude, value);
					else if (field=="model_station_latitude") IOUtils::convertString(latitude, value);
					else if (field=="model_station_longitude") IOUtils::convertString(longitude, value);
				}
			}
		}
	}

	sd.stationID = station_id;

	if (latitude==IOUtils::nodata || longitude==IOUtils::nodata || altitude==IOUtils::nodata)
		throw NoDataException("Some station location information is missing for station \""+station_id+"\"", AT);
	sd.position.setProj(coordin, coordinparam);
	sd.position.setLatLon(latitude, longitude, altitude);

	if (xml_id.empty()) throw NoDataException("XML station id missing for station \""+station_id+"\"", AT);
	xml_stations_id[station_id] = xml_id;

	xmlXPathFreeObject(xpathObj);
	return true;
}

CosmoXMLIO::MeteoReadStatus CosmoXMLIO::parseMeteoDataPoint(const Date& dateStart, const Date& dateEnd, const xmlNodePtr &element, MeteoData &md) const
{
	double iswr_dir = IOUtils::nodata, iswr_diff = IOUtils::nodata;

	//collect all the data fields
	for (xmlNode *cur_node = element; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE) {
			xmlChar *att = xmlGetProp(cur_node, xml_attribute);
			const std::string field( (const char*)(att) );
			xmlFree(att);

			if (cur_node->children->type == XML_TEXT_NODE) {
				const std::string value( (const char*)(cur_node->children->content) );
				if (field=="reference_ts") {
					IOUtils::convertString(md.date, value, in_tz);
					if (md.date<dateStart) return read_continue;
					if (md.date>dateEnd) return read_stop;
				} else {
					double tmp;
					IOUtils::convertString(tmp, value);
					tmp = IOUtils::standardizeNodata(tmp, plugin_nodata);

					//for now, we hard-code the fields mapping
					if (field=="108005") md(MeteoData::TA) = tmp;
					else if (field=="108014") md(MeteoData::RH) = tmp/100.;
					else if (field=="108015") md(MeteoData::VW) = tmp;
					else if (field=="108017") md(MeteoData::DW) = tmp;
					else if (field=="108018") md(MeteoData::VW_MAX) = tmp;
					else if (field=="108023") md(MeteoData::PSUM) = tmp;
					else if (field=="108060") md(MeteoData::HS) = tmp/100.;
					else if (field=="108062") md(MeteoData::TSS) = tmp;
					else if (field=="108064") iswr_diff = tmp;
					else if (field=="108065") iswr_dir = tmp;
					else if (field=="108066") md(MeteoData::RSWR) = tmp;
					else if (field=="108067") md(MeteoData::ILWR) = tmp; //108068=olwr
				}
			}
		}
	}

	if (iswr_diff!=IOUtils::nodata && iswr_dir!=IOUtils::nodata)
		md(MeteoData::ISWR) = iswr_diff+iswr_dir;

	//because of the Kalman filter applied on VW, sometimes VW_MAX<VW
	if (md(MeteoData::VW)!=IOUtils::nodata && md(MeteoData::VW_MAX)!=IOUtils::nodata && md(MeteoData::VW_MAX)<md(MeteoData::VW))
		md(MeteoData::VW_MAX) = md(MeteoData::VW);

	return read_ok;
}

size_t CosmoXMLIO::getFileIdx(const Date& start_date) const
{
	if (cache_meteo_files.empty())
		throw InvalidArgumentException("No input files found or configured!", AT);

	//find which file we should open
	if (cache_meteo_files.size()==1) {
		return 0;
	} else {
		for (size_t idx=1; idx<cache_meteo_files.size(); idx++) {
			if (start_date>=cache_meteo_files[idx-1].first && start_date<cache_meteo_files[idx].first) {
				return --idx;
			}
		}

		//not found, we take the closest timestamp we have
		if (start_date<cache_meteo_files.front().first)
			return 0;
		else
			return cache_meteo_files.size()-1;
	}
}

void CosmoXMLIO::readStationData(const Date& station_date, std::vector<StationData>& vecStation)
{
	vecStation.clear();
	
	const std::string meteofile( cache_meteo_files[ getFileIdx(station_date) ].second );
	openIn_XML(meteofile);

	//read all the stations' metadata
	for (size_t ii=0; ii<input_id.size(); ii++) {
		StationData sd;
		if (!parseStationData(input_id[ii], in_xpathCtx, sd)) {
			closeIn_XML();
			throw IOException("Unable to evaluate xpath expression for station \""+input_id[ii]+"\"", AT);
		}
		vecStation.push_back(sd);
	}

	closeIn_XML();
}

bool CosmoXMLIO::parseMeteoData(const Date& dateStart, const Date& dateEnd, const std::string& station_id, const StationData& sd, const xmlXPathContextPtr& xpathCtx, std::vector<MeteoData> &vecMeteo) const
{
	const std::string xpath( std::string(MeteoData_xpath)+"[@id='identifier' and text()='"+station_id+"']" );

	xmlXPathObjectPtr xpathObj( xmlXPathEvalExpression((const xmlChar*)xpath.c_str(), xpathCtx) );
	if (xpathObj == NULL) return false;

	//check the number of matches
	const xmlNodeSetPtr &data = xpathObj->nodesetval;
	const int nr_data = (data) ? data->nodeNr : 0;
	if (nr_data==0)
		throw NoDataException("No data found for station \""+station_id+"\"", AT);

	//loop over all data for this station_id
	for (int ii=0; ii<nr_data; ii++) {
		MeteoData md( Date(), sd);

		const MeteoReadStatus status = parseMeteoDataPoint(dateStart, dateEnd, data->nodeTab[ii], md);
		if (status==read_stop) break;
		if (status==read_ok) vecMeteo.push_back( md );
	}

	xmlXPathFreeObject(xpathObj);
	return true;
}

void CosmoXMLIO::readMeteoData(const Date& dateStart, const Date& dateEnd,
                               std::vector< std::vector<MeteoData> >& vecMeteo)
{
	vecMeteo.clear();
	const size_t nr_files = cache_meteo_files.size();
	size_t file_idx = getFileIdx(dateStart);
	Date nextDate;
	
	do {
		//since files contain overlapping data, we will only read the non-overlapping part
		//ie from start to the start date of the next file
		nextDate = ((file_idx+1)<nr_files)? cache_meteo_files[file_idx+1].first - 1./3600. : dateEnd;

		const std::string meteofile( cache_meteo_files[file_idx].second );
		openIn_XML(meteofile);

		//read all the stations' metadata
		std::vector<StationData> vecStation;
		for (size_t ii=0; ii<input_id.size(); ii++) {
			StationData sd;
			if (!parseStationData(input_id[ii], in_xpathCtx, sd)) {
				closeIn_XML();
				throw IOException("Unable to evaluate xpath expression for station \""+input_id[ii]+"\"", AT);
			}
			vecStation.push_back(sd);
		}

		//read all the stations' data
		for (size_t ii=0; ii<input_id.size(); ii++) {
			const std::string station_id( xml_stations_id[ input_id[ii] ] );

			//do we already have a vector with this station meteo?
			size_t found_id = IOUtils::npos;
			for (size_t jj=0; jj<vecMeteo.size(); jj++) {
				if (vecMeteo[jj].front().meta.stationID==input_id[ii]) {
					found_id = jj;
					break;
				}
			}

			if (found_id==IOUtils::npos) { //creating the station
				vector<MeteoData> vecTmp;
				if (!parseMeteoData(dateStart, nextDate, station_id, vecStation[ii], in_xpathCtx, vecTmp)) {
					closeIn_XML();
					throw IOException("Unable to evaluate xpath expression for station \""+input_id[ii]+"\"", AT);
				}
				vecMeteo.push_back(vecTmp);
			} else { //appending to the station
				if (!parseMeteoData(dateStart, nextDate, station_id, vecStation[ii], in_xpathCtx, vecMeteo[found_id])) {
					closeIn_XML();
					throw IOException("Unable to evaluate xpath expression for station \""+input_id[ii]+"\"", AT);
				}
			}
		}

		closeIn_XML();

		file_idx++;
	} while (file_idx<nr_files && nextDate<=dateEnd);
}

} //namespace

