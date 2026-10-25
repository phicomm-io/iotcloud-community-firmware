/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/* Stable storage values. Zero-filled pre-1.0 configurations use Chinese. */
enum { PH_LANG_ZH = 0, PH_LANG_EN = 1 };
static inline const char *ph_language_tag(unsigned language) {
    return language == PH_LANG_EN ? "en" : "zh-CN";
}
static inline bool ph_language_parse(const char *tag, uint8_t *out) {
    if (!strcmp(tag, "en")) *out = PH_LANG_EN;
    else if (!strcmp(tag, "zh-CN")) *out = PH_LANG_ZH;
    else return false;
    return true;
}
/* Message catalogs pass Chinese/English pairs; wire identifiers never enter here. */
static inline const char *ph_i18n(unsigned language, const char *zh, const char *en) {
    return language == PH_LANG_EN ? en : zh;
}
