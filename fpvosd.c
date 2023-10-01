/*****************************************************************************
 * fpvosd : MSP-OSD pseudo-subtitles decoder for FPV DVR
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>
#include <vlc_demux.h>
#include <vlc_interface.h>
#include <vlc_input.h>
#include <vlc_playlist.h>
#include <vlc_url.h>
#include <vlc_fs.h>

//#define DOMAIN  "vlc-fpvosd"
#define _(str)  dgettext(DOMAIN, str)
#define N_(str) (str)


#define FONT_BYTES_PER_PIXEL     4
// Overlay dimensions
#define DISPLAY_OVERLAY_WIDTH    1440
#define DISPLAY_OVERLAY_HEIGHT   810
// Dimensions for OSD
#define DISPLAY_ORIGINAL_WIDTH   1440
#define DISPLAY_ORIGINAL_HEIGHT  792

// OSD grid size
#define MAX_X  60
#define MAX_Y  22

// OSD font size
#define FONT_WIDTH   24
#define FONT_HEIGHT  36

// MSP-OSD
#define MAGIC "MSPOSD"
#define MSPOSD_VERSION 1

#define FOURCC_CODE VLC_FOURCC('M','S','P','O')


#define CFG_PREFIX       "fpvosd-"
#define CFG_FONT_FOLDER  CFG_PREFIX "font-folder"
#define CFG_FPS          CFG_PREFIX "fps"
#define CFG_AUTOLOAD     CFG_PREFIX "autoload"


#define FONT_FOLDER_TEXT N_("Font folder")
#define FONT_FOLDER_LONGTEXT N_("Folder with font files (ex. font_bf_hd.bin and others).")

#define FPS_TEXT N_("Frames per Second")
#define FPS_LONGTEXT N_("Frames per second for video. -1 for default")

#define AUTOLOAD_TEXT N_("Autoload .osd")
#define AUTOLOAD_LONGTEXT N_("Autoload .osd file if exists one with same name. Need enable interface module")

#define HELP_TEXT N_( \
    "FPV-OSD\n" \
    "It opens .osd file as subtitle and show OSD in realtime" \
    )


/*****************************************************************************
 * Module descriptor.
 *****************************************************************************/
static int  OpenCodec( vlc_object_t * );
static void CloseCodec( vlc_object_t * );
static int Decode( decoder_t *, block_t * );
static int  OpenDemux( vlc_object_t * );
static void CloseDemux( vlc_object_t * );
static int Demux( demux_t * );
static int  OpenInterface    ( vlc_object_t * );
static void CloseInterface   ( vlc_object_t * );
static int CfgCallback( vlc_object_t *p_this, char const *psz_var,
                         vlc_value_t oldval, vlc_value_t newval, void *p_data );

vlc_module_begin ()
	//set_category( CAT_INTERFACE )
	//set_subcategory( SUBCAT_INTERFACE_CONTROL )
	set_category( CAT_INPUT )
	set_subcategory( SUBCAT_INPUT_SCODEC )
	set_shortname( N_("FPV-OSD") )
	set_description( N_("FPV-OSD: OSD on FPV DVR") )
	set_help( HELP_TEXT )
	add_directory( CFG_FONT_FOLDER, NULL, FONT_FOLDER_TEXT, FONT_FOLDER_LONGTEXT, false )
	add_float( CFG_FPS, 60, FPS_TEXT, FPS_LONGTEXT, false )
	add_bool ( CFG_AUTOLOAD, true, AUTOLOAD_TEXT, AUTOLOAD_LONGTEXT, true )
    set_capability( "spu decoder", 10 )
    set_callbacks( OpenCodec, CloseCodec )

	add_submodule ()
    set_capability( "demux", 1 )
    set_callbacks( OpenDemux, CloseDemux )

	add_submodule ()
	set_category( CAT_INTERFACE )
	set_subcategory( SUBCAT_INTERFACE_CONTROL )
    set_capability( "interface", 0 )
    set_callbacks( OpenInterface, CloseInterface )
vlc_module_end ()


/****************************************************************************
 * Local structures
 ****************************************************************************/

// OSD (.osd) file header
typedef struct file_header_s
{
    char magic[7];
    uint16_t version;
    struct rec_config_s
    {
        uint8_t char_width;
        uint8_t char_height;
        uint8_t font_width;
        uint8_t font_height;
        uint16_t x_offset;
        uint16_t y_offset;
        uint8_t font_variant;
    } __attribute__((packed)) config;
} __attribute__((packed)) file_header_t;

