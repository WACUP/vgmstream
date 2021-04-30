#include "meta.h"
#include "../coding/coding.h"
#include "../layout/layout.h"
#include "opus_interleave_streamfile.h"

/* Nintendo OPUS - from Switch games, including header variations (not the same as Ogg Opus) */

static VGMSTREAM* init_vgmstream_opus(STREAMFILE* sf, meta_t meta_type, off_t offset, int32_t num_samples, int32_t loop_start, int32_t loop_end) {
    VGMSTREAM* vgmstream = NULL;
    off_t start_offset;
    int loop_flag = 0, channel_count;
    off_t data_offset, samples_offset, multichannel_offset = 0;
    size_t data_size, skip = 0;

    /* header chunk */
    if (read_u32le(offset + 0x00,sf) != 0x80000001)
        goto fail;
    /* 0x04: chunk size */

    /* 0x08: null */
    channel_count = read_u8(offset + 0x09, sf);
    /* 0x0a: packet size if CBR, 0 if VBR */
    data_offset = read_u32le(offset + 0x10, sf);
    /* 0x14: null/reserved? */
    samples_offset = read_u32le(offset + 0x18, sf);
    skip = read_u16le(offset + 0x1c, sf);
    /* 0x1e: ? (seen in Lego Movie 2 (Switch)) */

    /* samples chunk, rare [Famicom Detective Club (Switch)] */
    if (samples_offset && read_u32le(offset + samples_offset, sf) == 0x80000003) {
        /* maybe should give priority to external info? */
        samples_offset += offset;
        /* 0x08: null*/
        loop_flag   = read_u8   (samples_offset + 0x09, sf);
        num_samples = read_s32le(samples_offset + 0x0c, sf); /* slightly smaller than manual count */
        loop_start  = read_s32le(samples_offset + 0x10, sf);
        loop_end    = read_s32le(samples_offset + 0x14, sf);
        /* rest (~0x38) reserved/alignment? */
        /* values seem to take encoder delay into account */
    }
    else {
        loop_flag = (loop_end > 0); /* -1 when not set */
    }


    /* multichannel chunk, rare [Clannad (Switch)] */
    if (read_u32le(offset + 0x20, sf) == 0x80000005) {
        multichannel_offset = offset + 0x20;
    }


    /* data chunk */
    data_offset += offset;
    if (read_u32le(data_offset, sf) != 0x80000004)
        goto fail;

    data_size = read_u32le(data_offset + 0x04, sf);

    start_offset = data_offset + 0x08;


    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(channel_count,loop_flag);
    if (!vgmstream) goto fail;

    vgmstream->meta_type = meta_type;
    vgmstream->sample_rate = read_u32le(offset + 0x0c,sf);
    if (vgmstream->sample_rate == 16000)
	    vgmstream->sample_rate = 48000; // Grandia HD Collection contains a false sample_rate in header
    vgmstream->num_samples = num_samples;
    vgmstream->loop_start_sample = loop_start;
    vgmstream->loop_end_sample = loop_end;
    vgmstream->stream_size = data_size; /* to avoid inflated sizes from fake OggS IO */

#ifdef VGM_USE_FFMPEG
    {
        opus_config cfg = {0};

        cfg.channels = vgmstream->channels;
        cfg.skip = skip;
        cfg.sample_rate = vgmstream->sample_rate;

        if (multichannel_offset && vgmstream->channels <= 8) {
            int i;
            cfg.stream_count = read_u8(multichannel_offset + 0x08,sf);
            cfg.coupled_count = read_u8(multichannel_offset + 0x09,sf);
            for (i = 0; i < vgmstream->channels; i++) {
                cfg.channel_mapping[i] = read_u8(multichannel_offset + 0x0a + i,sf);
            }
        }

        vgmstream->codec_data = init_ffmpeg_switch_opus_config(sf, start_offset,data_size, &cfg);
        if (!vgmstream->codec_data) goto fail;
        vgmstream->coding_type = coding_FFmpeg;
        vgmstream->layout_type = layout_none;
        vgmstream->channel_layout = ffmpeg_get_channel_layout(vgmstream->codec_data);

        if (vgmstream->num_samples == 0) {
            vgmstream->num_samples = switch_opus_get_samples(start_offset, data_size, sf) - skip;
        }
    }
#else
    goto fail;
#endif

    if (!vgmstream_open_stream(vgmstream, sf, start_offset))
        goto fail;
    return vgmstream;

fail:
    close_vgmstream(vgmstream);
    return NULL;
}


