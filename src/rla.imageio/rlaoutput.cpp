/*
  Copyright 2011 Larry Gritz and the other authors and contributors.
  All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the software's owners nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  (This is the Modified BSD License)
*/

#include <cstdio>
#include <cstdlib>
#include <cmath>

#include <boost/algorithm/string.hpp>
using boost::algorithm::iequals;

#include "rla_pvt.h"

#include "dassert.h"
#include "typedesc.h"
#include "imageio.h"
#include "fmath.h"

OIIO_PLUGIN_NAMESPACE_BEGIN

using namespace RLA_pvt;


class RLAOutput : public ImageOutput {
public:
    RLAOutput ();
    virtual ~RLAOutput ();
    virtual const char * format_name (void) const { return "rla"; }
    virtual bool supports (const std::string &feature) const {
        // Support nothing nonstandard
        return false;
    }
    virtual bool open (const std::string &name, const ImageSpec &spec,
                       OpenMode mode=Create);
    virtual bool close ();
    virtual bool write_scanline (int y, int z, TypeDesc format,
                                 const void *data, stride_t xstride);

private:
    std::string m_filename;           ///< Stash the filename
    FILE *m_file;                     ///< Open image handle
    std::vector<unsigned char> m_scratch;
    WAVEFRONT m_rla;                  ///< Wavefront RLA header

    // Initialize private members to pre-opened state
    void init (void) {
        m_file = NULL;
    }
    
    /// Helper - sets a chromaticity from attribute
    inline void set_chromaticity (const ImageIOParameter *p, char *dst,
                                  size_t field_size, const char *default_val);
};




// Obligatory material to make this a recognizeable imageio plugin:
OIIO_PLUGIN_EXPORTS_BEGIN

DLLEXPORT ImageOutput *rla_output_imageio_create () { return new RLAOutput; }

// DLLEXPORT int rla_imageio_version = OIIO_PLUGIN_VERSION;   // it's in rlainput.cpp

DLLEXPORT const char * rla_output_extensions[] = {
    "rla", NULL
};

OIIO_PLUGIN_EXPORTS_END


RLAOutput::RLAOutput ()
{
    init ();
}



RLAOutput::~RLAOutput ()
{
    // Close, if not already done.
    close ();
}