// Font variants
enum  font_variant_e
{
    FONT_VARIANT_GENERIC = 0,
    FONT_VARIANT_BETAFLIGHT = 1,
    FONT_VARIANT_INAV = 2,
    FONT_VARIANT_ARDUPILOT = 3,
    FONT_VARIANT_KISS_ULTRA = 4,
    FONT_VARIANT_QUICKSILVER = 5,
	FONT_VARIANT__SIZE
};

// String codes for font variants
static const char * font_variant_str[FONT_VARIANT__SIZE] = {
        [FONT_VARIANT_GENERIC]="",
        [FONT_VARIANT_BETAFLIGHT]="_bf",
        [FONT_VARIANT_INAV]="_inav",
        [FONT_VARIANT_ARDUPILOT]="_ardu",
        [FONT_VARIANT_KISS_ULTRA]="_ultra",
        [FONT_VARIANT_QUICKSILVER]="_quic",
};

// Frame header
typedef struct frame_header_s {
	uint32_t frame_idx;
	uint32_t size;
} __attribute__((packed)) frame_header_t;


struct decoder_sys_t
{
    uint8_t * p_raw_font_page_1;
    uint8_t * p_raw_font_page_2;
    picture_t * p_pic_font_page_1;
};

typedef struct osd_entry_s {
    mtime_t start;
    mtime_t stop;
    size_t  blocknumber;
} osd_entry_t;

struct demux_sys_t {
    size_t      count;
    osd_entry_t *index;

    es_out_id_t *es;

    size_t      current;
    int64_t     next_date;
    bool        b_slave;
    bool        b_first_time;
    double      fps;
};

struct intf_sys_t
{
    vlc_mutex_t lock;
    bool b_autoload;
};

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void draw_osd_char(decoder_t *, picture_t *, int, int, uint16_t);
static void rgb_to_yuv( uint8_t *, uint8_t *, uint8_t *, int, int, int );
static char * uri_replace_ext(const char *, const char *);

/*****************************************************************************
 * OpenCodec:
 *****************************************************************************/
