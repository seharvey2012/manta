// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Manta
// Copyright (c) 2013 Illumina, Inc.
//
// This software is provided under the terms and conditions of the
// Illumina Open Source Software License 1.
//
// You should have received a copy of the Illumina Open Source
// Software License 1 along with this program. If not, see
// <https://github.com/downloads/sequencing/licenses/>.
//

///
/// \author Chris Saunders
///

#include "SVLocusSetFinder.hh"
#include "blt_util/align_path_bam_util.hh"

#include "boost/foreach.hpp"

#include <iostream>


SVLocusSetFinder::
SVLocusSetFinder(const ESLOptions& opt)
     : _opt(opt)
{
    // pull in insert stats:
    _rss.read(opt.statsFilename.c_str());

    // cache the insert stats we'll be looking up most often:
    BOOST_FOREACH(std::string file, opt.alignmentFilename)
    {
        const boost::optional<unsigned> index(_rss.getGroupIndex(file));
        assert(index);
        const ReadGroupStats rgs(_rss.getStats(*index));

        _stats.resize(_stats.size()+1);
        _stats.back().min=rgs.fragSize.quantile(opt.breakendEdgeTrimProb);
        _stats.back().max=rgs.fragSize.quantile((1-opt.breakendEdgeTrimProb));
    }
}



void
SVLocusSetFinder::
getChimericSVLocus(
    const CachedReadGroupStats& rstats,
    const bam_record& read,
    SVLocus& locus)
{
    ALIGNPATH::path_t apath;
    bam_cigar_to_apath(read.raw_cigar(),read.n_cigar(),apath);

    const unsigned readSize(apath_read_length(apath));

    unsigned thisReadNoninsertSize(0);
    if (read.is_fwd_strand())
    {
        thisReadNoninsertSize=(readSize-apath_read_trail_size(apath));
    }
    else
    {
        thisReadNoninsertSize=(readSize-apath_read_lead_size(apath));
    }

    // estimate mate read size to be same as this, and no clipping on mate read:
    const unsigned mateReadNoninsertSize(readSize);

    const unsigned totalNoninsertSize(thisReadNoninsertSize+mateReadNoninsertSize);

    pos_t breakendMin(0),breakendMax(0);
    {
        const pos_t startRefPos(read.pos()-1);
        const pos_t endRefPos(startRefPos+apath_ref_length(apath));
        // expected breakpoint range is from the end of the read alignment to the (probabilistic) end of the fragment:
        if (read.is_fwd_strand())
        {
            breakendMin=(endRefPos);
            breakendMax=(endRefPos + static_cast<pos_t>(rstats.max-(totalNoninsertSize)));
        }
        else
        {
            breakendMax=(startRefPos);
            breakendMin=(startRefPos - static_cast<pos_t>(rstats.max-(totalNoninsertSize)));
        }
    }
    NodeIndexType nodePtr1(locus.addNode(read.target_id(),breakendMin,breakendMax));

    {
        const pos_t startRefPos(read.mate_pos()-1);
        const pos_t endRefPos(startRefPos+readSize);
        if (read.is_mate_fwd_strand())
        {
            breakendMin=(endRefPos);
            breakendMax=(endRefPos + static_cast<pos_t>(rstats.max-(totalNoninsertSize)));
        }
        else
        {
            breakendMax=(startRefPos);
            breakendMin=(startRefPos - static_cast<pos_t>(rstats.max-(totalNoninsertSize)));
        }
    }
    NodeIndexType nodePtr2(locus.addNode(read.mate_target_id(),breakendMin,breakendMax));

    locus.linkNodes(nodePtr1,nodePtr2);
}



bool
SVLocusSetFinder::
isReadFiltered(const bam_record& read) const
{
    if (read.is_filter()) return true;
    if (read.is_dup()) return true;
    if (read.is_secondary()) return true;
    if (read.is_proper_pair()) return true;
    if (read.map_qual() < _opt.minMapq) return true;
    return false;
}



void
SVLocusSetFinder::
update(const bam_record& read,
       const unsigned defaultReadGroupIndex)
{
    // shortcut to speed things up:
    if (isReadFiltered(read)) return;

    const CachedReadGroupStats& rstats(_stats[defaultReadGroupIndex]);

    SVLocus locus;

    // start out looking for chimeric reads only:
    if (read.is_chimeric())
    {
        getChimericSVLocus(rstats,read,locus);
    }

    if(! locus.empty())
    {
        _svLoci.merge(locus);
    }
}
