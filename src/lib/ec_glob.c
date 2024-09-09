/*
 * Copyright (c) 2014-2019 Hong Xu <hong AT topbug DOT net>
 * Copyright (c) 2018 Sven Strickroth <email AT cs-ware DOT de>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
*/

#include <dispatch/dispatch.h>

#include <map>

#include "global.h"

#include <ctype.h>
#include <string.h>
#include <pcre2.h>

#define utarray_oom() { return -2; }
#include "utarray.h"
#include "util.h"

#include "ec_glob.h"

/* Special characters */
const char ec_special_chars[] = "?[]\\*-{},";

typedef struct int_pair
{
    int     num1;
    int     num2;
} int_pair;
static const UT_icd ut_int_pair_icd = {sizeof(int_pair),NULL,NULL,NULL};

/* concatenate the string then move the pointer to the end */
#define STRING_CAT(p, string, end)  do {    \
    size_t string_len = strlen(string); \
    if (p + string_len >= end) \
        return -1; \
    strcat(p, string); \
    p += string_len; \
} while(0)

static
pcre2_code* ec_glob_number_pattern(void)
{
    static dispatch_once_t  _inited;
    static uint8_t          *_data;
    static PCRE2_SIZE       _dataSize;
    
    dispatch_once(&_inited,
        ^()
        {
            static const char   kNumberPattern[] = "^\\{[\\+\\-]?\\d+\\.\\.[\\+\\-]?\\d+\\}$";
            
            pcre2_code  *re = NULL;
            int         error_code;
            size_t      erroffset;
            
            re = pcre2_compile((uint8_t*)kNumberPattern, PCRE2_ZERO_TERMINATED, 0, &error_code, &erroffset, NULL);

            if (NULL != re)
                pcre2_serialize_encode((const pcre2_code**)&re, 1, &_data, &_dataSize, NULL);

        }
    );

    if (NULL != _data)
    {
        pcre2_code  *pcre = NULL;
        
        if (1 == pcre2_serialize_decode(&pcre, 1, _data, NULL))
            return pcre;
    }
    
    return NULL;
}

typedef std::pair<uint8_t *, UT_array*>  ec_glob_cache_pair;

static std::pair<pcre2_code*, UT_array *>
ec_glob_cached_pattern(const char *pattern, pcre2_code *re /* NULL to fetch, otherwise to store */, UT_array *nums /* ignored for fetch */)
{
    static dispatch_once_t  _inited;
    static std::map<std::string, ec_glob_cache_pair>
                            *_map;
    static pthread_mutex_t  _mutex;
    
    dispatch_once(&_inited,
        ^()
        {
            pthread_mutexattr_t	mutexAttrs;
            
            pthread_mutexattr_init(&mutexAttrs);
            pthread_mutexattr_settype(&mutexAttrs, PTHREAD_MUTEX_RECURSIVE);
            
            pthread_mutex_init(&_mutex, &mutexAttrs);
            
            pthread_mutexattr_destroy(&mutexAttrs);
        
            _map = new std::map<std::string, ec_glob_cache_pair>();
        }
    );
    
    if ((NULL == pattern) || (0 == *pattern))
    {
        //  this is a problem.
        /*...*/
    }
    else
    if (0 == pthread_mutex_lock(&_mutex))
    {
        if (NULL == re)
        {
            //  we're going to fetch
            ec_glob_cache_pair entry = (*_map)[pattern];
            
            if (NULL != entry.first)
            {
                if (1 == pcre2_serialize_decode(&re, 1, entry.first, NULL))
                {
                	//	We are good to go, release our lock and return
			        pthread_mutex_unlock(&_mutex);
                
                    return std::pair<pcre2_code *, UT_array *>(re, entry.second);
				}
            }
        }
        else
        {
            //  we're going to store
            
            uint8_t     *data = NULL;
            PCRE2_SIZE  dataSize;
            
            if (1 == pcre2_serialize_encode((const pcre2_code**)&re, 1, &data, &dataSize, NULL))
                (*_map)[pattern] = ec_glob_cache_pair(data, nums);
        }
        
        pthread_mutex_unlock(&_mutex);
    }
    
    return std::pair<pcre2_code *, UT_array *>(NULL, NULL);
}

#define PATTERN_MAX  4097
/*
 * Whether the string matches the given glob pattern. Return 0 if successful, return -1 if a PCRE
 * error or other regex error occurs, and return -2 if an OOM outside PCRE occurs.
 */
