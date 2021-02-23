/**
 *  Standard MIDI file player for the eVY1 shield
 *
 *  @author  Toyomasa Watarai
 *
 *  This program parse MIDI format 0 files on SDCard
 *  and plays the data using eVY1 shield
 *
 *  Ported by Arduino example code of the SWITCHSCIENCE, Thanks!
 *  https://github.com/SWITCHSCIENCE/eVY1_Shield
 *
 */

#include "mbed.h"
#include "DirectoryList.h"
#include "FATFileSystem.h"
#include "BlockDevice.h"

BlockDevice *bd = BlockDevice::get_default_instance();
FATFileSystem fs("fs");

RawSerial midi(D1, D0);
InterruptIn btn(MBED_CONF_APP_FWD_BUTTON);

#if ENABLE_DEBUG_PRINT
#define DEBUG_PRINT(...)    { printf(__VA_ARGS__);}
#else
#define DEBUG_PRINT(...)
void error(const char* format, ...) {}
#endif

FILE *fp;
Timer timer;
uint32_t tempo;
uint32_t delta_time;
uint32_t wait_time;
uint32_t force_abort = 0;

#define midi_read() (fgetc(fp))

void disable_timer(void)
{
    timer.stop();
}

uint32_t delta_time_read(void)
{
    uint32_t r_buf;
    uint32_t ret = 0;

    while(1) {
        r_buf = midi_read();
        ret = (ret <<7) | (r_buf & 0x7f);
        if ((r_buf & 0x80) == 0)
            break;
    }

    wait_time += ((ret * tempo) / delta_time);
    return ret;
}

void midi_play(void)
{
    int32_t  buf[3];
    uint32_t cnt;
    uint32_t cmd;

    buf[0] = midi_read();
    buf[1] = midi_read();

    cmd = (buf[0] & 0xf0);
    if ((cmd == 0x80) || (cmd == 0x90) || (cmd == 0xA0) || (cmd == 0xB0) || (cmd == 0xE0)) {
        buf[2] = midi_read();
#if DISABLE_eVocaloid
        if ((buf[0] & 0x0f) == 0x0f) {    // CH.16
            return;
        }
        if ((buf[0] & 0x0f) == 0x00) {    // CH.1
            buf[0] = (buf[0] | 0x0f);     // Force change to CH.16
        }
#endif
        midi.putc(buf[0]);
        midi.putc(buf[1]);
        midi.putc(buf[2]);
    } else if (cmd == 0xC0) {
#if DISABLE_eVocaloid
        if ((buf[0] & 0x0f) == 0x00) {    // CH.1
            buf[0] = (buf[0] | 0x0f);     // Force change to CH.16
        }
#endif
        midi.putc(buf[0]);
        midi.putc(buf[1]);
    } else if (cmd == 0xD0) {
        midi.putc(buf[0]);
        midi.putc(buf[1]);
    } else if (cmd == 0xF0) {
        switch( buf[0] & 0x0F ) {
            case 0x00 : // SysEx
            case 0x07 : // SysEx2
                cnt = buf[1];
                midi.putc(buf[0]);
                for(uint32_t i=1; i<cnt+1; i++) {
                    midi.putc(midi_read());
                }
                break;
            case 0x0f : // Meta event
                switch ( buf[1] ) {
                    case 0x00: // Sequence number
                        midi_read();
                        break;
                    case 0x51: // Set tempo
                        midi_read(); // len (== 3)
                        tempo = midi_read();
                        tempo = (tempo << 8 ) | midi_read();
                        tempo = (tempo << 8 ) | midi_read();
                        tempo = tempo / 1000;
                        // DEBUG_PRINT("Set tempo = %ld\n", tempo);
                        break;
                    case 0x2f: // End of Track
                        midi_read(); // Read zero
                        disable_timer();
                        force_abort = 1;
                        break;
                    case 0x01:
                    case 0x02:
                    case 0x03:
                    case 0x04:
                    case 0x05:
                        DEBUG_PRINT("%d: ", buf[1]);
                        cnt = midi_read(); // len
                        for(uint32_t i=0; i<cnt; i++) {
                            DEBUG_PRINT("%c", midi_read());
                        }
                        DEBUG_PRINT("\n");
                        break;
                    default:
                        cnt = midi_read(); // len
                        for(uint32_t i=0; i<cnt; i++) {
                            midi_read();
                        }
                        break;
                }
                break;
        }
    }
}


