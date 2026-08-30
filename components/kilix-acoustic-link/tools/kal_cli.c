/*
 * kilix-acoustic-link — diagnostic tool.
 *
 * SPDX-License-Identifier: MIT
 *
 * Device access is refused unless the operator passes --allow-audio AND sets
 * KILIX_ACOUSTIC_LINK_ALLOW_AUDIO=1. F111's physical-qualification
 * authorization is not granted, so on this workspace every device-backed
 * command must refuse rather than make a sound. File-backed PCM commands
 * work offline and emit nothing.
 */
#include "kilix_acoustic_link.h"
#include "kal_audio.h"
#include "kal_channel.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLI_PCM_CHUNK 4096u
#define CLI_MAX_INPUT_BYTES KAL_MAX_MESSAGE_BYTES

struct cli_options {
    const char *command;
    const char *text;
    const char *input;
    const char *output;
    const char *input_pcm;
    const char *output_pcm;
    const char *device;
    const char *hex;
    uint8_t profile;
    uint16_t window;
    uint8_t role;
    uint8_t allow_audio;
    uint8_t allow_high_frequency;
    uint8_t profile_selected;
    unsigned int seconds;
};

static void usage(FILE *stream) {
    fprintf(stream,
        "kilix-acoustic-link %s\n"
        "\n"
        "usage: kilix-acoustic-link <command> [options]\n"
        "\n"
        "commands:\n"
        "  send        --text TEXT | --input FILE  [--output-pcm FILE]\n"
        "  receive     --input-pcm FILE [--output FILE]\n"
        "  beacon      --text TEXT [--output-pcm FILE]\n"
        "  listen      [--seconds N]\n"
        "  calibrate   [--show]\n"
        "  loopback    --text TEXT [--output-pcm FILE]\n"
        "  channel     [--profile NAME] [--seconds TRIALS]\n"
        "  probe\n"
        "  dump-frame  --hex <128 hex characters>\n"
        "  version\n"
        "\n"
        "options:\n"
        "  --profile NAME     audible-normal|audible-fast|audible-fastest|\n"
        "                     dt-normal|dt-fast|high-normal\n"
        "  --window N         1..16 frames\n"
        "  --role NAME        initiator|responder\n"
        "  --device NAME      capture/playback device passed to the backend\n"
        "  --allow-audio      permit opening a real device (also needs\n"
        "                     KILIX_ACOUSTIC_LINK_ALLOW_AUDIO=1)\n"
        "  --allow-high-frequency  permit the high-frequency profile\n"
        "\n"
        "The high-frequency profile is high-frequency, not inaudible.\n"
        "Sound over this channel is observable, recordable and replayable;\n"
        "distance and directionality do not authenticate a peer.\n",
        kal_version_string());
}