bool
RLAOutput::open (const std::string &name, const ImageSpec &userspec,
                 OpenMode mode)
{
    if (mode != Create) {
        error ("%s does not support subimages or MIP levels", format_name());
        return false;
    }

    close ();  // Close any already-opened file
    m_spec = userspec;  // Stash the spec

    m_file = fopen (name.c_str(), "wb");
    if (! m_file) {
        error ("Could not open file \"%s\"", name.c_str());
        return false;
    }
    
    // Check for things this format doesn't support
    if (m_spec.width < 1 || m_spec.height < 1) {
        error ("Image resolution must be at least 1x1, you asked for %d x %d",
               m_spec.width, m_spec.height);
        return false;
    }

    if (m_spec.depth < 1)
        m_spec.depth = 1;
    else if (m_spec.depth > 1) {
        error ("%s does not support volume images (depth > 1)", format_name());
        return false;
    }

    // prepare and write the RLA header
    WAVEFRONT rla;
    memset (&rla, 0, sizeof (rla));
    // frame and window coordinates
    rla.WindowLeft = m_spec.full_x;
    rla.WindowRight = m_spec.full_x + m_spec.full_width - 1;
    rla.WindowBottom = -m_spec.full_y;
    rla.WindowTop = m_spec.full_height - m_spec.full_y - 1;
    
    rla.ActiveLeft = m_spec.x;
    rla.ActiveRight = m_spec.x + m_spec.width - 1;
    rla.ActiveBottom = -m_spec.y;
    rla.ActiveTop = m_spec.height - m_spec.y - 1;

    rla.FrameNumber = m_spec.get_int_attribute ("rla:FrameNumber", 0);

    // figure out what's going on with the channels
    int remaining = m_spec.nchannels;
    if (m_spec.channelformats.size ()) {
        int streak;
        // accomodate first 3 channels of the same type as colour ones
        for (streak = 1; streak < 3 && remaining > 0; ++streak, --remaining)
            if (m_spec.channelformats[streak] != m_spec.channelformats[0])
                break;
        rla.ColorChannelType = m_spec.channelformats[0] == TypeDesc::FLOAT
            ? CT_FLOAT : CT_BYTE;
        rla.NumOfChannelBits = m_spec.channelformats[0].size () * 8;
        rla.NumOfColorChannels = streak;
        // if we have anything left, treat it as alpha
        if (remaining) {
            for (streak = 1; remaining > 0; ++streak, --remaining)
                if (m_spec.channelformats[rla.NumOfColorChannels + streak]
                    != m_spec.channelformats[rla.NumOfColorChannels])
                    break;
            rla.MatteChannelType = m_spec.channelformats[rla.NumOfColorChannels]
                == TypeDesc::FLOAT ? CT_FLOAT : CT_BYTE;
            rla.NumOfMatteBits = m_spec.channelformats[rla.NumOfColorChannels].size () * 8;
            rla.NumOfMatteChannels = streak;
        }
        // and if there's something more left, put it in auxiliary
        if (remaining) {
            for (streak = 1; remaining > 0; ++streak, --remaining)
                if (m_spec.channelformats[rla.NumOfColorChannels
                        + rla.NumOfMatteChannels + streak]
                    != m_spec.channelformats[rla.NumOfColorChannels
                        + rla.NumOfMatteChannels])
                    break;
            rla.MatteChannelType = m_spec.channelformats[rla.NumOfColorChannels
                    + rla.NumOfMatteChannels]
                == TypeDesc::FLOAT ? CT_FLOAT : CT_BYTE;
            rla.NumOfAuxBits = m_spec.channelformats[rla.NumOfColorChannels
                + rla.NumOfMatteChannels].size () * 8;
            rla.NumOfAuxChannels = streak;
        }
    } else {
        rla.ColorChannelType = rla.MatteChannelType = rla.AuxChannelType =
            m_spec.format == TypeDesc::FLOAT ? CT_FLOAT : CT_BYTE;
        rla.NumOfChannelBits = rla.NumOfMatteBits = rla.NumOfAuxBits =
            m_spec.format.size () * 8;
        if (remaining >= 3) {
            // if we have at least 3 channels, treat them as colour
            rla.NumOfColorChannels = 3;
            remaining -= 3;
        } else {
            // otherwise let's say it's luminosity
            rla.NumOfColorChannels = 1;
            --remaining;
        }
        // if there's at least 1 more channel, it's alpha
        if (remaining-- > 0)
            ++rla.NumOfMatteChannels;
        // anything left is auxiliary
        if (remaining > 0)
            rla.NumOfAuxChannels = remaining;
    }
    
    rla.Revision = 0xFFFE;
    
    std::string s = m_spec.get_string_attribute ("oiio:ColorSpace", "Unknown");
    if (iequals(s, "Linear"))
        strcpy (rla.Gamma, "1.0");
    else if (iequals(s, "GammaCorrected"))
        snprintf (rla.Gamma, sizeof(rla.Gamma), "%.10f",
            m_spec.get_float_attribute ("oiio:Gamma", 1.f));
    
    const ImageIOParameter *p;
    // default NTSC chromaticities
    p = m_spec.find_attribute ("rla:RedChroma");
    set_chromaticity (p, rla.RedChroma, sizeof (rla.RedChroma), "0.67 0.08");
    p = m_spec.find_attribute ("rla:GreenChroma");
    set_chromaticity (p, rla.GreenChroma, sizeof (rla.GreenChroma), "0.21 0.71");
    p = m_spec.find_attribute ("rla:BlueChroma");
    set_chromaticity (p, rla.BlueChroma, sizeof (rla.BlueChroma), "0.14 0.33");
    p = m_spec.find_attribute ("rla:WhitePoint");
    set_chromaticity (p, rla.WhitePoint, sizeof (rla.WhitePoint), "0.31 0.316");

    rla.JobNumber = m_spec.get_int_attribute ("rla:JobNumber", 0);
    strncpy (rla.FileName, name.c_str (), sizeof (rla.FileName));
    
    s = m_spec.get_string_attribute ("ImageDescription", "");
    if (s.length ())
        strncpy (rla.Description, s.c_str (), sizeof (rla.Description));
    
    // yay for advertising!
    strcpy (rla.ProgramName, OIIO_INTRO_STRING);
    
    s = m_spec.get_string_attribute ("rla:MachineName", "");
    if (s.length ())
        strncpy (rla.MachineName, s.c_str (), sizeof (rla.MachineName));
    s = m_spec.get_string_attribute ("rla:UserName", "");
    if (s.length ())
        strncpy (rla.UserName, s.c_str (), sizeof (rla.UserName));
    
    // the month number will be replaced with the 3-letter abbreviation
    time_t t = time (NULL);
    strftime (rla.DateCreated, sizeof (rla.DateCreated), "%m  %d %H:%M %Y",
        localtime (&t));
    switch (atoi (rla.DateCreated)) {
        case 1:  memcpy(rla.DateCreated, "JAN", 3); break;
        case 2:  memcpy(rla.DateCreated, "FEB", 3); break;
        case 3:  memcpy(rla.DateCreated, "MAR", 3); break;
        case 4:  memcpy(rla.DateCreated, "APR", 3); break;
        case 5:  memcpy(rla.DateCreated, "MAY", 3); break;
        case 6:  memcpy(rla.DateCreated, "JUN", 3); break;
        case 7:  memcpy(rla.DateCreated, "JUL", 3); break;
        case 8:  memcpy(rla.DateCreated, "AUG", 3); break;
        case 9:  memcpy(rla.DateCreated, "SEP", 3); break;
        case 10: memcpy(rla.DateCreated, "OCT", 3); break;
        case 11: memcpy(rla.DateCreated, "NOV", 3); break;
        case 12: memcpy(rla.DateCreated, "DEC", 3); break;
    }
    
    // FIXME: it appears that Wavefront have defined a set of aspect names;
    // I think it's safe not to care until someone complains
    s = m_spec.get_string_attribute ("rla:Aspect", "");
    if (s.length ())
        strncpy (rla.Aspect, s.c_str (), sizeof (rla.Aspect));
    
    snprintf (rla.AspectRatio, sizeof(rla.AspectRatio), "%.10f",
        m_spec.width / (float)m_spec.height);
    strcpy (rla.ColorChannel, m_spec.get_string_attribute ("rla:ColorChannel",
        "rgb").c_str ());
    rla.FieldRendered = m_spec.get_int_attribute ("rla:FieldRendered", 0);
    
    s = m_spec.get_string_attribute ("rla:Time", "");
    if (s.length ())
        strncpy (rla.Time, s.c_str (), sizeof (rla.Time));
        
    s = m_spec.get_string_attribute ("rla:Filter", "");
    if (s.length ())
        strncpy (rla.Filter, s.c_str (), sizeof (rla.Filter));
    
    s = m_spec.get_string_attribute ("rla:AuxData", "");
    if (s.length ())
        strncpy (rla.AuxData, s.c_str (), sizeof (rla.AuxData));
    
    if (littleendian()) {
        // RLAs are big-endian
        swap_endian (&rla.WindowLeft);
        swap_endian (&rla.WindowRight);
        swap_endian (&rla.WindowBottom);
        swap_endian (&rla.WindowTop);
        swap_endian (&rla.ActiveLeft);
        swap_endian (&rla.ActiveRight);
        swap_endian (&rla.ActiveBottom);
        swap_endian (&rla.ActiveTop);
        swap_endian (&rla.FrameNumber);
        swap_endian (&rla.ColorChannelType);
        swap_endian (&rla.NumOfColorChannels);
        swap_endian (&rla.NumOfMatteChannels);
        swap_endian (&rla.NumOfAuxChannels);
        swap_endian (&rla.Revision);
        swap_endian (&rla.JobNumber);
        swap_endian (&rla.FieldRendered);
        swap_endian (&rla.NumOfChannelBits);
        swap_endian (&rla.MatteChannelType);
        swap_endian (&rla.NumOfMatteBits);
        swap_endian (&rla.AuxChannelType);
        swap_endian (&rla.NumOfAuxBits);
        swap_endian (&rla.NextOffset);
    }
    // due to struct packing, we may get a corrupt header if we just dump the
    // struct to the file; to adress that, write every member individually
    // save some typing
#define WH(memb)    fwrite (&rla.memb, sizeof (rla.memb), 1, m_file)
    WH(WindowLeft);
    WH(WindowRight);
    WH(WindowBottom);
    WH(WindowTop);
    WH(ActiveLeft);
    WH(ActiveRight);
    WH(ActiveBottom);
    WH(ActiveTop);
    WH(FrameNumber);
    WH(ColorChannelType);
    WH(NumOfColorChannels);
    WH(NumOfMatteChannels);
    WH(NumOfAuxChannels);
    WH(Revision);
    WH(Gamma);
    WH(RedChroma);
    WH(GreenChroma);
    WH(BlueChroma);
    WH(WhitePoint);
    WH(JobNumber);
    WH(FileName);
    WH(Description);
    WH(ProgramName);
    WH(MachineName);
    WH(UserName);
    WH(DateCreated);
    WH(Aspect);
    WH(AspectRatio);
    WH(ColorChannel);
    WH(FieldRendered);
    WH(Time);
    WH(Filter);
    WH(NumOfChannelBits);
    WH(MatteChannelType);
    WH(NumOfMatteBits);
    WH(AuxChannelType);
    WH(NumOfAuxBits);
    WH(AuxData);
    WH(Reserved);
    WH(NextOffset);
#undef WH
    
    // FIXME
    int32_t temp = 0;
    for (int i = 0; i < m_spec.height; ++i)
        fwrite(&temp, sizeof(temp), 1, m_file);

    return true;
}



