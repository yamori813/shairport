/*
 * libao output driver. This file is part of Shairport.
 * Copyright (c) James Laird 2013
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include <stdio.h>
#include <unistd.h>
#include <memory.h>
#include <out123.h>
#include "common.h"
#include "audio.h"

//ao_device *dev = NULL;
out123_handle *dev = NULL;

static void help(void) {
    printf("    -d driver           set the output driver\n"
           "    -o name=value       set an arbitrary ao option\n"
           "    -i id               shorthand for -o id=<id>\n"
           "    -n name             shorthand for -o dev=<name> -o dsp=<name>\n"
          );
}

static int init(int argc, char **argv) {
#if 0
    ao_initialize();
    int driver = ao_default_driver_id();
    ao_option *ao_opts = NULL;

    optind = 1; // optind=0 is equivalent to optind=1 plus special behaviour
    argv--;     // so we shift the arguments to satisfy getopt()
    argc++;
    // some platforms apparently require optreset = 1; - which?
    int opt;
    char *mid;
    while ((opt = getopt(argc, argv, "d:i:n:o:")) > 0) {
        switch (opt) {
            case 'd':
                driver = ao_driver_id(optarg);
                if (driver < 0)
                    die("could not find ao driver %s", optarg);
                break;
            case 'i':
                ao_append_option(&ao_opts, "id", optarg);
                break;
            case 'n':
                ao_append_option(&ao_opts, "dev", optarg);
                // Old libao versions (for example, 0.8.8) only support
                // "dsp" instead of "dev".
                ao_append_option(&ao_opts, "dsp", optarg);
                break;
            case 'o':
                mid = strchr(optarg, '=');
                if (!mid)
                    die("Expected an = in audio option %s", optarg);
                *mid = 0;
                ao_append_option(&ao_opts, optarg, mid+1);
                break;
            default:
                help();
                die("Invalid audio option -%c specified", opt);
        }
    }

    if (optind < argc)
        die("Invalid audio argument: %s", argv[optind]);

    ao_sample_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.bits = 16;
    fmt.rate = 44100;
    fmt.channels = 2;
    fmt.byte_format = AO_FMT_NATIVE;

    dev = ao_open_live(driver, &fmt, ao_opts);
#endif
    dev = out123_new();
    out123_open(dev, NULL, NULL);

    return dev ? 0 : 1;
}

static void deinit(void) {
    if (dev)
        out123_del(dev);
    dev = NULL;
}

static void start(int sample_rate) {
    if (sample_rate != 44100)
        die("unexpected sample rate!");
    out123_start(dev, sample_rate, 2, MPG123_ENC_SIGNED_16);
}

static void play(short buf[], int samples) {
    out123_play(dev, buf, samples * 4);
}

static void stop(void) {
    out123_stop(dev);
}

audio_output audio_out123 = {
    .name = "out123",
    .help = &help,
    .init = &init,
    .deinit = &deinit,
    .start = &start,
    .stop = &stop,
    .play = &play,
    .volume = NULL
};
