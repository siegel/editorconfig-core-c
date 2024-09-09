/* inih -- simple .INI file parser

The "inih" library is distributed under the New BSD license:

Copyright (c) 2009, Brush Technology
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Brush Technology nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY BRUSH TECHNOLOGY ''AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL BRUSH TECHNOLOGY BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Go to the project home page for more info:
http://code.google.com/p/inih/

*/

#include <dispatch/dispatch.h>
#include <map>

#include "global.h"

#include <sys/fcntl.h>
#include <sys/stat.h>

#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include "ini.h"

#define MAX_LINE 5000
#define MAX_SECTION MAX_SECTION_NAME
#define MAX_NAME MAX_PROPERTY_NAME

/* Strip whitespace chars off end of given string, in place. Return s. */
static char* rstrip(char* s)
{
    char* p = s + strlen(s);
    while (p > s && isspace(*--p))
        *p = '\0';
    return s;
}

/* Return pointer to first non-whitespace char in given string. */
static char* lskip(const char* s)
{
    while (*s && isspace(*s))
        s++;
    return (char*)s;
}

/* Return pointer to first char c or ';' comment in given string, or pointer to
   null at end of string if neither found. ';' must be prefixed by a whitespace
   character to register as a comment. */
static char* find_char_or_comment(const char* s, char c)
{
    int was_whitespace = 0;
    while (*s && *s != c && !(was_whitespace && (*s == ';' || *s == '#'))) {
        was_whitespace = isspace(*s);
        s++;
    }
    return (char*)s;
}

static char* find_last_char_or_comment(const char* s, char c)
{
    const char* last_char = s;
    int was_whitespace = 0;
    while (*s && !(was_whitespace && (*s == ';' || *s == '#'))) {
        if (*s == c)
            last_char = s;
        was_whitespace = isspace(*s);
        s++;
    }
    return (char*)last_char;
}

/* Version of strncpy that ensures dest (size bytes) is null-terminated. */
static char* strncpy0(char* dest, const char* src, size_t size)
{
    strncpy(dest, src, size);
    dest[size - 1] = '\0';
    return dest;
}

/* See documentation in header file. */
EDITORCONFIG_LOCAL
int ini_parse_file(const char *file /* null terminated */,
                   int (*handler)(void*, const char*, const char*,
                                  const char*),
                   void* user)
{
    /* Uses a fair bit of stack (use heap instead if you need to) */
    char section[MAX_SECTION+1] = "";
    char prev_name[MAX_NAME+1] = "";

    char* start;
    char* end;
    char* name;
    char* value;
    int lineno = 0;
    int error = 0;

    const char  *p = file;
    
    /* Scan through file line by line */
    while (0 != *p)
    {
        const char *nextLine = strchr(p, '\n');
        char line[MAX_LINE];
        
        //  REVIEW copying each line is inefficient, but means the input is const
        //  and can be kept in a cache.
        if (nextLine == p) {
        	line[0] = 0;	//	empty line, but passing a zero field with to snprintf would copy the whole buffer
		}
		else
        if (NULL != nextLine) {
            snprintf(line, sizeof(line), "%.*s", (int)(nextLine - p), p);
        }
        else {
            //  we're at the end of the text, we'll exit after this line
            snprintf(line, sizeof(line), "%s", p);  //  input is zero-terminated
        }
        
        lineno++;

        start = line;
#if INI_ALLOW_BOM
        if (lineno == 1 && (unsigned char)start[0] == 0xEF &&
                           (unsigned char)start[1] == 0xBB &&
                           (unsigned char)start[2] == 0xBF) {
            start += 3;
        }
#endif
        start = lskip(rstrip(start));

        if (*start == ';' || *start == '#') {
            /* Per Python ConfigParser, allow '#' comments at start of line */
        }
#if INI_ALLOW_MULTILINE
        else if (*prev_name && *start && start > line) {
            /* Non-black line with leading whitespace, treat as continuation
               of previous name's value (as per Python ConfigParser). */
            if (!handler(user, section, prev_name, start) && !error)
                error = lineno;
        }
#endif
        else if (*start == '[') {
            /* A "[section]" line */
            end = find_last_char_or_comment(start + 1, ']');
            if (*end == ']') {
                *end = '\0';
                /* Section name too long. Skipped. */
                if (end - start - 1 > MAX_SECTION_NAME)
                    continue;
                strncpy0(section, start + 1, sizeof(section));
                *prev_name = '\0';
            }
            else if (!error) {
                /* No ']' found on section line */
                error = lineno;
            }
        }
        else if (*start && (*start != ';' || *start == '#')) {
            /* Not a comment, must be a name[=:]value pair */
            end = find_char_or_comment(start, '=');
            if (*end != '=') {
                end = find_char_or_comment(start, ':');
            }
            if (*end == '=' || *end == ':') {
                *end = '\0';
                name = rstrip(start);
                value = lskip(end + 1);
                end = find_char_or_comment(value, '\0');
                if (*end == ';' || *end == '#')
                    *end = '\0';
                rstrip(value);

                /* Either name or value is too long. Skip it. */
                if (strlen(name) > MAX_PROPERTY_NAME ||
                    strlen(value) > MAX_PROPERTY_VALUE)
                    continue;

                /* Valid name[=:]value pair found, call handler */
                strncpy0(prev_name, name, sizeof(prev_name));
                if (!handler(user, section, name, value) && !error)
                    error = lineno;
            }
            else if (!error) {
                /* No '=' or ':' found on name[=:]value line */
                error = lineno;
            }
        }
        
        if (NULL == nextLine)
            break;
        
        p = nextLine + 1;
    }

    return error;
}

