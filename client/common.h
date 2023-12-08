#include <android/log.h>


#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, "main", __VA_ARGS__))
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "main", __VA_ARGS__))
#define LOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, "main", __VA_ARGS__))
