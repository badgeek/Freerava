// cyclomp — access to the JavaVM and the Activity captured in main.cpp's
// interposed ANativeActivity_onCreate. Needed by the JNI HTTP transport and
// the JNI GPS telemetry source.
#pragma once
#ifdef __ANDROID__
#include <jni.h>
JavaVM *cyclomp_java_vm();
// Global ref to the NativeActivity instance (valid for the process lifetime),
// used as the Context/Activity for getSystemService and permission calls.
jobject cyclomp_activity();
#endif