typedef struct CacheEntry
{
    char                *filename = NULL;
    char                *data = NULL;
    dispatch_source_t   dispatchSource = NULL;
    int                 fd = 0;
    
    ~CacheEntry();
} CacheEntry;

CacheEntry::~CacheEntry()
{
    free(filename);
    free(data);
    dispatch_source_cancel(dispatchSource);
    dispatch_release(dispatchSource);

    if (0 != fd)
        close(fd);
}

typedef std::map<std::string, CacheEntry*>  FileDataCache;

ini_parse_cache_invalidation_callback   ini_parse_cache_invalidated;

static
char* ini_data_from_file(const char *filename)
{
    int file;
    int error;
    struct stat	status;
    char    *data = NULL;
    ssize_t actLen = 0;
    
    file = open(filename, O_RDONLY);
    if (file < 0)
        return NULL;  //  errno is set
    
    if (fstat(file, &status) < 0)
    {
        close(file);
        return NULL;  //  errno is set
    }
    
    data = static_cast<char*>(malloc(status.st_size + 1 /* room for trailing NULL */));
    if (NULL == data)
    {
        close(file);
        return NULL;
    }
    
    actLen = read(file, data, status.st_size);
    if (actLen < 0)
    {
        close(file);
        free(data);
        
        return NULL;
    }
    
    data[actLen] = 0;   //  null terminate
    
    close(file);
    return data;
}

static
char* ini_data_for_file(const char *filename, const char *data /* NULL to fetch, otherwise to store */)
{
    static dispatch_once_t  _inited;
    static FileDataCache    *_map;
    static pthread_mutex_t  _mutex;
    
    dispatch_once(&_inited,
        ^()
        {
            pthread_mutexattr_t	mutexAttrs;
            
            pthread_mutexattr_init(&mutexAttrs);
            pthread_mutexattr_settype(&mutexAttrs, PTHREAD_MUTEX_RECURSIVE);
            
            pthread_mutex_init(&_mutex, &mutexAttrs);
            
            pthread_mutexattr_destroy(&mutexAttrs);

            _map = new FileDataCache;
        }
    );
    
    if (0 == pthread_mutex_lock(&_mutex))
    {
        if (NULL == data)
        {
            CacheEntry  *entry = (*_map)[filename];
            
            pthread_mutex_unlock(&_mutex);
            
            if (NULL != entry)
                return entry->data;
        }
        else
        {
            CacheEntry  *entry = new CacheEntry;
            
            entry->filename = strdup(filename);
            entry->data = const_cast<char*>(data);
            entry->fd = open(filename, O_EVTONLY);
            entry->dispatchSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE,
                                                            entry->fd,
                                                            (DISPATCH_VNODE_DELETE | DISPATCH_VNODE_WRITE | DISPATCH_VNODE_EXTEND | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_LINK | DISPATCH_VNODE_REVOKE),
                                                            DISPATCH_TARGET_QUEUE_DEFAULT);
            
            dispatch_source_set_event_handler(entry->dispatchSource,
                ^()
                {
                    //  we're here asynchronously on a different thread, so we need to acquire the lock.
                    if (0 == pthread_mutex_lock(&_mutex))
                    {
                        //  punch it out of the cache, we'll reread it the next time we need it
                        (*_map)[entry->filename] = NULL;
                        
                        if (NULL != ini_parse_cache_invalidated)
                            ini_parse_cache_invalidated(entry->filename);
                            
                        delete entry;   //  this does all the cleanup
                        
                        pthread_mutex_unlock(&_mutex);
                    }
                }
            );
            
            //  cache it, then start the dispatch source

            (*_map)[filename] = entry;
            dispatch_resume(entry->dispatchSource);
            
            pthread_mutex_unlock(&_mutex);
        }
    }
    
    return NULL;
}

/* See documentation in header file. */
EDITORCONFIG_LOCAL
int ini_parse(const char* filename,
              int (*handler)(void*, const char*, const char*, const char*),
              void* user)
{
    char    *data = NULL;
    bool    wasCached = false;
   
    data = ini_data_for_file(filename, NULL);
    if (NULL == data)
        data = ini_data_from_file(filename);
    else
        wasCached = true;
    
    if (NULL != data)
    {
        int error = 0;
        
        if (0 == (error = ini_parse_file(data, handler, user)))
        {
            if (! wasCached)
                ini_data_for_file(filename, data);  //  cache it
        }
        
        return error;
    }
    
    return -1;
}