static int OpenCodec( vlc_object_t *p_this )
{
    static const char str_path_sep[] = "/";
    static const char str_font[] = "font";
    static const char str_font_hd[] = "_hd";
    static const char str_font_ext[] = ".bin";
    decoder_t     *decoder = (decoder_t *) p_this;
    decoder_sys_t *sys = NULL;
    FILE * fp = NULL;
    size_t font_page_size;
    char * fontpath = NULL;
    char * fontfolder = NULL;
    int rtn = VLC_SUCCESS;
    int font_variant;
    size_t fontpath_size = 0;
    const file_header_t *file_hdr = NULL;

    msg_Info( decoder, "OpenCodec()" );

    if ( decoder->fmt_in.i_codec != FOURCC_CODE )
    {
        return VLC_EGENERIC;
    }

    if ( decoder->fmt_in.i_extra != sizeof(file_header_t) )
    {
        msg_Err( decoder, "OpenCodec(): incorrect size of the extra. Expected %d, got %d", (int)sizeof(file_header_t), decoder->fmt_in.i_extra );
        return VLC_EGENERIC;
    }
    file_hdr = (const file_header_t *)decoder->fmt_in.p_extra;

    sys = (decoder_sys_t*) malloc( sizeof(decoder_sys_t) );
    if ( sys == NULL )
    {
        return VLC_ENOMEM;
    }

    // Font
    sys->p_raw_font_page_1 = NULL;
    sys->p_raw_font_page_2 = NULL;

    font_page_size = FONT_WIDTH * FONT_HEIGHT * FONT_BYTES_PER_PIXEL * 256;

    sys->p_raw_font_page_1 = malloc( font_page_size );
    if (sys->p_raw_font_page_1 == NULL) {
    	rtn = VLC_ENOMEM;
    	goto cleanup;
    }

    // get font folder
    fontfolder = var_CreateGetStringCommand( decoder, CFG_FONT_FOLDER );
    if ( fontfolder == NULL )
    {
    	msg_Err( decoder, "OpenCodec(): error get CFG_FONT_FOLDER" );
    	rtn = VLC_ENOMEM;
    	goto cleanup;
    }

    font_variant = file_hdr->config.font_variant;
    if ( font_variant < 0 || font_variant >= FONT_VARIANT__SIZE )
    {
        msg_Err( decoder, "OpenCodec(): incorrect font variant %d", font_variant );
        rtn = VLC_ENOMEM;
        goto cleanup;
    }

    // Load needed font from the fontfolder
    fontpath_size = strlen(fontfolder) + strlen(str_path_sep) +
            strlen(str_font) + strlen(font_variant_str[font_variant]) + strlen(str_font_hd) +
            strlen(str_font_ext) + 1;
    fontpath = malloc( fontpath_size );
    if ( fontpath == NULL )
    {
        msg_Err( decoder, "OpenCodec(): error malloc(%llu)", fontpath_size );
        rtn = VLC_ENOMEM;
        goto cleanup;
    }

    fontpath[0] = '\0';
    strcat(fontpath, fontfolder);
    strcat(fontpath, str_path_sep);
    strcat(fontpath, str_font);
    strcat(fontpath, font_variant_str[font_variant]);
    strcat(fontpath, str_font_hd);
    strcat(fontpath, str_font_ext);

    msg_Dbg( decoder, "OpenCodec(): open font file \"%s\"", fontpath );
    fp = vlc_fopen( fontpath, "rb" );
    if ( fp == NULL )
    {
    	msg_Err( decoder, "OpenCodec(): font file \"%s\" not found", fontpath );
    	rtn = VLC_EGENERIC;
    	goto cleanup;
    }
    fseek( fp, 0, SEEK_END );
    if ( (size_t)ftell(fp ) != font_page_size )
    {
    	msg_Err( decoder, "OpenCodec(): Incorrect size of font file" );
    	rtn = VLC_EGENERIC;
    	goto cleanup;
    }

    fseek(fp, 0, SEEK_SET);

    if ( fread( sys->p_raw_font_page_1, font_page_size, 1, fp ) != 1 )
    {
    	msg_Err( decoder, "OpenCodec(): Error read font file" );
    	rtn = VLC_EGENERIC;
    	goto cleanup;
    }

    fclose( fp ); fp = NULL;
    free( fontfolder ); fontfolder = NULL;
    free( fontpath ); fontpath = NULL;

    // Decode font to picture_t for optimization
    sys->p_pic_font_page_1 = picture_New(
    		VLC_CODEC_YUVA,
			FONT_WIDTH * 256,
			FONT_HEIGHT, 1, 1);
    if ( sys->p_pic_font_page_1 == NULL )
    {
    	msg_Err( decoder, "OpenCodec(): Error picture_New()" );
    	rtn = VLC_EGENERIC;
    	goto cleanup;
    }

    // Put chars to row to a picture_t
    for ( int i_char = 0; i_char < 256; i_char++ )
    {
    	picture_t *pic = sys->p_pic_font_page_1;
    	const int cw = FONT_WIDTH;
    	const int ch = FONT_HEIGHT;
    	int i_pitch = pic->p[0].i_pitch;
    	int i_pixel_pitch = pic->p[0].i_pixel_pitch;
    	uint8_t *font_char = sys->p_raw_font_page_1 + cw * ch * FONT_BYTES_PER_PIXEL * i_char;

    	for ( int i_line = 0; i_line < ch; i_line++ )
    	{
    		uint32_t offset = i_pitch * i_line + i_pixel_pitch * cw * i_char;  // begin of char
    		for ( int i = 0; i < cw; i++ )
    		{
    			uint8_t px[4];
    			uint8_t *inp_px = font_char + (i_line * cw + i) * FONT_BYTES_PER_PIXEL;
    			px[3] = inp_px[3];  // transparency
    			rgb_to_yuv(px, px+1, px+2, inp_px[0], inp_px[1], inp_px[2]);
    			for ( int i_plane = 0; i_plane < pic->i_planes; i_plane++ )
    			{
    				if ( i_plane < 4 )
    				{
    					pic->p[i_plane].p_pixels[offset + i_pixel_pitch * i] = px[i_plane];
    				}
    			}
    		}
    	}
    }

    decoder->p_sys = sys;
    decoder->pf_decode = Decode;
    decoder->fmt_out.i_codec = 0;

    return VLC_SUCCESS;

cleanup:
    free( fontfolder); fontfolder = NULL;
    free( fontpath); fontpath = NULL;
    if ( fp )
    {
        fclose( fp ); fp = NULL;
    }
    if ( sys )
    {
        free( sys->p_raw_font_page_1 ); sys->p_raw_font_page_1 = NULL;
        free( sys ); sys = NULL;
    }

	return rtn;
}

