/****************************************************************************
 * apps/ai_watch/fonts/ai_watch_font_weather_28.h
 *
 * Weather card font (28 px, 2 bpp): ASCII + the seven weather words
 * rendered by ai_watch_weather_text() + U+00B0/U+2103 degree glyphs.
 * Micro subset on purpose -- a full charset at 28 px would cost ~1 MB of
 * flash. Regenerate with the command documented in fonts/gen_cjk_font.sh;
 * do not edit the .c by hand.
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __APPS_AI_WATCH_FONTS_AI_WATCH_FONT_WEATHER_28_H
#define __APPS_AI_WATCH_FONTS_AI_WATCH_FONT_WEATHER_28_H

#include <lvgl/lvgl.h>

extern const lv_font_t ai_watch_font_weather_28;

#endif /* __APPS_AI_WATCH_FONTS_AI_WATCH_FONT_WEATHER_28_H */
