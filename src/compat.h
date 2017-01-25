/*
 *******************************************************************
 *
 * Copyright 2016 Intel Corporation All rights reserved.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 */

#ifndef _COMPAT_H
#define _COMPAT_H

#include <string.h>
#include <stdint.h>
#include <safe_lib.h>

/*
 * Code required for platform compatibility
 */

#ifdef _WIN32

inline char* strndup(const char* str, size_t maxLen)
{
    size_t len = strnlen_s(str, maxLen + 1);
    if (len > maxLen) {
        return NULL;
    } else {
        char* c = malloc(len + 1);
        if (c) {
            memcpy_s(c, len + 1, str, len + 1);
        }
        return c;
    }
}

#endif

#endif