/*****************************************************************************
 * CloseCodec:
 *****************************************************************************/
static void CloseCodec( vlc_object_t *p_this )
{
    decoder_t     *decoder = (decoder_t*) p_this;
    decoder_sys_t *sys = decoder->p_sys;

    msg_Info( decoder, "CloseCodec()" );

    if ( sys == NULL )
    	return;

    if ( sys->p_pic_font_page_1 )
    {
    	picture_Release(sys->p_pic_font_page_1);
    	sys->p_pic_font_page_1 = NULL;
    }
	free( sys->p_raw_font_page_1 ); sys->p_raw_font_page_1 = NULL;
    free( sys ); sys = NULL;
}

/*****************************************************************************
 * draw_osd_char:
 *****************************************************************************/
static void draw_osd_char(decoder_t *decoder, picture_t *pic, int x, int y, uint16_t c) {
	const int cw = FONT_WIDTH;
	const int ch = FONT_HEIGHT;
	picture_t *font_pic = decoder->p_sys->p_pic_font_page_1;
	int yoffset = (DISPLAY_OVERLAY_HEIGHT - DISPLAY_ORIGINAL_HEIGHT) / 2;
	int xoffset = (DISPLAY_OVERLAY_WIDTH - DISPLAY_ORIGINAL_WIDTH) / 2;

	c  &= 0xFF;

	for( int i_plane = 0; i_plane < pic->i_planes; i_plane++ ) {
		int i_pitch = pic->p[i_plane].i_pitch;
		int i_pixel_pitch = pic->p[i_plane].i_pixel_pitch;
		int i_pitch_font = font_pic->p[i_plane].i_pitch;
		for ( int i_line = 0; i_line < ch; i_line++ ) {
			uint32_t offset = i_pitch * (i_line + ch * y + yoffset) + i_pixel_pitch * (x * cw + xoffset);
			uint32_t offset_font = i_pitch_font * i_line + i_pixel_pitch * cw * c;
			memcpy(pic->p[i_plane].p_pixels + offset,
				   font_pic->p[i_plane].p_pixels + offset_font,
				   i_pixel_pitch * cw);
		}
	}
}

/*****************************************************************************
 * Decode:
 *****************************************************************************/
static int Decode( decoder_t *decoder, block_t *block )
{
    decoder_sys_t *sys = decoder->p_sys;
    subpicture_t *spu = NULL;
    video_format_t fmt;
    subpicture_region_t *p_region;

    //msg_Info(decoder, "Decode()" );

    if ( block == NULL ) /* No Drain */
        return VLCDEC_SUCCESS;

    if ( block->i_flags & BLOCK_FLAG_CORRUPTED )
    {
    	msg_Warn( decoder, "Decode(): skip corrupted block" );
        block_Release( block );
        return VLCDEC_SUCCESS;
    }
    VLC_UNUSED(sys);

    //msg_Info(decoder, "Decode(): i_pts=%lld i_buffer=%lld i_length=%lld i_size=%lld", block->i_pts, block->i_buffer, block->i_length, block->i_size );

    spu = decoder_NewSubpicture( decoder, NULL );
	if ( spu != NULL )
	{
		spu->i_start = block->i_pts;
		//spu->i_stop = block->i_pts + block->i_length;
		// TODO: To ensure that the OSD does not disappear when paused
		spu->i_stop = spu->i_start + CLOCK_FREQ * 1000000;
		spu->b_ephemer = true;

		spu->b_absolute = true;
		spu->b_subtitle = true;
		spu->i_original_picture_width = DISPLAY_OVERLAY_WIDTH;
		spu->i_original_picture_height = DISPLAY_OVERLAY_HEIGHT;

	    // Create new SPU region
	    memset( &fmt, 0, sizeof(video_format_t) );
	    fmt.i_chroma = VLC_CODEC_YUVA;
	    fmt.i_sar_num = fmt.i_sar_den = 1;
	    fmt.i_width = fmt.i_visible_width = spu->i_original_picture_width;
	    fmt.i_height = fmt.i_visible_height = spu->i_original_picture_height;
	    fmt.i_x_offset = fmt.i_y_offset = 0;
	    fmt.transfer = TRANSFER_FUNC_BT709;
	    fmt.primaries = COLOR_PRIMARIES_BT709;
	    fmt.space = COLOR_SPACE_BT709;
	    fmt.b_color_range_full = false;
	    p_region = subpicture_region_New( &fmt );
	    if ( !p_region )
	    {
	        msg_Err( decoder, "cannot allocate SPU region" );
	        subpicture_Delete( spu );
	        spu = NULL;
	        goto exit;
	    }
		p_region->i_align = 0;
	    p_region->i_x = 0;
	    p_region->i_y = 0;

	    spu->p_region = p_region;
	    spu->i_alpha = 255;  // non-transparent

	    // Draw all non-null chars
	    uint16_t * map = (uint16_t *)(block->p_buffer + sizeof(frame_header_t));
	    for ( int x_i = 0; x_i < MAX_X; x_i++ ) {
	    	for ( int y_i = 0; y_i < MAX_Y; y_i++ ) {
	    		uint16_t c = map[MAX_Y * x_i + y_i];
	    		if ( c != 0 ) {
	    			draw_osd_char( decoder, p_region->p_picture, x_i, y_i, c );
	    		}
	    	}
	    }
		decoder_QueueSub( decoder, spu );
	} else {
		msg_Err( decoder, "Decode(): spu=NULL" );
	}

exit:
    block_Release( block );
    return VLCDEC_SUCCESS;
}

