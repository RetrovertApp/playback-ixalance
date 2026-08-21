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
    // Part of the last generated block that did not fit the previous read
    uint32_t pending_frames;
    uint32_t pending_offset;
    // Fallback decompression buffer when the engine's live pattern buffer is absent
    uint8_t tracker_pattern_buf[64000];
    // Scope capture (allocated/attached by set_scope_enabled)
    IxsScopeCapture* scope_capture;
    bool scope_enabled;
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
    data->pending_frames = 0;
    data->pending_offset = 0;

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

    // Reattach scope capture only if it was enabled; keep capture-side gating in sync with the flag
    if (data->scope_enabled && data->scope_capture) {
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

    // Generate the next block unless part of the previous one is still pending
    if (data->pending_frames == 0) {
        if ((*data->player->vftable->isSongEnd)(data->player)) {
            data->playing = false;
            return (RVReadInfo){format, 0, RVReadStatus_Finished};
        }

        (*data->player->vftable->genAudio)(data->player);

        uint8_t* audio_buf = (*data->player->vftable->getAudioBuffer)(data->player);
        uint32_t num_frames = (*data->player->vftable->getAudioBufferLen)(data->player);
        if (!audio_buf || num_frames == 0) {
            return (RVReadInfo){format, 0, RVReadStatus_Ok};
        }
        data->pending_frames = num_frames;
        data->pending_offset = 0;
    }

    uint32_t bytes_per_frame = sizeof(int16_t) * IXS_CHANNELS;
    uint32_t capacity_frames = dest.channels_output_max_bytes_size / bytes_per_frame;
    uint32_t max_frames = dest.info.frame_count < capacity_frames ? dest.info.frame_count : capacity_frames;
    uint32_t frames_to_copy = data->pending_frames < max_frames ? data->pending_frames : max_frames;

    uint8_t* audio_buf = (*data->player->vftable->getAudioBuffer)(data->player);
    memcpy(dest.channels_output, audio_buf + data->pending_offset * bytes_per_frame,
           frames_to_copy * bytes_per_frame);
    data->pending_offset += frames_to_copy;
    data->pending_frames -= frames_to_copy;

    // Check if song ended after draining the block
    if (data->pending_frames == 0 && (*data->player->vftable->isSongEnd)(data->player)) {
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

// Column schema: note, instrument, volume, effect command, effect parameter.
enum { IXS_COLUMN_COUNT = 5 };

static IXS::Module* ixs_module(IxalanceData* data) {
    if (!data || !data->player || !data->player->ptrCore_0x4) {
        return nullptr;
    }
    return data->player->ptrCore_0x4->ptrModule_0x8;
}

static bool ixalance_get_structure(void* user_data, RVVizInfo* out) {
    IxalanceData* data = (IxalanceData*)user_data;
    IXS::Module* module = ixs_module(data);
    if (!module || !out) {
        return false;
    }
    uint32_t ch = (uint32_t)ixs_get_num_channels(module);
    out->caps = RVVizCaps_PatternCells | RVVizCaps_Scope | RVVizCaps_WholeSongKnown;
    out->scroll_mode = RVScrollMode_Synchronized;
    out->pattern_channel_count = ch;
    out->scope_channel_count = ch;
    out->column_count = IXS_COLUMN_COUNT;
    return true;
}

static uint32_t ixalance_get_columns(void* user_data, RVColumnDesc* out, uint32_t cap) {
    (void)user_data;
    static const struct {
        const char* label;
        uint8_t width;
        RVColumnKind kind;
    } cols[IXS_COLUMN_COUNT] = {
        {"Note", 3, RVColumnKind_Note}, {"Inst", 2, RVColumnKind_Instrument}, {"Vol", 3, RVColumnKind_Volume},
        {"Eff", 1, RVColumnKind_Effect}, {"Prm", 2, RVColumnKind_Param},
    };
    uint32_t n = cap < IXS_COLUMN_COUNT ? cap : IXS_COLUMN_COUNT;
    for (uint32_t i = 0; i < n; i++) {
        memset(out[i].label, 0, sizeof(out[i].label));
        strncpy((char*)out[i].label, cols[i].label, sizeof(out[i].label) - 1);
        out[i].char_width = cols[i].width;
        out[i].kind = cols[i].kind;
    }
    return n;
}

static uint32_t ixalance_fill_channels(IxalanceData* data, RVChannelDesc* out, uint32_t cap) {
    IXS::Module* module = ixs_module(data);
    if (!module || !out) {
        return 0;
    }
    uint32_t count = (uint32_t)ixs_get_num_channels(module);
    if (count > cap) {
        count = cap;
    }
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        snprintf((char*)out[i].name, sizeof(out[i].name), "Ch %u", i + 1);
        out[i].scope_width = 0; // mono
    }
    return count;
}

static uint32_t ixalance_get_pattern_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    return ixalance_fill_channels((IxalanceData*)user_data, out, cap);
}