void smf_main_loop(void)
{
    if(!force_abort) {
        if (wait_time < (uint32_t)timer.read_ms()) {
            midi_play();
            if (!force_abort)
                delta_time_read();
        }
    }
}

void smf_init(void)
{
    wait_time = 0;
    tempo = 500; // default value

    uint32_t ch;
    for (ch=0; ch<16; ch++) {
        midi.putc(0xB0|ch);
        midi.putc(0x78);
        midi.putc(0x00);
        midi.putc(0xB0|ch);
        midi.putc(0x79);
        midi.putc(0x00);
    }

    // Skip MIDI header
    for (uint32_t i=0; i<8; i++) {
        midi_read();
    }

    uint32_t format;
    format = (midi_read() << 8);
    format |= midi_read();

    if (format > 1) {
        DEBUG_PRINT("This is not a MIDI format 0 file! (%lx)\n", format);
        force_abort = 1;
        return;
    }

    uint32_t track;
    track = (midi_read() << 8);
    track |= midi_read();

    DEBUG_PRINT("Number of tracks : %ld\n", track);

    // timebase
    delta_time = (midi_read() << 8);
    delta_time |= midi_read();
    DEBUG_PRINT("tempo = %ld, delta_time = %ld\n", tempo, delta_time);

    // skip track chunk header
    for (uint32_t i = 0; i < 8; i++) {
        midi_read();
    }

    wait_time = (delta_time_read() * tempo) / delta_time ;
    DEBUG_PRINT("wait_time = %ld\n", wait_time);
    force_abort = 0;
}

void skip()
{
    force_abort = 1;
}

int main()
{
    DEBUG_PRINT("Initializing...\n");
    
    int err = fs.mount(bd);
    DEBUG_PRINT("Filesystem %s\n", (err ? "Fail :(" : "OK"));
    if (err) {
        return -1;
    }

    btn.mode(PullUp);
    btn.fall(&skip);
    midi.baud(31250);

#if (DISABLE_eVocaloid == 0)
    const uint8_t aMsg[] = "\xF0\x43\x79\x09\x00\x50\x10" "4 a\0" "\xF7";
    for (uint32_t i = 0; i < (sizeof(aMsg)-1); i++) {
        midi.putc(aMsg[i]);
    }

#else
    thread_sleep_for(3500);    // Wait few seconds for booting eVY1-Shleld.
#endif

    DEBUG_PRINT("Initialized.\n");

    char buf[50];

#if defined(USE_DIRECTORY_LIST)
    DirectoryList   dir((char*)"/fs");
    if ( dir.error_check() ) {
        DEBUG_PRINT("directory could not be opened\r\n");
        return -1;
    }

    for ( int i = 0; i < (int)dir.size(); i++ ) {
        sprintf(buf, "/fs/%s", dir[ i ].c_str() );
#else
    for ( uint32_t i = 0; i < 10; i++ ) {
        sprintf(buf, "/fs/%ld.mid", i);
#endif
        DEBUG_PRINT("%s\n", buf);
        fp = fopen(buf, "r");
        if (fp == NULL) {
            DEBUG_PRINT("Unable to read the file \n");
        } else {
            timer.reset();
            timer.start();
            smf_init();
            if (force_abort) {
                fclose(fp);
                continue;
            }

            DEBUG_PRINT("Now, playing (%s)... \n", buf);
            while (1) {
                smf_main_loop();
                if (force_abort) {
                    force_abort = 0;
                    break;
                }
            }
            fclose(fp);
            DEBUG_PRINT("End.\n\n");
        }
    }
    fs.unmount();

}