/*****************************************************************************
 * ControlDemux:
 *****************************************************************************/
static int ControlDemux(demux_t *demux, int query, va_list args)
{
	//msg_Dbg( demux, "ControlDemux(%d, ...)", query );

    demux_sys_t *sys = demux->p_sys;
    switch ( query ) {
    case DEMUX_CAN_SEEK: {
    	int ret = vlc_stream_vaControl( demux->s, query, args );
    	//msg_Dbg( demux, "ControlDemux(DEMUX_CAN_SEEK, ...) = %d", ret );
        return ret;
    }
    case DEMUX_GET_LENGTH: {
        int64_t *l = va_arg( args, int64_t * );
        //msg_Dbg( demux, "ControlDemux(DEMUX_GET_LENGTH, %lld)", l );
        *l = sys->count > 0 ? sys->index[sys->count-1].stop : 0;
        return VLC_SUCCESS;
    }
    case DEMUX_GET_TIME: {
        int64_t *t = va_arg( args, int64_t * );
        *t = sys->next_date - var_GetInteger( demux->obj.parent, "spu-delay" );
        //msg_Dbg( demux, "ControlDemux(DEMUX_GET_TIME, %lld) spu-delay=%lld", *t, (int64_t)var_GetInteger( demux->obj.parent, "spu-delay" ) );
        if ( *t < 0 )
            *t = sys->next_date;
        return VLC_SUCCESS;
    }
    case DEMUX_SET_NEXT_DEMUX_TIME: {
        sys->b_slave = true;
        sys->next_date = va_arg( args, int64_t );
        //msg_Dbg( demux, "ControlDemux(DEMUX_SET_NEXT_DEMUX_TIME, %lld)", sys->next_date );
        return VLC_SUCCESS;
    }
    case DEMUX_SET_TIME: {
        int64_t t = va_arg( args, int64_t );
        //msg_Dbg( demux, "ControlDemux(DEMUX_SET_TIME, %lld)", t );
        for ( size_t i = 0; i + 1 < sys->count; i++ )
        {
            if ( sys->index[i + 1].start >= t &&
                vlc_stream_Seek( demux->s, 1024 + 128LL * sys->index[i].blocknumber ) == VLC_SUCCESS )
            {
                sys->current = i;
                sys->next_date = t;
                sys->b_first_time = true;
                return VLC_SUCCESS;
            }
        }
        break;
    }
    case DEMUX_SET_POSITION:
    {
        double f = va_arg( args, double );
        //msg_Info( demux, "ControlDemux(DEMUX_SET_POSITION, %f)", f );
        if (sys->count && sys->index[sys->count-1].stop > 0)
        {
            int64_t i64 = f * sys->index[sys->count-1].stop;
            return demux_Control( demux, DEMUX_SET_TIME, i64 );
        }
        break;
    }
    case DEMUX_GET_POSITION:
    {
        double *pf = va_arg( args, double * );
        if ( sys->current >= sys->count )
        {
            *pf = 1.0;
        }
        else if ( sys->count > 0 && sys->index[sys->count-1].stop > 0 )
        {
            *pf = sys->next_date - var_GetInteger( demux->obj.parent, "spu-delay" );
            if (*pf < 0)
               *pf = sys->next_date;
            *pf /= sys->index[sys->count-1].stop;
        }
        else
        {
            *pf = 0.0;
        }
        //msg_Dbg( demux, "ControlDemux(DEMUX_GET_POSITION, ...) = %f", *pf );
        return VLC_SUCCESS;
    }
    default:
    	//msg_Dbg( demux, "ControlDemux(%d, ...)", query );
        break;
    }
    return VLC_EGENERIC;
}