inline void
RLAOutput::set_chromaticity (const ImageIOParameter *p, char *dst,
                             size_t field_size, const char *default_val)
{
    if (p && p->type().basetype == TypeDesc::FLOAT) {
        switch (p->type().aggregate) {
            case TypeDesc::VEC2:
                snprintf (dst, field_size, "%.4f %.4f",
                    ((float *)p->data ())[0], ((float *)p->data ())[1]);
                break;
            case TypeDesc::VEC3:
                snprintf (dst, field_size, "%.4f %.4f %.4f",
                    ((float *)p->data ())[0], ((float *)p->data ())[1],
                        ((float *)p->data ())[2]);
                break;
        }
    } else
        strcpy (dst, default_val);
}



bool
RLAOutput::close ()
{
    if (m_file) {
        // close the stream
        fclose (m_file);
        m_file = NULL;
    }

    init ();      // re-initialize
    return true;  // How can we fail?
                  // Epicly. -- IneQuation
}



bool
RLAOutput::write_scanline (int y, int z, TypeDesc format,
                            const void *data, stride_t xstride)
{
    m_spec.auto_stride (xstride, format, spec().nchannels);
    const void *origdata = data;
    data = to_native_scanline (format, data, xstride, m_scratch);
    if (data == origdata) {
        m_scratch.assign ((unsigned char *)data,
                          (unsigned char *)data+m_spec.scanline_bytes());
        data = &m_scratch[0];
    }

    return true;
}

OIIO_PLUGIN_NAMESPACE_END