static int parse_profile(const char *name, uint8_t *out) {
    uint8_t index;
    for (index = 0u; index < (uint8_t)KAL_PROFILE_COUNT; ++index) {
        if (strcmp(name, kal_profile_string(index)) == 0) {
            *out = index;
            return 0;
        }
    }
    return -1;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int audio_permitted(const struct cli_options *options) {
    const char *env = getenv("KILIX_ACOUSTIC_LINK_ALLOW_AUDIO");
    if (options->allow_audio == 0u) {
        return 0;
    }
    return env != NULL && strcmp(env, "1") == 0;
}

static void refuse_audio(const char *what) {
    fprintf(stderr,
        "refusing to %s: device access needs --allow-audio and\n"
        "KILIX_ACOUSTIC_LINK_ALLOW_AUDIO=1.\n"
        "F111 physical qualification is not authorized on this workspace, so\n"
        "this build must not open a capture or playback device.\n", what);
}

static size_t read_file(const char *path, uint8_t *buffer, size_t capacity,
                        int *too_large) {
    FILE *file = fopen(path, "rb");
    size_t got;
    int extra;

    *too_large = 0;
    if (file == NULL) {
        return (size_t)-1;
    }
    got = fread(buffer, 1u, capacity, file);
    extra = fgetc(file);
    if (extra != EOF) {
        *too_large = 1;
    }
    (void)fclose(file);
    return got;
}

static int cmd_probe(const struct cli_options *options) {
    uint8_t profile;
    const char *backend = "none";

    printf("kilix-acoustic-link %s\n", kal_version_string());
    printf("wire KAL1 version 1 CANDIDATE (not frozen)\n");
    printf("frame bytes %u header %u payload %u crc %u\n",
           KAL_FRAME_BYTES, KAL_HEADER_BYTES, KAL_PAYLOAD_BYTES, KAL_CRC_BYTES);
    printf("max message bytes %u max segments %u\n",
           KAL_MAX_MESSAGE_BYTES, KAL_MAX_SEGMENTS);
    printf("profile,frame_samples,airtime_ms,graduated\n");
    for (profile = 0u; profile < (uint8_t)KAL_PROFILE_COUNT; ++profile) {
        kal_options create;
        kal_link *link = NULL;
        kal_stats stats;
        memset(&create, 0, sizeof create);
        create.sample_rate = KAL_SAMPLE_RATE;
        create.profile = profile;
        create.role = (uint8_t)KAL_ROLE_INITIATOR;
        create.allow_high_frequency = 1u;
        if (kal_link_create(&link, &create) != KAL_OK) {
            printf("%s,unavailable,unavailable,0/1\n", kal_profile_string(profile));
            continue;
        }
        kal_get_stats(link, &stats);
        /* No profile is graduated: graduation is a physical measurement and
         * none has been produced. */
        printf("%s,%llu,%llu,0/1\n", kal_profile_string(profile),
               (unsigned long long)(stats.frame_airtime_ms * KAL_SAMPLE_RATE / 1000u),
               (unsigned long long)stats.frame_airtime_ms);
        kal_link_free(link);
    }
    (void)kal_audio_backend_argv(KAL_AUDIO_CAPTURE, KAL_SAMPLE_RATE, NULL, &backend);
    printf("capture backend %s\n", backend);
    (void)kal_audio_backend_argv(KAL_AUDIO_PLAYBACK, KAL_SAMPLE_RATE, NULL, &backend);
    printf("playback backend %s\n", backend);
    printf("device access permitted %d/1\n", audio_permitted(options));
    printf("physical profiles graduated 0/6\n");
    return 0;
}

static int cmd_dump_frame(const struct cli_options *options) {
    uint8_t frame[KAL_FRAME_BYTES];
    char text[256];
    size_t index;
    size_t length;

    if (options->hex == NULL) {
        fprintf(stderr, "dump-frame needs --hex\n");
        return 2;
    }
    length = strlen(options->hex);
    if (length != KAL_FRAME_BYTES * 2u) {
        fprintf(stderr, "dump-frame needs exactly %u hex characters, got %zu\n",
                KAL_FRAME_BYTES * 2u, length);
        return 2;
    }
    for (index = 0u; index < KAL_FRAME_BYTES; ++index) {
        const int high = hex_value(options->hex[index * 2u]);
        const int low = hex_value(options->hex[index * 2u + 1u]);
        if (high < 0 || low < 0) {
            fprintf(stderr, "dump-frame: not hexadecimal at character %zu\n",
                    index * 2u);
            return 2;
        }
        frame[index] = (uint8_t)((high << 4) | low);
    }
    if (kal_describe_frame(frame, KAL_FRAME_BYTES, text, sizeof text) == KAL_OK) {
        printf("%s\n", text);
        return 0;
    }
    printf("%s\n", text);
    return 1;
}

/* Modulate a message to a PCM file, or refuse to touch a device. */
static int cmd_send(const struct cli_options *options) {
    uint8_t message[CLI_MAX_INPUT_BYTES];
    size_t message_size = 0u;
    kal_options create;
    kal_link *link = NULL;
    FILE *output = NULL;
    uint64_t now = 0u;
    kal_result result;
    unsigned long long frames = 0u;
    kal_stats stats;

    if (options->text != NULL) {
        message_size = strlen(options->text);
        if (message_size > sizeof message) {
            fprintf(stderr, "send: text above the %u-byte limit\n",
                    (unsigned int)CLI_MAX_INPUT_BYTES);
            return 2;
        }
        memcpy(message, options->text, message_size);
    } else if (options->input != NULL) {
        int too_large = 0;
        message_size = read_file(options->input, message, sizeof message, &too_large);
        if (message_size == (size_t)-1) {
            fprintf(stderr, "send: cannot read %s: %s\n", options->input,
                    strerror(errno));
            return 2;
        }
        if (too_large) {
            fprintf(stderr, "send: %s is above the %u-byte limit\n",
                    options->input, (unsigned int)CLI_MAX_INPUT_BYTES);
            return 2;
        }
    } else {
        fprintf(stderr, "send needs --text or --input\n");
        return 2;
    }

    if (options->output_pcm == NULL) {
        if (!audio_permitted(options)) {
            refuse_audio("transmit to a playback device");
            return 3;
        }
        fprintf(stderr, "send: device transmission is authorized but not "
                        "exercised by this build\n");
        return 3;
    }

    memset(&create, 0, sizeof create);
    create.sample_rate = KAL_SAMPLE_RATE;
    create.profile = options->profile;
    create.window_frames = options->window;
    create.role = options->role;
    create.allow_high_frequency = options->allow_high_frequency;
    result = kal_link_create(&link, &create);
    if (result != KAL_OK) {
        fprintf(stderr, "send: link create failed: %s\n", kal_result_string(result));
        return 1;
    }
    result = kal_send(link, message, message_size);
    if (result != KAL_OK) {
        fprintf(stderr, "send: %s\n", kal_result_string(result));
        kal_link_free(link);
        return 1;
    }
    output = fopen(options->output_pcm, "wb");
    if (output == NULL) {
        fprintf(stderr, "send: cannot write %s: %s\n", options->output_pcm,
                strerror(errno));
        kal_link_free(link);
        return 1;
    }
    for (;;) {
        int16_t pcm[CLI_PCM_CHUNK];
        size_t written = 0u;
        const kal_result pulled =
            kal_tx_pull_s16(link, pcm, CLI_PCM_CHUNK, &written, now);
        if (pulled != KAL_HAVE_OUTPUT || written == 0u) {
            break;
        }
        if (fwrite(pcm, sizeof(int16_t), written, output) != written) {
            fprintf(stderr, "send: short write to %s\n", options->output_pcm);
            (void)fclose(output);
            kal_link_free(link);
            return 1;
        }
        frames += written;
        now += (written * 1000u) / KAL_SAMPLE_RATE;
    }
    (void)fclose(output);
    kal_get_stats(link, &stats);
    printf("message bytes %zu frames sent %u samples %llu profile %s\n",
           message_size, stats.frames_sent, frames,
           kal_profile_string(options->profile));
    kal_link_free(link);
    return 0;
}

static int cmd_receive(const struct cli_options *options) {
    kal_options create;
    kal_link *link = NULL;
    FILE *input = NULL;
    uint64_t now = 0u;
    uint8_t message[CLI_MAX_INPUT_BYTES];
    size_t message_size = 0u;
    kal_result result;
    int delivered = 0;

    if (options->input_pcm == NULL) {
        if (!audio_permitted(options)) {
            refuse_audio("capture from a recording device");
            return 3;
        }
        fprintf(stderr, "receive: device capture is authorized but not "
                        "exercised by this build\n");
        return 3;
    }
    memset(&create, 0, sizeof create);
    create.sample_rate = KAL_SAMPLE_RATE;
    create.profile = options->profile;
    create.window_frames = options->window;
    create.role = options->role;
    create.allow_high_frequency = options->allow_high_frequency;
    result = kal_link_create(&link, &create);
    if (result != KAL_OK) {
        fprintf(stderr, "receive: link create failed: %s\n",
                kal_result_string(result));
        return 1;
    }
    input = fopen(options->input_pcm, "rb");
    if (input == NULL) {
        fprintf(stderr, "receive: cannot read %s: %s\n", options->input_pcm,
                strerror(errno));
        kal_link_free(link);
        return 1;
    }
    for (;;) {
        int16_t pcm[CLI_PCM_CHUNK];
        const size_t got = fread(pcm, sizeof(int16_t), CLI_PCM_CHUNK, input);
        if (got == 0u) {
            break;
        }
        (void)kal_rx_push_s16(link, pcm, got, now);
        now += (got * 1000u) / KAL_SAMPLE_RATE;
        if (kal_receive(link, message, sizeof message, &message_size) == KAL_OK) {
            delivered = 1;
            break;
        }
    }
    (void)fclose(input);
    if (!delivered) {
        kal_stats stats;
        kal_get_stats(link, &stats);
        fprintf(stderr, "receive: no message delivered "
                        "(frames received %u, rejected %u, crc failures %u)\n",
                stats.frames_received, stats.frames_rejected, stats.crc_failures);
        kal_link_free(link);
        return 1;
    }
    if (options->output != NULL) {
        FILE *out = fopen(options->output, "wb");
        if (out == NULL) {
            fprintf(stderr, "receive: cannot write %s: %s\n", options->output,
                    strerror(errno));
            kal_link_free(link);
            return 1;
        }
        (void)fwrite(message, 1u, message_size, out);
        (void)fclose(out);
    }
    /* Payload content is never written to a log stream by default. */
    printf("delivered bytes %zu\n", message_size);
    kal_link_free(link);
    return 0;
}

static int cmd_beacon(const struct cli_options *options) {
    kal_options create;
    kal_link *link = NULL;
    FILE *output;
    uint64_t now = 0u;
    size_t length;
    kal_result result;

    if (options->text == NULL) {
        fprintf(stderr, "beacon needs --text\n");
        return 2;
    }
    length = strlen(options->text);
    if (length == 0u || length > KAL_MAX_BEACON_BYTES) {
        fprintf(stderr, "beacon: text must be 1..%u bytes\n", KAL_MAX_BEACON_BYTES);
        return 2;
    }
    if (options->output_pcm == NULL) {
        if (!audio_permitted(options)) {
            refuse_audio("transmit a beacon to a playback device");
            return 3;
        }
        fprintf(stderr, "beacon: device transmission is authorized but not "
                        "exercised by this build\n");
        return 3;
    }
    memset(&create, 0, sizeof create);
    create.sample_rate = KAL_SAMPLE_RATE;
    create.profile = options->profile;
    create.role = options->role;
    create.allow_high_frequency = options->allow_high_frequency;
    result = kal_link_create(&link, &create);
    if (result != KAL_OK) {
        fprintf(stderr, "beacon: link create failed: %s\n",
                kal_result_string(result));
        return 1;
    }
    result = kal_beacon(link, (const uint8_t *)options->text, length);
    if (result != KAL_OK) {
        fprintf(stderr, "beacon: %s\n", kal_result_string(result));
        kal_link_free(link);
        return 1;
    }
    output = fopen(options->output_pcm, "wb");
    if (output == NULL) {
        fprintf(stderr, "beacon: cannot write %s\n", options->output_pcm);
        kal_link_free(link);
        return 1;
    }
    for (;;) {
        int16_t pcm[CLI_PCM_CHUNK];
        size_t written = 0u;
        if (kal_tx_pull_s16(link, pcm, CLI_PCM_CHUNK, &written, now)
                != KAL_HAVE_OUTPUT || written == 0u) {
            break;
        }
        (void)fwrite(pcm, sizeof(int16_t), written, output);
        now += (written * 1000u) / KAL_SAMPLE_RATE;
    }
    (void)fclose(output);
    printf("beacon bytes %zu profile %s\n", length,
           kal_profile_string(options->profile));
    kal_link_free(link);
    return 0;
}

static int cmd_listen(const struct cli_options *options) {
    if (!audio_permitted(options)) {
        refuse_audio("listen on a capture device");
        return 3;
    }
    fprintf(stderr, "listen: device capture is authorized but not exercised "
                    "by this build\n");
    return 3;
}

static int calibration_path(char *buffer, size_t capacity) {
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (state != NULL && state[0] != '\0') {
        return snprintf(buffer, capacity, "%s/kilix-acoustic-link", state) > 0 ? 0 : -1;
    }
    if (home == NULL) {
        return -1;
    }
    return snprintf(buffer, capacity, "%s/.local/state/kilix-acoustic-link", home) > 0
        ? 0 : -1;
}

static int cmd_calibrate(const struct cli_options *options) {
    char directory[512];
    char path[640];
    FILE *file;

    if (calibration_path(directory, sizeof directory) != 0) {
        fprintf(stderr, "calibrate: no XDG state directory\n");
        return 1;
    }
    if (snprintf(path, sizeof path, "%s/calibration.txt", directory) < 0) {
        return 1;
    }
    if (options->input != NULL && strcmp(options->input, "--show") == 0) {
        return 0;
    }
    if (!audio_permitted(options)) {
        /* Calibration measures a real device by definition. Without the
         * authorization it writes the schema and records that the measured
         * fields are absent, rather than inventing them. */
        if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "calibrate: cannot create %s: %s\n", directory,
                    strerror(errno));
            return 1;
        }
        file = fopen(path, "w");
        if (file == NULL) {
            fprintf(stderr, "calibrate: cannot write %s\n", path);
            return 1;
        }
        fprintf(file,
                "schema 1\n"
                "component kilix-acoustic-link %s\n"
                "profile %s\n"
                "device %s\n"
                "measured_records 0/5\n"
                "loopback_latency_ms unmeasured\n"
                "gain unmeasured\n"
                "success_rate unmeasured\n"
                "graduated 0/1\n"
                "note calibration is not proof of security or proximity\n",
                kal_version_string(), kal_profile_string(options->profile),
                options->device == NULL ? "unselected" : options->device);
        (void)fclose(file);
        printf("wrote %s with measured records 0/5\n", path);
        refuse_audio("measure a real device");
        return 3;
    }
    fprintf(stderr, "calibrate: device measurement is authorized but not "
                    "exercised by this build\n");
    return 3;
}