static uint32_t ixalance_get_scope_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    return ixalance_fill_channels((IxalanceData*)user_data, out, cap);
}

static bool ixalance_get_position(void* user_data, RVTrackerPosition* out) {
    IxalanceData* data = (IxalanceData*)user_data;
    if (!ixs_module(data) || !out) {
        return false;
    }
    IXS::PlayerCore* core = data->player->ptrCore_0x4;
    out->order = core->ordIdx_0x3215;
    out->pattern = core->order_0x3214;
    // currentRow_0x3216 is incremented after processing each row's data,
    // so it represents the *next* row to process, not the one currently playing.
    out->row = core->currentRow_0x3216 > 0 ? core->currentRow_0x3216 - 1 : 0;
    out->window_lo = 0;
    out->window_hi = core->patternHeadPtr_0x321c ? core->patternHeadPtr_0x321c->rows_0x2 : 0;
    return true;
}

static uint32_t ixalance_get_channel_rows(void* user_data, uint32_t* out, uint32_t cap) {
    (void)user_data;
    (void)out;
    (void)cap;
    return 0; // Synchronized: window comes from get_position
}

static const char s_note_names[12][3] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};

// Render one 5-byte IXS cell (note, instrument, vol/pan, command, command arg) into the column's
// raw value + fixed-width text. `col` is the column index in the schema declared by get_columns.
static void ixs_render_cell(RVPatternCell* out, int col, const uint8_t* src) {
    uint8_t note = src[0], ins = src[1], vol = src[2], cmd = src[3], arg = src[4];
    memset(out->text, 0, sizeof(out->text));
    switch (col) {
        case 0: // Note — engine stores IT note (0..119 = C-0..B-9), 254 cut, 255 off, 0 empty
            out->raw = note;
            if (note >= 1 && note <= 119)
                snprintf((char*)out->text, sizeof(out->text), "%s%u", s_note_names[note % 12], note / 12);
            else if (note == 255)
                strncpy((char*)out->text, "===", sizeof(out->text) - 1); // note off
            else if (note == 254)
                strncpy((char*)out->text, "^^^", sizeof(out->text) - 1); // note cut
            else
                strncpy((char*)out->text, "...", sizeof(out->text) - 1); // empty
            break;
        case 1: // Instrument
            out->raw = ins;
            if (ins == 0)
                strncpy((char*)out->text, "..", sizeof(out->text) - 1);
            else
                snprintf((char*)out->text, sizeof(out->text), "%02u", ins);
            break;
        case 2: // Volume/pan — 0xFF = empty
            out->raw = vol;
            if (vol == 0xFF)
                strncpy((char*)out->text, "...", sizeof(out->text) - 1);
            else
                snprintf((char*)out->text, sizeof(out->text), "%3u", vol);
            break;
        case 3: // Effect command — raw = command byte, text = IT letter (1='A')
            out->raw = cmd;
            out->text[0] = (cmd >= 1 && cmd <= 26) ? (uint8_t)('A' + (cmd - 1)) : (uint8_t)'.';
            break;
        default: // 4: Effect parameter
            out->raw = arg;
            if (cmd == 0)
                strncpy((char*)out->text, "..", sizeof(out->text) - 1);
            else
                snprintf((char*)out->text, sizeof(out->text), "%02X", arg);
            break;
    }
}