/*****************************************************************************
 * Demux:
 *****************************************************************************/
static int Demux(demux_t *demux)
{
	const size_t frame_size = MAX_X * MAX_Y * sizeof(uint16_t) + sizeof(frame_header_t);
    demux_sys_t *sys = demux->p_sys;

    //msg_Dbg( demux, "Demux()" );

    int64_t i_barrier = sys->next_date;
    i_barrier -= var_GetInteger( demux->obj.parent, "spu-delay" );
    if (i_barrier < 0)
        i_barrier = sys->next_date;

    while ( sys->current < sys->count &&
          sys->index[sys->current].start <= i_barrier )
    {
        osd_entry_t *s = &sys->index[sys->current];

        if ( !sys->b_slave && sys->b_first_time )
        {
            es_out_SetPCR( demux->out, VLC_TS_0 + i_barrier );
            sys->b_first_time = false;
        }

        const uint64_t i_pos = 18 + frame_size * s->blocknumber;
        if ( i_pos != vlc_stream_Tell( demux->s ) &&
        		vlc_stream_Seek( demux->s, i_pos ) != VLC_SUCCESS )
            return VLC_DEMUXER_EOF;

        block_t *b = vlc_stream_Block( demux->s, frame_size );
        if ( b && b->i_buffer == frame_size )
        {
            b->i_dts =
            b->i_pts = VLC_TS_0 + s->start;
            if ( s->stop > s->start )
                b->i_length = s->stop - s->start;
            //msg_Info( demux, "Demux() i_start = %lld", s->start );
            es_out_Send(demux->out, sys->es, b);
        }
        else
        {
            if ( b )
                block_Release( b );
            return VLC_DEMUXER_EOF;
        }
        sys->current++;
    }

    if ( !sys->b_slave )
    {
        es_out_SetPCR( demux->out, VLC_TS_0 + i_barrier );
        sys->next_date += CLOCK_FREQ / 8;
        //msg_Info( demux, "Demux() sys->next_date=%lld i_barrier=%lld", sys->next_date, i_barrier );
    }

    return sys->current < sys->count ? VLC_DEMUXER_SUCCESS : VLC_DEMUXER_EOF;
}

/*****************************************************************************
 * OpenDemux:
 *****************************************************************************/