/*
 * Two links in one process, over the real modem, with the PCM carried in
 * memory. Nothing is played and nothing is recorded: this is the offline
 * end-to-end path that a device-backed run would take.
 */
static int cmd_loopback(const struct cli_options *options) {
    kal_options initiator;
    kal_options responder;
    kal_link *side[2] = {NULL, NULL};
    FILE *output = NULL;
    uint8_t message[CLI_MAX_INPUT_BYTES];
    uint8_t received[CLI_MAX_INPUT_BYTES];
    size_t message_size;
    size_t received_size = 0u;
    uint64_t now = 0u;
    unsigned int step;
    int delivered = 0;
    kal_result result;
    kal_stats stats_tx;
    kal_stats stats_rx;
    int16_t *pcm = NULL;
    size_t pcm_capacity;

    if (options->text == NULL) {
        fprintf(stderr, "loopback needs --text\n");
        return 2;
    }
    message_size = strlen(options->text);
    if (message_size == 0u || message_size > sizeof message) {
        fprintf(stderr, "loopback: text must be 1..%u bytes\n",
                (unsigned int)CLI_MAX_INPUT_BYTES);
        return 2;
    }
    memcpy(message, options->text, message_size);

    memset(&initiator, 0, sizeof initiator);
    initiator.sample_rate = KAL_SAMPLE_RATE;
    initiator.profile = options->profile;
    initiator.window_frames = options->window;
    initiator.allow_high_frequency = options->allow_high_frequency;
    responder = initiator;
    initiator.role = (uint8_t)KAL_ROLE_INITIATOR;
    responder.role = (uint8_t)KAL_ROLE_RESPONDER;

    result = kal_link_create(&side[0], &initiator);
    if (result == KAL_OK) {
        result = kal_link_create(&side[1], &responder);
    }
    if (result != KAL_OK) {
        fprintf(stderr, "loopback: link create failed: %s\n",
                kal_result_string(result));
        kal_link_free(side[0]);
        kal_link_free(side[1]);
        return 1;
    }
    kal_get_stats(side[0], &stats_tx);
    pcm_capacity = (size_t)stats_tx.frame_airtime_ms * KAL_SAMPLE_RATE / 1000u
        + KAL_SAMPLE_RATE;
    pcm = (int16_t *)calloc(pcm_capacity, sizeof(int16_t));
    if (pcm == NULL) {
        kal_link_free(side[0]);
        kal_link_free(side[1]);
        return 1;
    }
    if (options->output_pcm != NULL) {
        output = fopen(options->output_pcm, "wb");
        if (output == NULL) {
            fprintf(stderr, "loopback: cannot write %s\n", options->output_pcm);
            free(pcm);
            kal_link_free(side[0]);
            kal_link_free(side[1]);
            return 1;
        }
    }

    result = kal_send(side[0], message, message_size);
    if (result != KAL_OK) {
        fprintf(stderr, "loopback: send failed: %s\n", kal_result_string(result));
        goto finish;
    }
    for (step = 0u; step < 512u && !delivered; ++step) {
        int direction;
        int moved = 0;
        for (direction = 0; direction < 2; ++direction) {
            size_t written = 0u;
            const kal_result pulled = kal_tx_pull_s16(side[direction], pcm,
                                                      pcm_capacity, &written, now);
            if (pulled != KAL_HAVE_OUTPUT || written == 0u) {
                continue;
            }
            moved = 1;
            if (getenv("KILIX_ACOUSTIC_LINK_TRACE") != NULL) {
                fprintf(stderr, "trace t=%llums dir=%d samples=%zu\n",
                        (unsigned long long)now, direction, written);
            }
            if (output != NULL) {
                (void)fwrite(pcm, sizeof(int16_t), written, output);
            }
            (void)kal_rx_push_s16(side[direction == 0 ? 1 : 0], pcm, written, now);
            now += (written * 1000u) / KAL_SAMPLE_RATE;
        }
        if (kal_receive(side[1], received, sizeof received, &received_size) == KAL_OK) {
            delivered = 1;
        }
        if (!moved && !delivered) {
            uint64_t deadline = 0u;
            if (kal_next_deadline(side[0], &deadline) && deadline > now) {
                now = deadline;
                (void)kal_tick(side[0], now);
                (void)kal_tick(side[1], now);
            } else {
                break;
            }
        }
    }

finish:
    if (output != NULL) {
        (void)fclose(output);
    }
    kal_get_stats(side[0], &stats_tx);
    kal_get_stats(side[1], &stats_rx);
    printf("profile %s\n", kal_profile_string(options->profile));
    printf("message bytes %zu delivered %d/1 match %d/1\n", message_size,
           delivered,
           (delivered && received_size == message_size
            && memcmp(received, message, message_size) == 0) ? 1 : 0);
    printf("frames sent %u received %u retransmitted %u rejected %u\n",
           stats_tx.frames_sent, stats_rx.frames_received,
           stats_tx.frames_retransmitted, stats_rx.frames_rejected);
    printf("timeouts tx %u rx %u duplicates suppressed %u crc failures %u\n",
           stats_tx.timeouts, stats_rx.timeouts, stats_rx.duplicates_suppressed,
           stats_rx.crc_failures);
    printf("air seconds %.3f\n", (double)now / 1000.0);
    free(pcm);
    kal_link_free(side[0]);
    kal_link_free(side[1]);
    if (!delivered || received_size != message_size
            || memcmp(received, message, message_size) != 0) {
        return 1;
    }
    return 0;
}


