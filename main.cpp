/**
 *  Standard MIDI file player for the eVY1 shield
 *
 *  @author  Toyomasa Watarai
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  This program parse MIDI format 0 files on SDCard
 *  and plays the data using eVY1 shield
 *
 *  Ported by Arduino example code of the SWITCHSCIENCE, Thanks!
 *  https://github.com/SWITCHSCIENCE/eVY1_Shield
 *
 */

#include "mbed.h"
#include <cinttypes>
#include <cstring>
#include <strings.h>
#include "DirectoryList.h"
#include "FATFileSystem.h"
#include "SDBlockDevice.h"

using namespace std::chrono;

SDBlockDevice bd;
FATFileSystem fs("fs");
UnbufferedSerial midi(D1, D0, 31250);
InterruptIn btn(MBED_CONF_APP_FWD_BUTTON);

#if ENABLE_DEBUG_PRINT
#define DEBUG_PRINT(...)    { printf(__VA_ARGS__);}
#else
#define DEBUG_PRINT(...)
#endif

#define DEFAULT_TEMPO_US    500000      // 120 BPM
#define DEBOUNCE_TIME       200ms

FILE *fp;
Timer timer;
Timer btn_timer;

uint32_t track_remaining;   // bytes left in the current MTrk chunk
bool     stream_error;      // EOF, truncated chunk or malformed data
bool     end_of_track;
uint8_t  running_status;

uint32_t tempo_us;          // microseconds per quarter note
uint16_t division;          // ticks per quarter note
uint64_t tempo_base_us;     // time of the last tempo change
uint64_t ticks_since_tempo; // ticks elapsed since the last tempo change
uint64_t next_event_us;

volatile bool skip_requested = false;

/* Returns the next byte of the track, or 0 with stream_error set on failure */
uint8_t midi_read(void)
{
    if (stream_error || track_remaining == 0) {
        stream_error = true;
        return 0;
    }
    int c = fgetc(fp);
    if (c == EOF) {
        stream_error = true;
        return 0;
    }
    track_remaining--;
    return (uint8_t)c;
}

/* Raw read used for the chunk headers, outside of the track bounds */
bool file_read(uint8_t *buf, size_t len)
{
    return fread(buf, 1, len, fp) == len;
}

uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Variable length quantity (max 4 bytes, 28 bits) */
uint32_t vlq_read(void)
{
    uint32_t ret = 0;

    for (int i = 0; i < 4; i++) {
        uint8_t c = midi_read();
        if (stream_error) {
            return 0;
        }
        ret = (ret << 7) | (c & 0x7f);
        if ((c & 0x80) == 0) {
            return ret;
        }
    }
    stream_error = true;
    return 0;
}

void midi_skip(uint32_t len)
{
    for (uint32_t i = 0; i < len && !stream_error; i++) {
        midi_read();
    }
}

void update_next_event_time(void)
{
    next_event_us = tempo_base_us + (ticks_since_tempo * tempo_us) / division;
}

void delta_time_read(void)
{
    uint32_t delta = vlq_read();
    if (stream_error) {
        return;
    }
    ticks_since_tempo += delta;
    update_next_event_time();
}

void all_notes_off(void)
{
    uint8_t buf[6];

    for (uint8_t ch = 0; ch < 16; ch++) {
        buf[0] = (0xB0 | ch);
        buf[1] = 0x78;  // All Sound Off
        buf[2] = 0x00;
        buf[3] = (0xB0 | ch);
        buf[4] = 0x79;  // Reset All Controllers
        buf[5] = 0x00;
        midi.write(buf, 6);
    }
}

void sysex_play(uint8_t status)
{
    uint8_t buf[32];
    uint32_t len = vlq_read();

    if (stream_error) {
        return;
    }
    // 0xF0 is sent before its data, 0xF7 (escape) sends the data as is
    if (status == 0xF0) {
        midi.write(&status, 1);
    }
    while (len > 0 && !stream_error) {
        size_t n = (len < sizeof(buf)) ? len : sizeof(buf);
        for (size_t i = 0; i < n; i++) {
            buf[i] = midi_read();
        }
        if (stream_error) {
            break;
        }
        midi.write(buf, n);
        len -= n;
    }
}

