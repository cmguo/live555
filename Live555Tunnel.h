#ifndef _LIVE555_TUNNEL_H_
#define _LIVE555_TUNNEL_H_

#ifdef LIVE555_DYNAMIC
#  define LIVE555_EXPORT
#  define LIVE555_FUNC(x) (*x)
#else
#  ifdef _WIN32
#    ifdef LIVE555_EXPORTS
#      define LIVE555_EXPORT __declspec(dllexport)
#    else
#      define LIVE555_EXPORT __declspec(dllimport)
#    endif
#  else
#    define LIVE555_EXPORT
#  endif // __WIN32__
#  define LIVE555_FUNC(x) x
#endif // LIVE555_DYNAMIC

#ifdef _WIN32
typedef wchar_t tchar;
#else
typedef char tchar;
#endif

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

struct Live555Tunnel;

enum Live555_Error
{
    Rtsp_success,
    Rtsp_stream_end,
};

struct Live555Block
{
    int size = 0;
    int track = 0;
    int flags = 0;
    int extra = 0;
    unsigned char const * buffer;
    unsigned long long decode_time;
};

typedef void (*Live555BlockArrive)(void* context, Live555Block const* block);

typedef void (*Live555Logger)(void* context, int level, char const* msg);

#ifdef LIVE555_DYNAMIC
typedef struct __Live555Lib {
#endif

LIVE555_EXPORT int LIVE555_FUNC(Live555Tunnel_Create)(Live555Tunnel** tunnel);

LIVE555_EXPORT void LIVE555_FUNC(Live555Tunnel_SetLogger)(Live555Tunnel* tunnel, Live555Logger logger, void* context);

LIVE555_EXPORT void LIVE555_FUNC(Live555Tunnel_SetBlockCallback)(Live555Tunnel* tunnel, Live555BlockArrive callback, void* context);

LIVE555_EXPORT int LIVE555_FUNC(Live555Tunnel_Open)(Live555Tunnel* tunnel, char const* url);

LIVE555_EXPORT int LIVE555_FUNC(Live555Tunnel_Loop)(Live555Tunnel* tunnel, char * watch_variable);

LIVE555_EXPORT void LIVE555_FUNC(Live555Tunnel_FreeBuffer)(Live555Tunnel* tunnel, unsigned char const* buffer);

LIVE555_EXPORT void LIVE555_FUNC(Live555Tunnel_Close)(Live555Tunnel* tunnel);

LIVE555_EXPORT int LIVE555_FUNC(Live555Tunnel_GetLastError)(Live555Tunnel* tunnel);

LIVE555_EXPORT tchar const* LIVE555_FUNC(Live555Tunnel_GetLastErrorMsg)(Live555Tunnel* tunnel);

LIVE555_EXPORT void LIVE555_FUNC(Live555Tunnel_Destroy)(Live555Tunnel* tunnel);

#ifdef LIVE555_DYNAMIC
} Live555Lib;
#endif

#ifdef __cplusplus
}
#endif // __cplusplus

#endif /* _LIVE555_TUNNEL_H_ */