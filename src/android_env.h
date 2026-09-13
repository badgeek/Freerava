// cyclomp — access to the JavaVM captured in main.cpp's interposed
// ANativeActivity_onCreate. Needed by the JNI HTTP transport.
#pragma once
#ifdef __ANDROID__
#include <jni.h>
JavaVM *cyclomp_java_vm();
#endif
