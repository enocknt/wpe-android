#include <jni.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "logging.h"

extern "C" {
JNIEXPORT void JNICALL Java_com_wpe_wpe_WebProcess_Glue_initializeXdg(JNIEnv*, jobject, jstring);
JNIEXPORT void JNICALL Java_com_wpe_wpe_WebProcess_Glue_initializeFontconfig(JNIEnv*, jobject, jstring);
JNIEXPORT void JNICALL Java_com_wpe_wpe_WebProcess_Glue_initializeMain(JNIEnv*, jobject, jint, jint);

JNIEXPORT void JNICALL Java_com_wpe_wpe_WebProcess_Glue_provideSurface(JNIEnv*, jobject, jobject);
}

JNIEXPORT void JNICALL
Java_com_wpe_wpe_WebProcess_Glue_initializeXdg(JNIEnv* env, jobject, jstring xdgCachePath)
{
    ALOGV("Glue::initializeXdg()");

    const char* cachePath = env->GetStringUTFChars(xdgCachePath, 0);
    ALOGV("  cachePath %s", cachePath);

    setenv("XDG_CACHE_HOME", cachePath, 1);
}

JNIEXPORT void JNICALL
Java_com_wpe_wpe_WebProcess_Glue_initializeFontconfig(JNIEnv* env, jobject, jstring fontconfigPath)
{
    ALOGV("Glue::initializeFontconfig(), path %p", fontconfigPath);
    jsize pathLength = env->GetStringUTFLength(fontconfigPath);
    const char* pathChars = env->GetStringUTFChars(fontconfigPath, 0);
    ALOGV("  pathLength %d, pathChars %s", pathLength, pathChars);

    setenv("FONTCONFIG_PATH", pathChars, 1);
}

using WebProcessEntryPoint = int(int, char**);

JNIEXPORT void JNICALL
Java_com_wpe_wpe_WebProcess_Glue_initializeMain(JNIEnv*, jobject, jint fd1, jint fd2)
{
    pipe_stdout_to_logcat();

    auto* entrypoint = reinterpret_cast<WebProcessEntryPoint*>(dlsym(RTLD_DEFAULT, "android_WebProcess_main"));
    ALOGV("Glue::initializeMain(), fd1 %d, fd2 %d, entrypoint %p", fd1, fd2, entrypoint);

    char fd1String[32];
    snprintf(fd1String, sizeof(fd1String), "%d", fd1);
    char fd2String[32];
    snprintf(fd2String, sizeof(fd1String), "%d", fd2);

    char* argv[3];
    argv[0] = "WPEWebProcess";
    argv[1] = fd1String;
    argv[2] = fd2String;
    (*entrypoint)(3, argv);
}

JNIEXPORT void JNICALL
Java_com_wpe_wpe_WebProcess_Glue_provideSurface(JNIEnv* env, jobject, jobject surface)
{
    ALOGV("Glue::provideSurface() surface object %p", surface);

    using ProvideSurfaceEntryPoint = void(ANativeWindow*);
    auto* entrypoint = reinterpret_cast<ProvideSurfaceEntryPoint*>(dlsym(RTLD_DEFAULT, "libwpe_android_provideNativeWindow"));

    ANativeWindow* nativeWindow = ANativeWindow_fromSurface(env, surface);
    ALOGV("  nativeWindow %p, entrypoint %p", nativeWindow, entrypoint);

    (*entrypoint)(nativeWindow);
}