/* standard Switch Opus, Nintendo header + raw data (generated by opus_test.c?) [Lego City Undercover (Switch)] */
VGMSTREAM* init_vgmstream_opus_std(STREAMFILE* sf) {
    STREAMFILE* psi_sf = NULL;
    off_t offset;
    int num_samples, loop_start, loop_end;

    /* checks */
    /* .opus: standard
     * .bgm: Cotton Reboot (Switch) */
    if (!check_extensions(sf,"opus,lopus,bgm"))
        goto fail;

    offset = 0x00;

    /* BlazBlue: Cross Tag Battle (Switch) PSI Metadata for corresponding Opus */
    /* Maybe future Arc System Works games will use this too? */
    psi_sf = open_streamfile_by_ext(sf, "psi");
    if (psi_sf) {
        num_samples = read_s32le(0x8C, psi_sf);
        loop_start = read_s32le(0x84, psi_sf);
        loop_end = read_s32le(0x88, psi_sf);
        close_streamfile(psi_sf);
    }
    else {
        num_samples = 0;
        loop_start = 0;
        loop_end = 0;
    }

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples,loop_start,loop_end);
fail:
    return NULL;
}

/* Nippon1 variation [Disgaea 5 (Switch)] */
VGMSTREAM* init_vgmstream_opus_n1(STREAMFILE* sf) {
    off_t offset;
    int num_samples, loop_start, loop_end;

    /* checks */
    if ( !check_extensions(sf,"opus,lopus"))
        goto fail;
    if (!((read_32bitBE(0x04,sf) == 0x00000000 && read_32bitBE(0x0c,sf) == 0x00000000) ||
          (read_32bitBE(0x04,sf) == 0xFFFFFFFF && read_32bitBE(0x0c,sf) == 0xFFFFFFFF)))
        goto fail;

    offset = 0x10;
    num_samples = 0;
    loop_start = read_32bitLE(0x00,sf);
    loop_end = read_32bitLE(0x08,sf);

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples,loop_start,loop_end);
fail:
    return NULL;
}

/* Capcom variation [Ultra Street Fighter II (Switch), Resident Evil: Revelations (Switch)] */
VGMSTREAM* init_vgmstream_opus_capcom(STREAMFILE* sf) {
    VGMSTREAM *vgmstream = NULL;
    off_t offset;
    int num_samples, loop_start, loop_end;
    int channel_count;

    /* checks */
    if ( !check_extensions(sf,"opus,lopus"))
        goto fail;

    channel_count = read_32bitLE(0x04,sf);
    if (channel_count != 1 && channel_count != 2 && channel_count != 6)
        goto fail; /* unknown stream layout */

    num_samples = read_32bitLE(0x00,sf);
    /* 0x04: channels, >2 uses interleaved streams (2ch+2ch+2ch) */
    loop_start = read_32bitLE(0x08,sf);
    loop_end = read_32bitLE(0x0c,sf);
    /* 0x10: frame size (with extra data) */
    /* 0x14: extra chunk count */
    /* 0x18: null */
    offset = read_32bitLE(0x1c,sf);
    /* 0x20-8: config? (0x0077C102 04000000 E107070C) */
    /* 0x2c: some size? */
    /* 0x30+: extra chunks (0x00: 0x7f, 0x04: num_sample), alt loop starts/regions? */

    if (channel_count == 6) {
        /* 2ch multistream hacky-hacks in RE:RE, don't try this at home. We'll end up with:
         * main vgmstream > N vgmstream layers > substream IO deinterleaver > opus meta > Opus IO transmogrifier (phew) */
        layered_layout_data* data = NULL;
        int layers = channel_count / 2;
        int i;
        int loop_flag = (loop_end > 0);


        /* build the VGMSTREAM */
        vgmstream = allocate_vgmstream(channel_count,loop_flag);
        if (!vgmstream) goto fail;

        vgmstream->layout_type = layout_layered;

        /* init layout */
        data = init_layout_layered(layers);
        if (!data) goto fail;
        vgmstream->layout_data = data;

        /* open each layer subfile */
        for (i = 0; i < layers; i++) {
            STREAMFILE* temp_sf = setup_opus_interleave_streamfile(sf, offset, i, layers);
            if (!temp_sf) goto fail;

            data->layers[i] = init_vgmstream_opus(temp_sf, meta_OPUS, 0x00, num_samples,loop_start,loop_end);
            close_streamfile(temp_sf);
            if (!data->layers[i]) goto fail;
        }

        /* setup layered VGMSTREAMs */
        if (!setup_layout_layered(data))
            goto fail;

        vgmstream->sample_rate = data->layers[0]->sample_rate;
        vgmstream->num_samples = data->layers[0]->num_samples;
        vgmstream->loop_start_sample = data->layers[0]->loop_start_sample;
        vgmstream->loop_end_sample = data->layers[0]->loop_end_sample;
        vgmstream->meta_type = meta_OPUS;
        vgmstream->coding_type = data->layers[0]->coding_type;

        return vgmstream;
    }
    else {
        return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples,loop_start,loop_end);
    }