void meta_play(void)
{
    uint8_t type = midi_read();
    uint32_t len = vlq_read();

    if (stream_error) {
        return;
    }

    switch (type) {
        case 0x51: // Set tempo
            if (len != 3) {
                midi_skip(len);
                break;
            }
            tempo_base_us = next_event_us;
            ticks_since_tempo = 0;
            tempo_us = midi_read();
            tempo_us = (tempo_us << 8) | midi_read();
            tempo_us = (tempo_us << 8) | midi_read();
            DEBUG_PRINT("Set tempo = %" PRIu32 " us\n", tempo_us);
            break;
        case 0x2f: // End of Track
            midi_skip(len);
            end_of_track = true;
            break;
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
            DEBUG_PRINT("%d: ", type);
            for (uint32_t i = 0; i < len && !stream_error; i++) {
                char c = midi_read();
                DEBUG_PRINT("%c", c);
                (void)c;
            }
            DEBUG_PRINT("\n");
            break;
        default:
            midi_skip(len);
            break;
    }
}

void midi_play(void)
{
    uint8_t buf[3];
    uint8_t status = midi_read();

    if (stream_error) {
        return;
    }

    if (status == 0xF0 || status == 0xF7) {
        running_status = 0;
        sysex_play(status);
        return;
    }
    if (status == 0xFF) {
        running_status = 0;
        meta_play();
        return;
    }
    if (status >= 0xF0) {
        // System common/realtime messages are not allowed in SMF tracks
        stream_error = true;
        return;
    }

    if (status < 0x80) {
        // Running status: this byte is the first data byte
        if (running_status == 0) {
            stream_error = true;
            return;
        }
        buf[1] = status;
        status = running_status;
    } else {
        running_status = status;
        buf[1] = midi_read();
    }
    buf[0] = status;

    uint8_t cmd = (status & 0xf0);
    size_t len = ((cmd == 0xC0) || (cmd == 0xD0)) ? 2 : 3;
    if (len == 3) {
        buf[2] = midi_read();
    }
    if (stream_error) {
        return;
    }

#if DISABLE_eVocaloid
    if ((buf[0] & 0x0f) == 0x0f) {    // CH.16
        return;
    }
    if ((buf[0] & 0x0f) == 0x00) {    // CH.1
        buf[0] = (buf[0] | 0x0f);     // Force change to CH.16
    }
#endif
    midi.write(buf, len);
}

void smf_main_loop(void)
{
    if (timer.elapsed_time().count() >= (int64_t)next_event_us) {
        midi_play();
        if (!stream_error && !end_of_track) {
            delta_time_read();
        }
    }
}

bool smf_init(void)
{
    uint8_t hdr[8];
    uint8_t data[6];

    stream_error = false;
    end_of_track = false;
    running_status = 0;
    tempo_us = DEFAULT_TEMPO_US;
    tempo_base_us = 0;
    ticks_since_tempo = 0;
    next_event_us = 0;

    all_notes_off();

    // Header chunk
    if (!file_read(hdr, 8) || memcmp(hdr, "MThd", 4) != 0) {
        DEBUG_PRINT("This is not a MIDI file!\n");
        return false;
    }
    uint32_t hdr_len = be32(&hdr[4]);
    if (hdr_len < 6 || !file_read(data, 6)) {
        DEBUG_PRINT("Invalid MIDI header\n");
        return false;
    }
    if (hdr_len > 6 && fseek(fp, hdr_len - 6, SEEK_CUR) != 0) {
        return false;
    }

    uint16_t format = be16(&data[0]);
    uint16_t track = be16(&data[2]);
    division = be16(&data[4]);

    if (format != 0) {
        DEBUG_PRINT("This is not a MIDI format 0 file! (%x)\n", format);
        return false;
    }
    DEBUG_PRINT("Number of tracks : %d\n", track);

    if (division & 0x8000) {
        DEBUG_PRINT("SMPTE time division is not supported\n");
        return false;
    }
    if (division == 0) {
        DEBUG_PRINT("Invalid time division\n");
        return false;
    }
    DEBUG_PRINT("tempo = %" PRIu32 ", division = %d\n", tempo_us, division);

    // Find the track chunk, skipping unknown chunks
    while (true) {
        if (!file_read(hdr, 8)) {
            DEBUG_PRINT("Track chunk not found\n");
            return false;
        }
        uint32_t len = be32(&hdr[4]);
        if (memcmp(hdr, "MTrk", 4) == 0) {
            track_remaining = len;
            break;
        }
        if (fseek(fp, len, SEEK_CUR) != 0) {
            return false;
        }
    }

    delta_time_read();
    DEBUG_PRINT("wait_time = %" PRIu64 " us\n", next_event_us);
    return !stream_error;
}