struct cli_channel_cell {
    const char *name;
    enum kal_channel_kind kind;
    double parameter;
    double secondary;
};

static const struct cli_channel_cell cli_channel_cells[] = {
    {"clean",           KAL_CHANNEL_CLEAN,      0.0,   0.0},
    {"awgn-20dB",       KAL_CHANNEL_AWGN,      20.0,   0.0},
    {"awgn-6dB",        KAL_CHANNEL_AWGN,       6.0,   0.0},
    {"awgn-0dB",        KAL_CHANNEL_AWGN,       0.0,   0.0},
    {"awgn-minus9dB",   KAL_CHANNEL_AWGN,      -9.0,   0.0},
    {"awgn-minus12dB",  KAL_CHANNEL_AWGN,     -12.0,   0.0},
    {"gain-0.25x",      KAL_CHANNEL_GAIN,       0.25,  0.0},
    {"gain-0.003x",     KAL_CHANNEL_GAIN,       0.003, 0.0},
    {"clip-0.5",        KAL_CHANNEL_CLIP,       0.5,   0.0},
    {"drift-plus100",   KAL_CHANNEL_DRIFT,    100.0,   0.0},
    {"drift-plus1000",  KAL_CHANNEL_DRIFT,   1000.0,   0.0},
    {"drift-plus2000",  KAL_CHANNEL_DRIFT,   2000.0,   0.0},
    {"drift-plus5000",  KAL_CHANNEL_DRIFT,   5000.0,   0.0},
    {"drift-plus10000", KAL_CHANNEL_DRIFT,  10000.0,   0.0},
    {"echo-10ms-0.5",   KAL_CHANNEL_ECHO,     10.0,    0.5},
    {"echo-20ms-0.9",   KAL_CHANNEL_ECHO,     20.0,    0.9}
};