fail:
    close_vgmstream(vgmstream);
    return NULL;
}

/* Procyon Studio variation [Xenoblade Chronicles 2 (Switch)] */
VGMSTREAM* init_vgmstream_opus_nop(STREAMFILE* sf) {
    off_t offset;
    int num_samples, loop_start = 0, loop_end = 0, loop_flag;

    /* checks */
    if (!check_extensions(sf,"nop"))
        goto fail;
    if (read_32bitBE(0x00, sf) != 0x73616466 || /* "sadf" */
        read_32bitBE(0x08, sf) != 0x6f707573)   /* "opus" */
        goto fail;

    offset = read_32bitLE(0x1c, sf);
    num_samples = read_32bitLE(0x28, sf);
    loop_flag = read_8bit(0x19, sf);
    if (loop_flag) {
        loop_start = read_32bitLE(0x2c, sf);
        loop_end = read_32bitLE(0x30, sf);
    }

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples,loop_start,loop_end);
fail:
    return NULL;
}

/* Shin'en variation [Fast RMX (Switch)] */
VGMSTREAM* init_vgmstream_opus_shinen(STREAMFILE* sf) {
    off_t offset = 0;
    int num_samples, loop_start, loop_end;

    /* checks */
    if ( !check_extensions(sf,"opus,lopus"))
        goto fail;
    if (read_32bitBE(0x08,sf) != 0x01000080)
        goto fail;

    offset = 0x08;
    num_samples = 0;
    loop_start = read_32bitLE(0x00,sf);
    loop_end = read_32bitLE(0x04,sf); /* 0 if no loop */

    if (loop_start > loop_end)
        goto fail; /* just in case */

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples,loop_start,loop_end);
fail:
    return NULL;
}

/* Bandai Namco Opus (found in NUS3Banks) [Taiko no Tatsujin: Nintendo Switch Version!] */
VGMSTREAM* init_vgmstream_opus_nus3(STREAMFILE* sf) {
    off_t offset = 0;
    int num_samples = 0, loop_start = 0, loop_end = 0, loop_flag;

    /* checks */
    /* .opus: header ID (they only exist inside .nus3bank) */
    if (!check_extensions(sf, "opus,lopus"))
        goto fail;
    if (read_32bitBE(0x00, sf) != 0x4F505553) /* "OPUS" */
        goto fail;

    /* Here's an interesting quirk, OPUS header contains big endian values
       while the Nintendo Opus header and data that follows remain little endian as usual */
    offset = read_32bitBE(0x20, sf);
    num_samples = read_32bitBE(0x08, sf);

    /* Check if there's a loop end value to determine loop_flag*/
    loop_flag = read_32bitBE(0x18, sf);
    if (loop_flag) {
        loop_start = read_32bitBE(0x14, sf);
        loop_end = read_32bitBE(0x18, sf);
    }

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples, loop_start, loop_end);
fail:
    return NULL;
}

/* Nippon Ichi SPS wrapper (non-segmented) [Ys VIII: Lacrimosa of Dana (Switch)] */
VGMSTREAM* init_vgmstream_opus_sps_n1(STREAMFILE* sf) {
    off_t offset;
    int num_samples, loop_start = 0, loop_end = 0, loop_flag;

    /* checks */
    /* .sps: Labyrinth of Refrain: Coven of Dusk (Switch)
     * .nlsd: Disgaea Refine (Switch), Ys VIII (Switch)
     * .at9: void tRrLM(); //Void Terrarium (Switch) */
    if (!check_extensions(sf, "sps,nlsd,at9"))
        goto fail;
    if (read_32bitBE(0x00, sf) != 0x09000000) /* file type (see other N1 SPS) */
        goto fail;

    num_samples = read_32bitLE(0x0C, sf);

    if (read_32bitBE(0x1c, sf) == 0x01000080) {
        offset = 0x1C;

        /* older games loop section (remnant of segmented opus_sps_n1): */
        loop_start = read_32bitLE(0x10, sf); /* intro samples */
        loop_end = loop_start + read_32bitLE(0x14, sf); /* loop samples */
        /* 0x18: end samples (all must add up to num_samples) */
        loop_flag = read_32bitLE(0x18, sf); /* with loop disabled only loop_end has a value */
    }
    else {
        offset = 0x18;

        /* newer games loop section: */
        loop_start = read_32bitLE(0x10, sf);
        loop_end = read_32bitLE(0x14, sf);
        loop_flag = loop_start != loop_end; /* with loop disabled start and end are the same as num samples */
    }

    if (!loop_flag) {
        loop_start = 0;
        loop_end = 0;
    }

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples, loop_start, loop_end);
fail:
    return NULL;
}

