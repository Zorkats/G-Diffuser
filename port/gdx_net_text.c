/* port/gdx_net_text.c -- see gdx_net_text.h.
 * Copyright (c) Zorkats
 */

#include "gdx_net_text.h"

#include <stdio.h>
#include <string.h>

/* The character as the font draws it: a capital for a small letter, a space for what the font lacks. */
static char font_char(int font, char c) {
    if (c >= 'a' && c <= 'z') {
        return (char) (c - 'a' + 'A');
    }
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return c;
    }
    if (c != '\0' && strchr(font == GDX_NET_ROOM_FONT_TITLE ? "'&-:.," : "'\"&+-.", c) != NULL) {
        return c;
    }
    return ' ';
}

void gdx_net_text_filter(int font, const char* in, char* out, int size) {
    int i = 0;

    if (size <= 0) {
        return;
    }
    for (; in != NULL && in[i] != '\0' && i < size - 1; i++) {
        out[i] = font_char(font, in[i]);
    }
    out[i] = '\0';
}

int gdx_net_text_wrap(const char* text, int columns, char* out, int lineBytes, int maxLines) {
    int lines = 0;

    if (columns > lineBytes - 1) {
        columns = lineBytes - 1;
    }
    if (text == NULL || columns <= 0) {
        return 0;
    }
    while (*text == ' ') {
        text++;
    }
    while (*text != '\0' && lines < maxLines) {
        int length = 0;
        int lastSpace = -1;

        while (text[length] != '\0' && text[length] != '\n' && length < columns) {
            if (text[length] == ' ') {
                lastSpace = length;
            }
            length++;
        }
        /* The line ends in the middle of a word: back to the last space, unless the word fills the line. */
        if (text[length] != '\0' && text[length] != '\n' && text[length] != ' ' && lastSpace > 0) {
            length = lastSpace;
        }
        memcpy(out + lines * lineBytes, text, (size_t) length);
        out[lines * lineBytes + length] = '\0';
        text += length;
        while (length > 0 && out[lines * lineBytes + length - 1] == ' ') {
            out[lines * lineBytes + --length] = '\0';
        }
        lines++;
        while (*text == ' ') {
            text++;
        }
        if (*text == '\n') {
            text++;
            while (*text == ' ') {
                text++;
            }
        }
    }
    return lines;
}

void gdx_net_text_track_name(const char* stock, char* out, int size) {
    int used = 0;
    int start = 1;

    if (size <= 0) {
        return;
    }
    while (stock != NULL && *stock != '\0' && used < size - 1) {
        const unsigned char c = (unsigned char) *stock;
        const unsigned char next = (unsigned char) stock[1];

        /* The game's alpha and beta: 0xA3 and the code of a full width A or B. */
        if (c == 0xA3 && (next == 0xC1 || next == 0xC2)) {
            const char* word = next == 0xC1 ? "Alpha" : "Beta";

            while (*word != '\0' && used < size - 1) {
                out[used++] = *word++;
            }
            stock += 2;
            start = 0;
            continue;
        }
        if (c >= 'a' && c <= 'z' && start) {
            out[used++] = (char) (c - 'a' + 'A');
        } else if (c >= 'A' && c <= 'Z' && !start) {
            out[used++] = (char) (c - 'A' + 'a');
        } else {
            out[used++] = (char) c;
        }
        start = c == ' ';
        stock++;
    }
    out[used] = '\0';
}

void gdx_net_text_format_timer(unsigned leftMs, char* out, int size) {
    const unsigned seconds = (leftMs + 999u) / 1000u;

    snprintf(out, (size_t) size, "%u:%02u", seconds / 60u, seconds % 60u);
}