/* One beacon frame through the engine, a named impairment, and back. */
static int channel_trial(uint8_t profile, const struct cli_channel_cell *cell,
                         uint64_t seed, uint8_t allow_high_frequency) {
    kal_options tx_options;
    kal_options rx_options;
    kal_link *tx = NULL;
    kal_link *rx = NULL;
    kal_stats stats;
    struct kal_channel channel;
    uint8_t payload[KAL_MAX_BEACON_BYTES];
    uint8_t received[CLI_MAX_INPUT_BYTES];
    size_t size = 0u;
    int16_t *clean = NULL;
    int16_t *damaged = NULL;
    size_t capacity;
    size_t damaged_capacity;
    size_t written = 0u;
    size_t produced = 0u;
    size_t index;
    int ok = 0;

    memset(&tx_options, 0, sizeof tx_options);
    tx_options.sample_rate = KAL_SAMPLE_RATE;
    tx_options.profile = profile;
    tx_options.modem = (uint8_t)KAL_MODEM_GGWAVE;
    tx_options.allow_high_frequency = allow_high_frequency;
    tx_options.role = (uint8_t)KAL_ROLE_INITIATOR;
    rx_options = tx_options;
    rx_options.role = (uint8_t)KAL_ROLE_RESPONDER;

    if (kal_link_create(&tx, &tx_options) != KAL_OK) {
        return -1;
    }
    if (kal_link_create(&rx, &rx_options) != KAL_OK) {
        kal_link_free(tx);
        return -1;
    }
    kal_get_stats(tx, &stats);
    capacity = (size_t)stats.frame_airtime_ms * KAL_SAMPLE_RATE / 1000u
        + KAL_SAMPLE_RATE;

    memset(&channel, 0, sizeof channel);
    channel.kind = cell->kind;
    channel.parameter = cell->parameter;
    channel.secondary = cell->secondary;
    channel.seed = seed;
    damaged_capacity = kal_channel_output_samples(&channel, capacity)
        + KAL_SAMPLE_RATE;

    clean = (int16_t *)calloc(capacity, sizeof(int16_t));
    damaged = (int16_t *)calloc(damaged_capacity, sizeof(int16_t));
    if (clean == NULL || damaged == NULL) {
        free(clean);
        free(damaged);
        kal_link_free(tx);
        kal_link_free(rx);
        return -1;
    }
    for (index = 0u; index < sizeof payload; ++index) {
        payload[index] = (uint8_t)((index * 11u) + (size_t)seed);
    }
    if (kal_beacon(tx, payload, sizeof payload) == KAL_OK
            && kal_tx_pull_s16(tx, clean, capacity, &written, 0u) == KAL_HAVE_OUTPUT
            && written != 0u
            && kal_channel_apply(&channel, clean, written, damaged,
                                 damaged_capacity, &produced) == 0) {
        (void)kal_rx_push_s16(rx, damaged, produced, 0u);
        if (kal_receive(rx, received, sizeof received, &size) == KAL_OK
                && size == sizeof payload
                && memcmp(received, payload, size) == 0) {
            ok = 1;
        }
    }
    free(clean);
    free(damaged);
    kal_link_free(tx);
    kal_link_free(rx);
    return ok;
}