static uint32_t ixalance_get_cells(void* user_data, int32_t channel, uint32_t row_lo, uint32_t row_hi, RVPatternCell* out,
                                   uint32_t cap) {
    IxalanceData* data = (IxalanceData*)user_data;
    IXS::Module* module = ixs_module(data);
    if (!module || !out) {
        return 0;
    }
    IXS::PlayerCore* core = data->player->ptrCore_0x4;
    int pattern = core->order_0x3214; // synchronized: cells come from the current pattern
    if (pattern < 0 || pattern >= module->impulseHeader_0x0.PatNum_0x26) {
        return 0;
    }
    IXS::ITPatternHead* pat_head = module->patHeadPtrArray_0xd8[pattern];
    if (!pat_head) {
        return 0;
    }

    // Prefer the engine's live decompressed buffer; fall back to decompressing on demand.
    const uint8_t* buf;
    if (core->buf16kPtr_0x3220) {
        buf = (const uint8_t*)core->buf16kPtr_0x3220->buf_0x0;
    } else {
        ixs_decompress_pattern(pat_head, module->patDataPtrArray_0xdc[pattern], data->tracker_pattern_buf);
        buf = data->tracker_pattern_buf;
    }

    uint32_t num_rows = pat_head->rows_0x2;
    if (row_hi > num_rows) {
        row_hi = num_rows;
    }
    int num_ch = ixs_get_num_channels(module);
    int ch_start = channel < 0 ? 0 : channel;
    int ch_end = channel < 0 ? num_ch : channel + 1;
    if (ch_start >= num_ch) {
        return 0;
    }

    uint32_t written = 0;
    for (uint32_t row = row_lo; row < row_hi; row++) {
        for (int ch = ch_start; ch < ch_end; ch++) {
            const uint8_t* src = &buf[((uint32_t)row * 64 + (uint32_t)ch) * 5];
            for (int c = 0; c < IXS_COLUMN_COUNT; c++) {
                if (written >= cap) {
                    return written;
                }
                ixs_render_cell(&out[written++], c, src);
            }
        }
    }
    return written;
}

static void ixalance_set_scope_enabled(void* user_data, bool on) {
    IxalanceData* data = (IxalanceData*)user_data;
    if (!data || !data->player || !data->player->ptrCore_0x4) {
        return;
    }
    IXS::PlayerCore* core = data->player->ptrCore_0x4;
    if (on) {
        if (!data->scope_capture) {
            if (!core->ptrMixer_0x3224) {
                return; // mixer not ready yet; cannot size the capture buffer
            }
            uint32_t buf_len = core->ptrMixer_0x3224->sampleBuf16Length_0xc;
            data->scope_capture = ixs_scope_capture_create(buf_len);
            if (!data->scope_capture) {
                return;
            }
        }
        core->scopeCapture = data->scope_capture;
        data->scope_enabled = true;
    } else {
        core->scopeCapture = nullptr;
        data->scope_enabled = false;
    }
}

static uint32_t ixalance_get_scope_samples(void* user_data, int32_t channel, float* out, uint32_t cap) {
    IxalanceData* data = (IxalanceData*)user_data;
    if (!data || !data->scope_enabled || !data->scope_capture || !out) {
        return 0; // silent until set_scope_enabled(true)
    }
    if (channel < 0 || channel >= IXS_MAX_SCOPE_CHANNELS) {
        return 0;
    }

    uint32_t available = cap;
    if (available > IXS_SCOPE_BUFFER_SIZE) {
        available = IXS_SCOPE_BUFFER_SIZE;
    }

    int wp = data->scope_capture->write_pos[channel];
    int start = (wp - (int)available + IXS_SCOPE_BUFFER_SIZE) & (IXS_SCOPE_BUFFER_SIZE - 1);

    for (uint32_t i = 0; i < available; i++) {
        out[i] = data->scope_capture->buffers[channel][(start + i) & (IXS_SCOPE_BUFFER_SIZE - 1)];
    }

    return available;
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
    nullptr, // static_destroy
    ixalance_get_structure,
    ixalance_get_columns,
    ixalance_get_pattern_channels,
    ixalance_get_scope_channels,
    ixalance_get_position,
    ixalance_get_channel_rows,
    ixalance_get_cells,
    ixalance_set_scope_enabled,
    ixalance_get_scope_samples,
    nullptr, // get_vu
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_ixalance_plugin;
}