void skip()
{
    if (btn_timer.elapsed_time() < DEBOUNCE_TIME) {
        return;
    }
    btn_timer.reset();
    skip_requested = true;
}

bool is_midi_file(const char *name)
{
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(&name[len - 4], ".mid") == 0);
}

void play_file(const char *path)
{
    DEBUG_PRINT("%s\n", path);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        DEBUG_PRINT("Unable to read the file \n");
        return;
    }

    if (smf_init()) {
        // Give the eVY1 time to process the reset messages
        thread_sleep_for(100);

        DEBUG_PRINT("Now, playing (%s)... \n", path);
        timer.reset();
        timer.start();
        while (!stream_error && !end_of_track) {
            if (skip_requested) {
                skip_requested = false;
                break;
            }
            smf_main_loop();
        }
        timer.stop();
        if (stream_error) {
            DEBUG_PRINT("Malformed or truncated MIDI data\n");
        }
        all_notes_off();
    }
    fclose(fp);
    DEBUG_PRINT("End.\n\n");
}

int main()
{
    // Wait for the SD card and the eVY1 shield to power up
    thread_sleep_for(MBED_CONF_APP_STARTUP_DELAY_MS);
    DEBUG_PRINT("Initializing...\n");

    int err = fs.mount(&bd);
    DEBUG_PRINT("Filesystem %s\n", (err ? "Fail :(" : "OK"));
    if (err) {
        return -1;
    }

    btn_timer.start();
    btn.mode(PullUp);
    btn.fall(&skip);
    midi.enable_output(true);

#if (DISABLE_eVocaloid == 0)
    const uint8_t aMsg[] = "\xF0\x43\x79\x09\x00\x50\x10" "4 a\0" "\xF7";
    midi.write(aMsg, sizeof(aMsg)-1);
#else
    thread_sleep_for(3500);    // Wait few seconds for booting eVY1-Shield.
#endif

    DEBUG_PRINT("Initialized.\n");
    char path[300];
#if defined(USE_DIRECTORY_LIST)
    DirectoryList   dir((char*)"/fs");
    if ( dir.error_check() ) {
        DEBUG_PRINT("directory could not be opened\r\n");
        return -1;
    }

    for ( size_t i = 0; i < dir.size(); i++ ) {
        if (!is_midi_file(dir[ i ].c_str())) {
            continue;
        }
        int n = snprintf(path, sizeof(path), "/fs/%s", dir[ i ].c_str() );
        if (n < 0 || (size_t)n >= sizeof(path)) {
            DEBUG_PRINT("File name too long: %s\n", dir[ i ].c_str());
            continue;
        }
        play_file(path);
    }
#else
    for ( uint32_t i = 0; i < 10; i++ ) {
        snprintf(path, sizeof(path), "/fs/%" PRIu32 ".mid", i);
        play_file(path);
    }
#endif
    fs.unmount();
    return 0;
}