static int cmd_channel(const struct cli_options *options) {
    const size_t cell_count =
        sizeof cli_channel_cells / sizeof cli_channel_cells[0];
    const unsigned int trials = options->seconds == 0u ? 3u : options->seconds;
    size_t index;
    unsigned long executed = 0u;

    printf("# synthetic impairment matrix. No device is opened, nothing is\n");
    printf("# played and nothing is recorded. This is not a room, a\n");
    printf("# transducer or a device: it graduates 0/6 physical profiles and\n");
    printf("# decides profile choice in 0/1 cases.\n");
    printf("impairment,profile,delivered,trials\n");
    for (index = 0u; index < cell_count; ++index) {
        uint8_t profile;
        for (profile = 0u; profile < (uint8_t)KAL_PROFILE_COUNT; ++profile) {
            unsigned int delivered = 0u;
            unsigned int attempt;
            if (options->profile_selected != 0u && profile != options->profile) {
                continue;
            }
            for (attempt = 0u; attempt < trials; ++attempt) {
                const uint64_t seed = ((uint64_t)index << 32)
                    + ((uint64_t)profile << 8) + attempt + 1u;
                const int result = channel_trial(profile,
                                                 &cli_channel_cells[index], seed,
                                                 1u);
                if (result < 0) {
                    fprintf(stderr, "channel: link creation failed\n");
                    return 1;
                }
                delivered += (unsigned int)result;
                executed++;
            }
            printf("%s,%s,%u,%u\n", cli_channel_cells[index].name,
                   kal_profile_string(profile), delivered, trials);
        }
    }
    printf("# trials executed %lu/%lu\n", executed, executed);
    printf("# physical profiles graduated 0/6\n");
    return 0;
}