static int OpenDemux(vlc_object_t *object)
{
	const size_t frame_size = MAX_X * MAX_Y * sizeof(uint16_t);
    demux_t *demux = (demux_t*)object;
    double fps; // TODO: Get from video
    size_t frame_count;
    uint64_t size;
    file_header_t file_hdr, *p_file_hdr = NULL;
    demux_sys_t *sys = NULL;
    es_format_t fmt;

    msg_Dbg( demux, "OpenDemux(): filepath=%s name=%s file=%s", demux->s->psz_filepath, demux->s->psz_name, demux->psz_file );

    fps = var_CreateGetFloatCommand( demux, CFG_FPS );
    if ( fps <= 0 )
    	fps = 60;

    if ( vlc_stream_Peek( demux->s, (const uint8_t **)&p_file_hdr, sizeof(file_header_t) ) != sizeof(file_header_t) )
        return VLC_EGENERIC;

    if ( memcmp( p_file_hdr->magic, MAGIC, sizeof(p_file_hdr->magic) ) )
    {
    	return VLC_EGENERIC;
    }
    if ( p_file_hdr->version != MSPOSD_VERSION )
    {
    	msg_Dbg( demux, "OpenDemux(): unsupported version. expected: %d, got: %d", MSPOSD_VERSION, (int)p_file_hdr->version );
    	return VLC_EGENERIC;
    }
    if ( p_file_hdr->config.char_width != MAX_X ||
    		p_file_hdr->config.char_height != MAX_Y ||
			p_file_hdr->config.font_width != FONT_WIDTH ||
			p_file_hdr->config.font_height != FONT_HEIGHT ||
			p_file_hdr->config.x_offset != 0 ||
			p_file_hdr->config.y_offset != 0 ||
			p_file_hdr->config.font_variant >= FONT_VARIANT__SIZE )
    {
    	msg_Warn( demux, "OpenDemux(): unsupported config. Try anyway" );
    }

    msg_Dbg( demux, "OpenDemux(): valid OSD file!" );

	if ( vlc_stream_GetSize( demux->s, &size ) != VLC_SUCCESS )
	{
		msg_Err( demux, "OpenDemux(): Error retrieve stream size" );
		return VLC_EGENERIC;
	}
	frame_count = (size - sizeof(file_header_t)) / (sizeof(frame_header_t) + frame_size);

    if ( vlc_stream_Read( demux->s, &file_hdr, sizeof(file_header_t) ) != sizeof(file_header_t) )
    {
        msg_Err( demux, "OpenDemux(): Incomplete MSPOSD header" );
        return VLC_EGENERIC;
    }

    sys = malloc( sizeof(*sys) );
    if ( !sys )
        return VLC_EGENERIC;

    sys->b_slave   = false;
    sys->b_first_time = true;
    sys->next_date = 0;
    sys->current   = 0;
    sys->count     = 0;
    sys->index     = calloc( frame_count, sizeof(*sys->index) );
    if ( !sys->index )
    {
        free(sys);
        return VLC_EGENERIC;
    }

    for ( size_t i = 0; i < frame_count; i++ )
    {
    	frame_header_t hdr;
    	uint8_t frame_data[frame_size];
    	if ( vlc_stream_Read( demux->s, &hdr, sizeof(hdr) ) != sizeof(hdr) ) {
    		msg_Warn(demux, "OpenDemux(): Incomplete OSD file");
    		break;
    	}
    	if ( vlc_stream_Read( demux->s, frame_data, frame_size ) != frame_size ) {
    		msg_Warn(demux, "OpenDemux(): Incomplete OSD file");
    		break;
    	}
    	//msg_Info( demux, "OpenDemux(): #%llu hdr.frame_idx=%u hdr.size=%u", i, hdr.frame_idx, hdr.size );
    	sys->index[sys->count].start = hdr.frame_idx * CLOCK_FREQ / fps;
    	sys->index[sys->count].stop = sys->index[sys->count].start + CLOCK_FREQ / 10;
    	sys->index[sys->count].blocknumber = i;
    	if (sys->count >= 1) {
    		sys->index[sys->count - 1].stop = sys->index[sys->count].start;
    	}
    	sys->count++;
    }

	demux->p_sys = sys;
	if ( sys->count == 0 )
	{
		CloseDemux( object );
		return VLC_EGENERIC;
	}

    es_format_Init( &fmt, SPU_ES, FOURCC_CODE );
    fmt.i_extra = sizeof(file_header_t);
    fmt.p_extra = &file_hdr;

    sys->es = es_out_Add( demux->out, &fmt );
    fmt.i_extra = 0;
    fmt.p_extra = NULL;
    es_format_Clean( &fmt );

    if ( sys->es == NULL )
    {
    	CloseDemux( object );
        return VLC_EGENERIC;
    }

    demux->p_sys      = sys;
    demux->pf_demux   = Demux;
    demux->pf_control = ControlDemux;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * CloseDemux:
 *****************************************************************************/
static void CloseDemux(vlc_object_t *object)
{
    demux_t *demux = (demux_t*)object;
    demux_sys_t *sys = demux->p_sys;

    msg_Dbg( demux, "CloseDemux()" );

    free( sys->index );
    free( sys );
}

/*****************************************************************************
 * rgb_to_yuv:
 *****************************************************************************/
static void rgb_to_yuv( uint8_t *y, uint8_t *u, uint8_t *v,
                               int r, int g, int b )
{
    *y = ( ( (  66 * r + 129 * g +  25 * b + 128 ) >> 8 ) + 16 );
    *u =   ( ( -38 * r -  74 * g + 112 * b + 128 ) >> 8 ) + 128 ;
    *v =   ( ( 112 * r -  94 * g -  18 * b + 128 ) >> 8 ) + 128 ;
}

/*****************************************************************************
 * ItemChange: calls when new file opened
 *****************************************************************************/
static int ItemChange( vlc_object_t *p_this, const char *psz_var,
                       vlc_value_t oldval, vlc_value_t newval, void *param )
{
    VLC_UNUSED( psz_var ); VLC_UNUSED( oldval ); VLC_UNUSED( newval );
    input_thread_t *p_input = newval.p_address;
    intf_thread_t  *p_intf  = param;
    intf_sys_t     *p_sys   = p_intf->p_sys;
    char * newuri = NULL;
    char * newpath = NULL;
    struct stat st;
    int ret;
    bool b_autoload;

    if( !p_input )
        return VLC_SUCCESS;

    vlc_mutex_lock( &p_sys->lock );
    b_autoload = p_sys->b_autoload;
    vlc_mutex_unlock( &p_sys->lock );

    if ( b_autoload )
    {
		// Get opened file
		input_item_t *p_input_item = input_GetItem( p_input );

		// Skip if non-regular file
		if ( p_input_item->i_type != ITEM_TYPE_FILE )
		{
			return VLC_SUCCESS;
		}

		newuri = uri_replace_ext( p_input_item->psz_uri, ".osd" );
		if ( newuri )
		{
			newpath = vlc_uri2path( newuri );
			if ( newpath )
			{
				// If osd-file exits, then add it as subtitles
				if ( vlc_stat( newpath, &st ) == 0 )
				{
					ret = input_AddSlave( p_input, SLAVE_TYPE_SPU, newuri, true, true, false );
					VLC_UNUSED( ret );
					msg_Dbg( p_this, "ItemChange(): Auto-add OSD file as subtitles ret=%d", ret );
				}
				free( newpath ); newpath = NULL;
			}
			free( newuri ); newuri = NULL;
		}
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * OpenInterface: initialize and create stuff
 *****************************************************************************/
static int OpenInterface( vlc_object_t *p_this )
{
    intf_thread_t   *p_intf = (intf_thread_t *)p_this;
    intf_sys_t      *p_sys  = malloc( sizeof( *p_sys ) );

    if( !p_sys )
        return VLC_ENOMEM;

    msg_Dbg( p_intf, "OpenInterface():" );

    p_intf->p_sys = p_sys;

    vlc_mutex_init( &p_sys->lock );

    p_sys->b_autoload = var_CreateGetBoolCommand( p_intf, CFG_AUTOLOAD );

    var_AddCallback( pl_Get( p_intf ), "input-current", ItemChange, p_intf );

    // TODO callback for cfg change on the fly
    var_AddCallback( p_intf, CFG_AUTOLOAD, CfgCallback, p_sys );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * CloseInterface: destroy interface stuff
 *****************************************************************************/
static void CloseInterface( vlc_object_t *p_this )
{
    intf_thread_t   *p_intf = ( intf_thread_t* ) p_this;
    intf_sys_t      *p_sys  = p_intf->p_sys;

    msg_Dbg( p_intf, "CloseInterface():" );

    var_DelCallback( pl_Get( p_intf ), "input-current", ItemChange, p_this );
    var_DelCallback( p_intf, CFG_AUTOLOAD, CfgCallback, p_sys );

    vlc_mutex_destroy( &p_sys->lock );

    free( p_sys );
}

/*****************************************************************************
 * replace_ext: returns URI with new extension
 *****************************************************************************/
static char * uri_replace_ext(const char * uri, const char * newext)
{
	size_t n = strlen( uri );
	const char *oldext;
	char * newuri;

	if ( uri == NULL || newext == NULL )
		return NULL;

	// locate extension
	for ( oldext = uri + n; oldext > uri; oldext-- )
	{
		if ( *oldext == '/' )
		{
			// no extension
			oldext = uri;
			break;
		}
		if ( *oldext == '.' )
		{
			// found extension
			break;
		}
	}
	if ( oldext == uri )
	{
		// no extension
		oldext = uri + n;
	}
	newuri = malloc( (oldext - uri) + strlen(newext) + 1 );
	if ( newuri == NULL )
		return NULL;

	strcpy( newuri, uri );
	strcpy( newuri + (oldext - uri), newext );

	return newuri;
}

/*****************************************************************************
 * Callback to update params on the fly
 *****************************************************************************/
static int CfgCallback( vlc_object_t *p_this, char const *psz_var,
                         vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    VLC_UNUSED(oldval);
    intf_sys_t *p_sys = (intf_sys_t *)p_data;

    msg_Dbg( p_this, "CfgCallback():" );

    vlc_mutex_lock( &p_sys->lock );
    if( !strcmp( psz_var, CFG_AUTOLOAD ) )
    {
        msg_Dbg( p_this, "CfgCallback(): CFG_AUTOLOAD=%d", (int)newval.b_bool );
        p_sys->b_autoload = newval.b_bool;
    }
    vlc_mutex_unlock( &p_sys->lock );

    return VLC_SUCCESS;
}