/* AQUASTYLE wrapper [Touhou Genso Wanderer -Reloaded- (Switch)] */
VGMSTREAM* init_vgmstream_opus_opusx(STREAMFILE* sf) {
    off_t offset;
    int num_samples, loop_start = 0, loop_end = 0;
    float modifier;

    /* checks */
    if (!check_extensions(sf, "opusx"))
        goto fail;
    if (read_32bitBE(0x00, sf) != 0x4F505553) /* "OPUS" */
        goto fail;

    offset = 0x10;
    /* values are for the original 44100 files, but Opus resamples to 48000 */
    modifier = 48000.0f / 44100.0f;
    num_samples = 0;//read_32bitLE(0x04, sf) * modifier; /* better use calc'd num_samples */
    loop_start = read_32bitLE(0x08, sf) * modifier;
    loop_end = read_32bitLE(0x0c, sf) * modifier;

    /* resampling calcs are slighly off and may to over num_samples, but by removing delay seems ok */
    if (loop_start >= 120) {
        loop_start -= 128;
        loop_end -= 128;
    }
    else {
        loop_end = 0;
    }

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples, loop_start, loop_end);
fail:
    return NULL;
}

/* Prototype variation [Clannad (Switch)] */
VGMSTREAM* init_vgmstream_opus_prototype(STREAMFILE* sf) {
    off_t offset = 0;
    int num_samples = 0, loop_start = 0, loop_end = 0, loop_flag;

    /* checks */
    if (!check_extensions(sf, "opus,lopus"))
        goto fail;
    if (read_32bitBE(0x00, sf) != 0x4F505553 || /* "OPUS" */
        read_32bitBE(0x18, sf) != 0x01000080)
        goto fail;

    offset = 0x18;
    num_samples = read_32bitLE(0x08, sf);

    /* Check if there's a loop end value to determine loop_flag*/
    loop_flag = read_32bitLE(0x10, sf);
    if (loop_flag) {
        loop_start = read_32bitLE(0x0C, sf);
        loop_end = read_32bitLE(0x10, sf);
    }

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples, loop_start, loop_end);
fail:
    return NULL;
}

/* Edelweiss variation [Astebreed (Switch)] */
VGMSTREAM* init_vgmstream_opus_opusnx(STREAMFILE* sf) {
    off_t offset = 0;
    int num_samples = 0, loop_start = 0, loop_end = 0;

    /* checks */
    if (!check_extensions(sf, "opus,lopus"))
        goto fail;
    if (read_64bitBE(0x00, sf) != 0x4F5055534E580000) /* "OPUSNX\0\0" */
        goto fail;

    offset = 0x10;
    num_samples = 0; //read_32bitLE(0x08, sf); /* samples with encoder delay */
    if (read_32bitLE(0x0c, sf) != 0)
        goto fail;

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples, loop_start, loop_end);
fail:
    return NULL;
}

/* Edelweiss variation [Sakuna: Of Rice and Ruin (Switch)] */
VGMSTREAM* init_vgmstream_opus_nsopus(STREAMFILE* sf) {
    off_t offset = 0;
    int num_samples = 0, loop_start = 0, loop_end = 0;

    /* checks */
    if (!check_extensions(sf, "nsopus"))
        goto fail;
    if (read_u32be(0x00, sf) != 0x45574E4F) /* "EWNO" */
        goto fail;

    offset = 0x08;
    num_samples = 0; //read_32bitLE(0x08, sf); /* samples without encoder delay? (lower than count) */

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples, loop_start, loop_end);
fail:
    return NULL;
}

/* Square Enix variation [Dragon Quest I-III (Switch)] */
VGMSTREAM* init_vgmstream_opus_sqex(STREAMFILE* sf) {
    off_t offset = 0;
    int num_samples = 0, loop_start = 0, loop_end = 0, loop_flag;

    /* checks */
    /* .wav: default
     * .opus: fake? */
    if (!check_extensions(sf, "wav,lwav,opus,lopus"))
        goto fail;
    if (read_u32be(0x00, sf) != 0x01000000)
        goto fail;
    /* 0x04: channels */
    /* 0x08: data_size */
    offset = read_32bitLE(0x0C, sf);
    num_samples = read_32bitLE(0x1C, sf);

    loop_flag = read_32bitLE(0x18, sf);
    if (loop_flag) {
        loop_start = read_32bitLE(0x14, sf);
        loop_end = read_32bitLE(0x18, sf);
    }

    return init_vgmstream_opus(sf, meta_OPUS, offset, num_samples, loop_start, loop_end);
fail:
    return NULL;
}
