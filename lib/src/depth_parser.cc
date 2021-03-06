//  ********************************************************************
//  This file is part of Portcullis.
//
//  Portcullis is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  Portcullis is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with Portcullis.  If not, see <http://www.gnu.org/licenses/>.
//  *******************************************************************

#include <memory>
#include <sstream>
#include <string>
#include <vector>
using std::make_shared;
using std::shared_ptr;
using std::string;
using std::vector;
using std::stringstream;

#include <boost/exception/all.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>
using boost::filesystem::exists;
using boost::filesystem::path;
using boost::lexical_cast;

#include <htslib/faidx.h>
#include <htslib/sam.h>

#include <portcullis/bam/bam_alignment.hpp>
#include <portcullis/bam/bam_reader.hpp>
using portcullis::bam::BamAlignment;
using portcullis::bam::BamAlignmentPtr;
using portcullis::bam::BamReader;

#include <portcullis/bam/depth_parser.hpp>


// ******* Depth parser methods ********

int portcullis::bam::DepthParser::read_bam(void *data, bam1_t *b) {
	aux_t *aux = (aux_t*)data; // data in fact is a pointer to an auxiliary structure
	int ret = aux->iter ? BamReader::bam_iter_read(aux->fp, aux->iter, b) : bam_read1(aux->fp, b);
	if (!(b->core.flag & BAM_FUNMAP)) {
		if ((int)b->core.qual < aux->min_mapQ) b->core.flag |= BAM_FUNMAP;
		else if (aux->min_len && bam_cigar2qlen(b->core.n_cigar, bam_get_cigar(b)) < aux->min_len) b->core.flag |= BAM_FUNMAP;
	}
	return ret;
}

int portcullis::bam::DepthParser::read_bam_skip_gapped(void *data, bam1_t *b) { // read level filters better go here to avoid pileup
	aux_t *aux = (aux_t*)data; // data in fact is a pointer to an auxiliary structure
	int ret = 0;
	bool skip = false;
	do {
		skip = false;
		ret = aux->iter ? BamReader::bam_iter_read(aux->fp, aux->iter, b) : bam_read1(aux->fp, b);
		uint32_t *cigar = bam_get_cigar(b);
		for (int k = 0; k < b->core.n_cigar; ++k) {
			int cop = cigar[k] & BAM_CIGAR_MASK; // operation
			if (cop == BAM_CREF_SKIP) {
				skip = true;
				break;
			}
		}
		if (!(b->core.flag & BAM_FUNMAP)) {
			if ((int)b->core.qual < aux->min_mapQ) b->core.flag |= BAM_FUNMAP;
			else if (aux->min_len && bam_cigar2qlen(b->core.n_cigar, cigar) < aux->min_len) b->core.flag |= BAM_FUNMAP;
		}
	}
	while (skip);
	return ret;
}


portcullis::bam::DepthParser::DepthParser(path _bamFile, uint8_t _strandSpecific, bool _allowGappedAlignments) :
	bamFile(_bamFile), strandSpecific(_strandSpecific), allowGappedAlignments(_allowGappedAlignments) {
	data = (aux_t**)calloc(1, sizeof(aux_t**));
	data[0] = (aux_t*)calloc(1, sizeof(aux_t));
	data[0]->fp = bgzf_open(bamFile.c_str(), "r");
	data[0]->min_mapQ = 0;
	data[0]->min_len  = 0;
	header = bam_hdr_read(data[0]->fp);
	mplp = allowGappedAlignments ?
		   bam_mplp_init(1, read_bam, (void**)data) :
		   bam_mplp_init(1, read_bam_skip_gapped, (void**)data);
	res = 0;
	start = true;
}

portcullis::bam::DepthParser::~DepthParser() {
	bam_mplp_destroy(mplp);
	bam_hdr_destroy(header);
	bgzf_close(data[0]->fp);
	if (data[0]->iter) {
		bam_itr_destroy(data[0]->iter);
	}
	free(data);
}



bool portcullis::bam::DepthParser::loadNextBatch(vector<uint32_t>& depths) {
	if (res == 0 && !start) {
		return false;
	}
	depths.clear();
	int pos = 0;
	int tid = -1;
	int n_plp = 0; // n_plp is the number of covering reads from the i-th BAM
	// the core multi-pileup loop
	const bam_pileup1_t** plp = (const bam_pileup1_t**)calloc(1, sizeof(void*)); // plp points to the array of covering reads (internal in mplp)
	if (start) {
		start = false;
		if ((res = bam_mplp_auto(mplp, &tid, &pos, &n_plp, plp)) > 0) {
			int m = 0;
			for (int j = 0; j < n_plp; ++j) {
				const bam_pileup1_t *p = plp[0] + j;
				if (p->is_del || p->is_refskip) ++m;
			}
			last.ref = tid;
			last.pos = pos + 1;
			last.depth = n_plp - m;
		}
		else {
			free(plp);
			return false;
		}
	}
	// Create the vector
	depths.resize(header->target_len[last.ref], 0);
	// Use the details from the last run
	depths[last.pos] = last.depth;
	while ((res = bam_mplp_auto(mplp, &tid, &pos, &n_plp, plp)) > 0) {
		int m = 0;
		for (int j = 0; j < n_plp; ++j) {
			const bam_pileup1_t *p = plp[0] + j;
			if (p->is_del || p->is_refskip) ++m;
		}
		int32_t rpos = pos + 1;
		uint32_t cnt = n_plp - m;
		if (last.ref == tid) {
			// Set the depth
			depths[rpos] = cnt;
		}
		else {
			last.ref = tid;
			last.pos = rpos;
			last.depth = cnt;
			break;
		}
	}
	free(plp);
	return true;
}