EDITORCONFIG_LOCAL
int ec_glob(const char *pattern, const char *string)
{
    size_t                    i;
    int_pair *                p;
    char *                    c;
    char                      pcre_str[2 * PATTERN_MAX] = "^";
    char *                    p_pcre;
    char *                    pcre_str_end;
    int                       brace_level = 0;
    bool                      is_in_bracket = 0;
    int                       error_code;
    size_t                    erroffset;
    pcre2_code *              re;
    int                       rc;
    size_t *                  pcre_result;
    pcre2_match_data *        pcre_match_data;
    char                      l_pattern[2 * PATTERN_MAX];
    bool                      are_braces_paired = 1;
    UT_array *                nums = NULL;     /* number ranges */
    std::pair<pcre2_code *, UT_array *>
                              cached((pcre2_code*)NULL, (UT_array*)NULL);
    int                       ret = 0;

    strcpy(l_pattern, pattern);
    p_pcre = pcre_str + 1;
    pcre_str_end = pcre_str + 2 * PATTERN_MAX;

    cached = ec_glob_cached_pattern(pattern, NULL, NULL);
    if (NULL == (re = cached.first))
    {
    
        /* Determine whether curly braces are paired */
        {
            int     left_count = 0;
            int     right_count = 0;
            for (c = l_pattern; *c; ++ c)
            {
                if (*c == '\\' && *(c+1) != '\0')
                {
                    ++ c;
                    continue;
                }
    
                if (*c == '}')
                    ++ right_count;
                else if (*c == '{')
                    ++ left_count;
    
                if (right_count > left_count)
                {
                    are_braces_paired = 0;
                    break;
                }
            }
    
            if (right_count != left_count)
                are_braces_paired = 0;
        }
    
        /* used to search for {num1..num2} case */
        if (NULL == (re = ec_glob_number_pattern()))
            return -1;        /* failed to compile */
    
        utarray_new(nums, &ut_int_pair_icd);
    
        for (c = l_pattern; *c; ++ c)
        {
            switch (*c)
            {
            case '\\':      /* also skip the next one */
                if (*(c+1) != '\0')
                {
                    *(p_pcre ++) = *(c++);
                    *(p_pcre ++) = *c;
                }
                else
                    STRING_CAT(p_pcre, "\\\\", pcre_str_end);
    
                break;
            case '?':
                STRING_CAT(p_pcre, "[^/]", pcre_str_end);
                break;
            case '*':
                if (*(c+1) == '*')      /* case of ** */
                {
                    STRING_CAT(p_pcre, ".*", pcre_str_end);
                    ++ c;
                }
                else                    /* case of * */
                    STRING_CAT(p_pcre, "[^\\/]*", pcre_str_end);
    
                break;
            case '[':
                if (is_in_bracket)     /* inside brackets, we really mean bracket */
                {
                    STRING_CAT(p_pcre, "\\[", pcre_str_end);
                    break;
                }
    
                {
                    /* check whether we have slash within the bracket */
                    bool            has_slash = 0;
                    char *          cc;
                    for (cc = c; *cc && *cc != ']'; ++ cc)
                    {
                        if (*cc == '\\' && *(cc+1) != '\0')
                        {
                            ++ cc;
                            continue;
                        }
    
                        if (*cc == '/')
                        {
                            has_slash = 1;
                            break;
                        }
                    }
    
                    /* if we have slash in the brackets, just do it literally */
                    if (has_slash)
                    {
                        char *           right_bracket = strchr(c, ']');
    
                        if (!right_bracket)  /* The right bracket may not exist */
                            right_bracket = c + strlen(c);
    
                        strcat(p_pcre, "\\");
                        strncat(p_pcre, c, right_bracket - c);
                        if (*right_bracket)  /* right_bracket is a bracket */
                            strcat(p_pcre, "\\]");
                        p_pcre += strlen(p_pcre);
                        c = right_bracket;
                        if (!*c)
                            /* end of string, meaning that right_bracket is not a
                             * bracket. Then we go back one character to make the
                             * parsing end normally for the counter in the "for"
                             * loop. */
                            c -= 1;
                        break;
                    }
                }
    
                is_in_bracket = 1;
                if (*(c+1) == '!')     /* case of [!...] */
                {
                    STRING_CAT(p_pcre, "[^", pcre_str_end);
                    ++ c;
                }
                else
                    *(p_pcre ++) = '[';
    
                break;
    
            case ']':
                is_in_bracket = 0;
                *(p_pcre ++) = *c;
                break;
    
            case '-':
                if (is_in_bracket)      /* in brackets, - indicates range */
                    *(p_pcre ++) = *c;
                else
                    STRING_CAT(p_pcre, "\\-", pcre_str_end);
    
                break;
            case '{':
                if (!are_braces_paired)
                {
                    STRING_CAT(p_pcre, "\\{", pcre_str_end);
                    break;
                }
    
                /* Check the case of {single}, where single can be empty */
                {
                    char *                   cc;
                    bool                     is_single = 1;
    
                    for (cc = c + 1; *cc != '\0' && *cc != '}'; ++ cc)
                    {
                        if (*cc == '\\' && *(cc+1) != '\0')
                        {
                            ++ cc;
                            continue;
                        }
    
                        if (*cc == ',')
                        {
                            is_single = 0;
                            break;
                        }
                    }
    
                    if (*cc == '\0')
                        is_single = 0;
    
                    if (is_single)      /* escape the { and the corresponding } */
                    {
                        const char *        double_dots;
                        int_pair            pair;
    
                        pcre2_match_data *  match_data = pcre2_match_data_create_from_pattern(re, NULL);
    
                        /* Check the case of {num1..num2} */
                        rc = pcre2_match(re, (PCRE2_SPTR8)c, cc - c + 1, 0, 0, match_data, NULL);
    
                        pcre2_match_data_free(match_data);
    
                        if (rc < 0)    /* not {num1..num2} case */
                        {
                            STRING_CAT(p_pcre, "\\{", pcre_str_end);
    
                            memmove(cc+1, cc, strlen(cc) + 1);
                            *cc = '\\';
    
                            break;
                        }
    
                        /* Get the range */
                        double_dots = strstr(c, "..");
                        pair.num1 = ec_atoi(c + 1);
                        pair.num2 = ec_atoi(double_dots + 2);
    
                        utarray_push_back(nums, &pair);
    
                        STRING_CAT(p_pcre, "([\\+\\-]?\\d+)", pcre_str_end);
                        c = cc;
    
                        break;
                    }
                }
    
                ++ brace_level;
                STRING_CAT(p_pcre, "(?:", pcre_str_end);
                break;
    
            case '}':
                if (!are_braces_paired)
                {
                    STRING_CAT(p_pcre, "\\}", pcre_str_end);
                    break;
                }
    
                -- brace_level;
                *(p_pcre ++) = ')';
                break;
    
            case ',':
                if (brace_level > 0)  /* , inside {...} */
                    *(p_pcre ++) = '|';
                else
                    STRING_CAT(p_pcre, "\\,", pcre_str_end);
                break;
    
            case '/':
                // /**/ case, match both single / and /anything/
                if (!strncmp(c, "/**/", 4))
                {
                    STRING_CAT(p_pcre, "(\\/|\\/.*\\/)", pcre_str_end);
                    c += 3;
                }
                else
                    STRING_CAT(p_pcre, "\\/", pcre_str_end);
    
                break;
    
            default:
                if (!isalnum(*c))
                    *(p_pcre ++) = '\\';
    
                *(p_pcre ++) = *c;
            }
        }
    
        *(p_pcre ++) = '$';
    
        pcre2_code_free(re); /* ^\\d+\\.\\.\\d+$ */
    
        re = pcre2_compile((PCRE2_SPTR8)pcre_str, PCRE2_ZERO_TERMINATED, 0, &error_code, &erroffset, NULL);
    
        if (NULL != re)
        {
            //  cache it so that we don't have to do this again.
            //	Note that "nums" gets cached, so we only free it
            //	in the error case.
            ec_glob_cached_pattern(pattern, re, nums);
        }
        else
        {
            /* failed to compile */
            utarray_free(nums);
            return -1;
        }
    }
    else
    {
        nums = cached.second;
    }
    
    pcre_match_data = pcre2_match_data_create_from_pattern(re, NULL);
    rc = pcre2_match(re, (PCRE2_SPTR8)string, strlen(string), 0, 0, pcre_match_data, NULL);

    if (rc < 0)     /* failed to match */
    {
        if (rc == PCRE2_ERROR_NOMATCH)
            ret = EC_GLOB_NOMATCH;
        else
            ret = rc;

        goto cleanup;
    }

    /* Whether the numbers are in the desired range? */
    pcre_result = pcre2_get_ovector_pointer(pcre_match_data);
    for(p = (int_pair *) utarray_front(nums), i = 1; p;
            ++ i, p = (int_pair *) utarray_next(nums, p))
    {
        const char * substring_start = string + pcre_result[2 * i];
        size_t  substring_length = pcre_result[2 * i + 1] - pcre_result[2 * i];
        char *       num_string;
        int          num;

        /* we don't consider 0digits such as 010 as matched */
        if (*substring_start == '0')
            break;

        num_string = strndup(substring_start, substring_length);
        if (num_string == NULL) {
          ret = -2;
          goto cleanup;
        }
        num = ec_atoi(num_string);
        free(num_string);

        if (num < p->num1 || num > p->num2) /* not matched */
            break;
    }

    if (p != NULL)      /* numbers not matched */
        ret = EC_GLOB_NOMATCH;

 cleanup:

    pcre2_code_free(re);
    pcre2_match_data_free(pcre_match_data);

    return ret;
}
