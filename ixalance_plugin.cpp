///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Ixalance Playback Plugin
//
// Implements RVPlaybackPlugin interface for IXS (Impulse Tracker eXtendable Sequencer) files using
// webixs by Juergen Wothke, reverse-engineered from Shortcut Software's player.
// Audio output: Stereo S16 at 44100 Hz.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#include "PlayerIXS.h"
#include "PlayerCore.h"
#include "IxsScopeCapture.h"

extern "C" {
#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define IXS_SAMPLE_RATE 44100
#define IXS_CHANNELS 2

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_METADATA_API();
RV_PLUGIN_USE_LOG_API();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct IxalanceData {
    IXS::PlayerIXS* player;
    uint8_t* file_data;
    bool playing;
    // Tracker display: second decompression buffer for non-current patterns
    uint8_t tracker_pattern_buf[64000];
    int tracker_cached_pattern;
    // Scope capture
    IxsScopeCapture* scope_capture;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* ixalance_supported_extensions(void) {
    return "ixs";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void ixalance_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* ixalance_create(const RVService* service_api) {
    (void)service_api;

    IxalanceData* data = (IxalanceData*)calloc(1, sizeof(IxalanceData));
    if (!data) {
        return nullptr;
    }

    data->player = IXS::IXS__PlayerIXS__createPlayer_00405d90(IXS_SAMPLE_RATE);
    if (!data->player) {
        free(data);
        return nullptr;
    }

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int ixalance_destroy(void* user_data) {
    IxalanceData* data = (IxalanceData*)user_data;

    if (data->scope_capture) {
        if (data->player && data->player->ptrCore_0x4) {
            data->player->ptrCore_0x4->scopeCapture = nullptr;
        }
        ixs_scope_capture_destroy(data->scope_capture);
        data->scope_capture = nullptr;
    }
    if (data->player) {
        (*data->player->vftable->delete0)(data->player);
    }
    if (data->file_data) {
        rv_io_free_url_to_memory(data->file_data);
    }
    free(data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult ixalance_probe_can_play(uint8_t* probe_data, uint64_t data_size, const char* url,
                                             uint64_t total_size) {
    (void)total_size;

    // Check for IXS! magic at the beginning of the file
    if (data_size >= 4) {
        if (probe_data[0] == 'I' && probe_data[1] == 'X' && probe_data[2] == 'S' && probe_data[3] == '!') {
            return RVProbeResult_Supported;
        }
    }

    // Fall back to extension check
    if (url != nullptr) {
        const char* dot = strrchr(url, '.');
        if (dot != nullptr && strcasecmp(dot, ".ixs") == 0) {
            return RVProbeResult_Unsure;
        }
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int ixalance_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)subsong;
    (void)service_api;

    IxalanceData* data = (IxalanceData*)user_data;

    // Clean up previous state
    if (data->file_data) {
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
    }
    data->playing = false;

    // Destroy and recreate player for clean state
    if (data->player) {
        (*data->player->vftable->delete0)(data->player);
    }
    data->player = IXS::IXS__PlayerIXS__createPlayer_00405d90(IXS_SAMPLE_RATE);
    if (!data->player) {
        rv_error("IXS: Failed to create player");
        return -1;
    }

    RVIoReadUrlResult read_res;
    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("IXS: Failed to load %s", url);
        return -1;
    }

    data->file_data = (uint8_t*)read_res.data;

    // Load the IXS file data
    char result = (*data->player->vftable->loadIxsFileData)(
        data->player, data->file_data, (uint32_t)read_res.data_size, nullptr, nullptr, nullptr);

    if (result != 0) {
        rv_error("IXS: Failed to load file data from %s", url);
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
        return -1;
    }

    // Initialize audio output
    (*data->player->vftable->initAudioOut)(data->player);
    data->playing = true;
    data->tracker_cached_pattern = -1;

    // Reattach scope capture if it was previously allocated
    if (data->scope_capture) {
        data->player->ptrCore_0x4->scopeCapture = data->scope_capture;
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void ixalance_close(void* user_data) {
    IxalanceData* data = (IxalanceData*)user_data;

    // Detach scope capture from core before player is destroyed
    if (data->player && data->player->ptrCore_0x4) {
        data->player->ptrCore_0x4->scopeCapture = nullptr;
    }

    data->playing = false;
    data->tracker_cached_pattern = -1;

    if (data->file_data) {
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo ixalance_read_data(void* user_data, RVReadData dest) {
    IxalanceData* data = (IxalanceData*)user_data;

    RVAudioFormat format = {RVAudioStreamFormat_S16, IXS_CHANNELS, IXS_SAMPLE_RATE};

    if (!data->playing || !data->player) {
        return (RVReadInfo){format, 0, RVReadStatus_Finished};
    }

    // Check if song has ended
    if ((*data->player->vftable->isSongEnd)(data->player)) {
        data->playing = false;
        return (RVReadInfo){format, 0, RVReadStatus_Finished};
    }

    // Generate one block of audio
    (*data->player->vftable->genAudio)(data->player);

    // Get the generated audio buffer and length
    uint8_t* audio_buf = (*data->player->vftable->getAudioBuffer)(data->player);
    uint32_t num_frames = (*data->player->vftable->getAudioBufferLen)(data->player);

    if (!audio_buf || num_frames == 0) {
        return (RVReadInfo){format, 0, RVReadStatus_Ok};
    }

    // Calculate how many frames we can fit in the output buffer
    uint32_t bytes_per_frame = sizeof(int16_t) * IXS_CHANNELS;
    uint32_t max_frames = dest.channels_output_max_bytes_size / bytes_per_frame;
    uint32_t frames_to_copy = num_frames < max_frames ? num_frames : max_frames;

    memcpy(dest.channels_output, audio_buf, frames_to_copy * bytes_per_frame);

    // Check if song ended after generating
    if ((*data->player->vftable->isSongEnd)(data->player)) {
        data->playing = false;
        return (RVReadInfo){format, frames_to_copy, RVReadStatus_Finished};
    }

    return (RVReadInfo){format, frames_to_copy, RVReadStatus_Ok};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t ixalance_seek(void* user_data, int64_t ms) {
    (void)user_data;
    (void)ms;
    return 0; // No seeking support
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int ixalance_metadata(const char* url, const RVService* service_api) {
    (void)service_api;

    RVIoReadUrlResult read_res;
    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        return -1;
    }

    uint8_t* file_data = (uint8_t*)read_res.data;

    RVMetadataId id = rv_metadata_create_url(url);
    rv_metadata_set_tag(id, RV_METADATA_SONGTYPE_TAG, "IXS");

    // Try to extract song title from the IXS file
    // IXS! magic (4 bytes) + itHeadOffset (4) + offset1 (4) + offset2 (4) + packedLen (4) + outputVolume (4) = 24
    // Then 32 bytes of song title
    if (read_res.data_size >= 56) {
        uint32_t magic = *(uint32_t*)file_data;
        if (magic == 0x21535849) { // "IXS!"
            char title[33];
            memcpy(title, file_data + 24, 32);
            title[32] = '\0';
            // Trim trailing spaces/nulls
            for (int i = 31; i >= 0; i--) {
                if (title[i] == ' ' || title[i] == '\0') {
                    title[i] = '\0';
                } else {
                    break;
                }
            }
            if (title[0] != '\0') {
                rv_metadata_set_tag(id, RV_METADATA_TITLE_TAG, title);
            }
        }
    }

    rv_io_free_url_to_memory(read_res.data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Decompress an IT packed pattern into a flat 5-bytes-per-cell buffer.
// Layout: cell at (row, channel) is at offset (row * 64 + channel) * 5.
// Each cell: [note][instrument][vol_pan][cmd][cmdArg]
static void ixs_decompress_pattern(IXS::ITPatternHead* pat_head, uint8_t* pat_data, uint8_t* out_buf) {
    memset(out_buf, 0, 64000);

    if (!pat_data || !pat_head) {
        return;
    }

    // Initialize vol_pan bytes to 0xFF (empty)
    uint32_t total_cells = (uint32_t)pat_head->rows_0x2 * 64;
    for (uint32_t c = 0; c < total_cells; c++) {
        out_buf[c * 5 + 2] = 0xFF;
    }

    // Decompression state: mask variables and last values per channel
    uint8_t mask_var[64];
    uint8_t last_note[64];
    uint8_t last_ins[64];
    uint8_t last_vol[64];
    uint8_t last_cmd[64];
    uint8_t last_cmd_arg[64];
    memset(mask_var, 0, 64);
    memset(last_note, 0, 64);
    memset(last_ins, 0, 64);
    memset(last_vol, 0xFF, 64);
    memset(last_cmd, 0, 64);
    memset(last_cmd_arg, 0, 64);

    uint32_t row = 0;
    int i = 0;
    while (row < pat_head->rows_0x2) {
        if (pat_data[i] == 0) {
            // End of row
            row++;
            i++;
            continue;
        }

        uint32_t channel = (pat_data[i] - 1) & 63;
        int idx = i + 1;

        if (pat_data[i] & 0x80) {
            mask_var[channel] = pat_data[idx];
            idx++;
        }

        uint32_t cell_offset = (row * 64 + channel) * 5;

        if (mask_var[channel] & 1) {
            last_note[channel] = pat_data[idx];
            out_buf[cell_offset + 0] = last_note[channel];
            idx++;
        }
        if (mask_var[channel] & 2) {
            last_ins[channel] = pat_data[idx];
            out_buf[cell_offset + 1] = last_ins[channel];
            idx++;
        }
        if (mask_var[channel] & 4) {
            last_vol[channel] = pat_data[idx];
            out_buf[cell_offset + 2] = last_vol[channel];
            idx++;
        }
        if (mask_var[channel] & 8) {
            last_cmd[channel] = pat_data[idx];
            last_cmd_arg[channel] = pat_data[idx + 1];
            out_buf[cell_offset + 3] = last_cmd[channel];
            out_buf[cell_offset + 4] = last_cmd_arg[channel];
            idx += 2;
        }
        if (mask_var[channel] & 0x10) {
            out_buf[cell_offset + 0] = last_note[channel];
        }
        if (mask_var[channel] & 0x20) {
            out_buf[cell_offset + 1] = last_ins[channel];
        }
        if (mask_var[channel] & 0x40) {
            out_buf[cell_offset + 2] = last_vol[channel];
        }
        if (mask_var[channel] & 0x80) {
            out_buf[cell_offset + 3] = last_cmd[channel];
            out_buf[cell_offset + 4] = last_cmd_arg[channel];
        }

        i = idx;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int ixs_get_num_channels(IXS::Module* module) {
    // Count channels: scan ChnlPan, bit 7 clear = enabled
    int num_channels = 0;
    for (int i = 0; i < 64; i++) {
        if ((module->impulseHeader_0x0.ChnlPan_0x40[i] & 0x80) == 0) {
            num_channels = i + 1;
        }
    }
    return num_channels;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int ixalance_get_tracker_info(void* user_data, RVTrackerInfo* info) {
    IxalanceData* data = (IxalanceData*)user_data;
    if (!data || !data->player || !info) {
        return -1;
    }

    memset(info, 0, sizeof(*info));

    IXS::PlayerCore* core = data->player->ptrCore_0x4;
    IXS::Module* module = core->ptrModule_0x8;

    info->num_patterns = module->impulseHeader_0x0.PatNum_0x26;
    info->num_channels = (uint8_t)ixs_get_num_channels(module);
    info->num_orders = module->impulseHeader_0x0.OrdNum_0x20;
    info->num_samples = module->impulseHeader_0x0.SmpNum_0x24;
    info->current_pattern = core->order_0x3214;
    // currentRow_0x3216 is incremented after processing each row's data,
    // so it represents the *next* row to process, not the one currently playing.
    info->current_row = core->currentRow_0x3216 > 0 ? core->currentRow_0x3216 - 1 : 0;
    info->current_order = core->ordIdx_0x3215;
    info->channels_synchronized = 1;

    if (core->patternHeadPtr_0x321c) {
        info->rows_per_pattern = core->patternHeadPtr_0x321c->rows_0x2;
    }

    strncpy(info->module_type, "ixs", sizeof(info->module_type) - 1);

    // Song name: 26 bytes starting at songName_0x4 (12 bytes) + unknown_0x10 (14 bytes)
    memcpy(info->song_name, module->impulseHeader_0x0.songName_0x4, 26);
    info->song_name[26] = '\0';
    // Trim trailing spaces/nulls
    for (int i = 25; i >= 0; i--) {
        if (info->song_name[i] == ' ' || info->song_name[i] == '\0') {
            info->song_name[i] = '\0';
        } else {
            break;
        }
    }

    // Sample names
    uint16_t num_samples = info->num_samples;
    if (num_samples > 32) num_samples = 32;
    for (uint16_t i = 0; i < num_samples; i++) {
        if (module->smplHeadPtrArr0_0xd0[i]) {
            strncpy(info->sample_names[i], module->smplHeadPtrArr0_0xd0[i]->name_0x14,
                    sizeof(info->sample_names[i]) - 1);
            info->sample_names[i][sizeof(info->sample_names[i]) - 1] = '\0';
        }
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int ixalance_get_pattern_cell(void* user_data, int pattern, int row, int channel, RVPatternCell* cell) {
    IxalanceData* data = (IxalanceData*)user_data;
    if (!data || !data->player || !cell) {
        return -1;
    }

    IXS::PlayerCore* core = data->player->ptrCore_0x4;
    IXS::Module* module = core->ptrModule_0x8;

    if (pattern < 0 || pattern >= module->impulseHeader_0x0.PatNum_0x26) {
        return -1;
    }

    IXS::ITPatternHead* pat_head = module->patHeadPtrArray_0xd8[pattern];
    if (!pat_head || row < 0 || row >= pat_head->rows_0x2 || channel < 0 || channel >= 64) {
        return -1;
    }

    // Get the 5-byte cell data from the appropriate buffer
    const uint8_t* buf;
    if (pattern == core->order_0x3214 && core->buf16kPtr_0x3220) {
        // Current pattern: use the already-decompressed buffer
        buf = (const uint8_t*)core->buf16kPtr_0x3220->buf_0x0;
    } else {
        // Other pattern: decompress on demand with caching
        if (data->tracker_cached_pattern != pattern) {
            ixs_decompress_pattern(pat_head, module->patDataPtrArray_0xdc[pattern],
                                   data->tracker_pattern_buf);
            data->tracker_cached_pattern = pattern;
        }
        buf = data->tracker_pattern_buf;
    }

    uint32_t offset = ((uint32_t)row * 64 + (uint32_t)channel) * 5;
    uint8_t note = buf[offset + 0];
    uint8_t ins = buf[offset + 1];
    uint8_t vol_pan = buf[offset + 2];
    uint8_t cmd = buf[offset + 3];
    uint8_t cmd_arg = buf[offset + 4];

    // Note mapping: IXS 1-119 -> RV 2-120, IXS 255 -> RV 255 (note off), IXS 254 -> RV 254 (note cut)
    // Note 0 in the buffer is ambiguous (could be C-0 or empty cell from memset); treat as empty
    // since C-0 is virtually never used in IXS files.
    if (note >= 1 && note <= 119) {
        cell->note = note + 1;
    } else if (note == 255) {
        cell->note = 255;
    } else if (note == 254) {
        cell->note = 254;
    } else {
        cell->note = 0;
    }

    cell->instrument = ins;

    // Volume: 0xFF means empty
    cell->volume = (vol_pan == 0xFF) ? 0 : vol_pan;

    // Effect: IT-style cmd 1='A', 2='B', etc. cmd 0 = no effect
    if (cmd >= 1 && cmd <= 26) {
        cell->effect = 'A' + (cmd - 1);
    } else {
        cell->effect = 0;
    }
    cell->effect_param = cmd_arg;
    cell->dest_channel = 0;

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int ixalance_get_pattern_num_rows(void* user_data, int pattern) {
    IxalanceData* data = (IxalanceData*)user_data;
    if (!data || !data->player) {
        return 0;
    }

    IXS::Module* module = data->player->ptrCore_0x4->ptrModule_0x8;

    if (pattern < 0 || pattern >= module->impulseHeader_0x0.PatNum_0x26) {
        return 0;
    }

    IXS::ITPatternHead* pat_head = module->patHeadPtrArray_0xd8[pattern];
    return pat_head ? pat_head->rows_0x2 : 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t ixalance_get_scope_data(void* user_data, int channel, float* buffer, uint32_t num_samples) {
    IxalanceData* data = (IxalanceData*)user_data;
    if (!data || !data->player || !buffer) {
        return 0;
    }

    IXS::PlayerCore* core = data->player->ptrCore_0x4;
    if (!core->ptrMixer_0x3224) {
        return 0;
    }

    // Lazy initialization: allocate scope capture on first call
    if (!data->scope_capture) {
        uint32_t buf_len = core->ptrMixer_0x3224->sampleBuf16Length_0xc;
        data->scope_capture = ixs_scope_capture_create(buf_len);
        if (!data->scope_capture) {
            return 0;
        }
        core->scopeCapture = data->scope_capture;
    }

    if (channel < 0 || channel >= IXS_MAX_SCOPE_CHANNELS) {
        return 0;
    }

    uint32_t available = num_samples;
    if (available > IXS_SCOPE_BUFFER_SIZE) {
        available = IXS_SCOPE_BUFFER_SIZE;
    }

    int wp = data->scope_capture->write_pos[channel];
    int start = (wp - (int)available + IXS_SCOPE_BUFFER_SIZE) & (IXS_SCOPE_BUFFER_SIZE - 1);

    for (uint32_t i = 0; i < available; i++) {
        buffer[i] = data->scope_capture->buffers[channel][(start + i) & (IXS_SCOPE_BUFFER_SIZE - 1)];
    }

    return available;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t ixalance_get_scope_channel_names(void* user_data, const char** names, uint32_t max_channels) {
    IxalanceData* data = (IxalanceData*)user_data;
    if (!data || !data->player) {
        return 0;
    }

    IXS::Module* module = data->player->ptrCore_0x4->ptrModule_0x8;
    uint32_t count = (uint32_t)ixs_get_num_channels(module);

    static char s_name_bufs[64][8];
    if (count > 64) count = 64;
    if (count > max_channels) count = max_channels;

    for (uint32_t i = 0; i < count; i++) {
        snprintf(s_name_bufs[i], sizeof(s_name_bufs[i]), "Ch %u", i + 1);
        names[i] = s_name_bufs[i];
    }

    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void ixalance_event(void* user_data, uint8_t* event_data, uint64_t len) {
    (void)user_data;
    (void)event_data;
    (void)len;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_ixalance_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "ixalance",
    "0.0.1",
    "webixs (Juergen Wothke)",
    ixalance_probe_can_play,
    ixalance_supported_extensions,
    ixalance_create,
    ixalance_destroy,
    ixalance_event,
    ixalance_open,
    ixalance_close,
    ixalance_read_data,
    ixalance_seek,
    ixalance_metadata,
    ixalance_static_init,
    nullptr, // settings_updated
    ixalance_get_tracker_info,
    ixalance_get_pattern_cell,
    ixalance_get_pattern_num_rows,
    ixalance_get_scope_data,
    nullptr, // static_destroy
    ixalance_get_scope_channel_names
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_ixalance_plugin;
}