int main(int argc, char **argv) {
    struct cli_options options;
    int index;

    memset(&options, 0, sizeof options);
    options.profile = (uint8_t)KAL_PROFILE_AUDIBLE_NORMAL;
    options.window = 4u;
    options.role = (uint8_t)KAL_ROLE_INITIATOR;
    options.seconds = 0u;

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    options.command = argv[1];
    for (index = 2; index < argc; ++index) {
        const char *argument = argv[index];
        const char *value = (index + 1 < argc) ? argv[index + 1] : NULL;
        if (strcmp(argument, "--text") == 0 && value != NULL) {
            options.text = value; index++;
        } else if (strcmp(argument, "--input") == 0 && value != NULL) {
            options.input = value; index++;
        } else if (strcmp(argument, "--output") == 0 && value != NULL) {
            options.output = value; index++;
        } else if (strcmp(argument, "--input-pcm") == 0 && value != NULL) {
            options.input_pcm = value; index++;
        } else if (strcmp(argument, "--output-pcm") == 0 && value != NULL) {
            options.output_pcm = value; index++;
        } else if (strcmp(argument, "--device") == 0 && value != NULL) {
            options.device = value; index++;
        } else if (strcmp(argument, "--hex") == 0 && value != NULL) {
            options.hex = value; index++;
        } else if (strcmp(argument, "--profile") == 0 && value != NULL) {
            if (parse_profile(value, &options.profile) != 0) {
                fprintf(stderr, "unknown profile: %s\n", value);
                return 2;
            }
            options.profile_selected = 1u;
            index++;
        } else if (strcmp(argument, "--window") == 0 && value != NULL) {
            options.window = (uint16_t)atoi(value); index++;
        } else if (strcmp(argument, "--role") == 0 && value != NULL) {
            if (strcmp(value, "initiator") == 0) {
                options.role = (uint8_t)KAL_ROLE_INITIATOR;
            } else if (strcmp(value, "responder") == 0) {
                options.role = (uint8_t)KAL_ROLE_RESPONDER;
            } else {
                fprintf(stderr, "unknown role: %s\n", value);
                return 2;
            }
            index++;
        } else if (strcmp(argument, "--seconds") == 0 && value != NULL) {
            options.seconds = (unsigned int)atoi(value); index++;
        } else if (strcmp(argument, "--allow-audio") == 0) {
            options.allow_audio = 1u;
        } else if (strcmp(argument, "--allow-high-frequency") == 0) {
            options.allow_high_frequency = 1u;
        } else if (strcmp(argument, "--show") == 0) {
            options.input = "--show";
        } else if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argument);
            return 2;
        }
    }

    if (strcmp(options.command, "probe") == 0) return cmd_probe(&options);
    if (strcmp(options.command, "channel") == 0) return cmd_channel(&options);
    if (strcmp(options.command, "loopback") == 0) return cmd_loopback(&options);
    if (strcmp(options.command, "dump-frame") == 0) return cmd_dump_frame(&options);
    if (strcmp(options.command, "send") == 0) return cmd_send(&options);
    if (strcmp(options.command, "receive") == 0) return cmd_receive(&options);
    if (strcmp(options.command, "beacon") == 0) return cmd_beacon(&options);
    if (strcmp(options.command, "listen") == 0) return cmd_listen(&options);
    if (strcmp(options.command, "calibrate") == 0) return cmd_calibrate(&options);
    if (strcmp(options.command, "version") == 0) {
        printf("%s\n", kal_version_string());
        return 0;
    }
    if (strcmp(options.command, "--help") == 0 || strcmp(options.command, "-h") == 0) {
        usage(stdout);
        return 0;
    }
    fprintf(stderr, "unknown command: %s\n", options.command);
    usage(stderr);
    return 2;
}
