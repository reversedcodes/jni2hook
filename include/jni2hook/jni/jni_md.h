/*
 * Platform dispatch for the machine dependent part of the JNI headers.
 *
 * jni.h reaches this file through #include "jni_md.h", which resolves next to
 * jni.h itself, so no include path setup is needed. The two files it selects
 * between are vendored from OpenJDK unchanged and both carry the original
 * _JAVASOFT_JNI_MD_H_ guard, which is why exactly one of them may be included.
 *
 * OpenJDK ships one variant for Windows and one for every other platform; the
 * darwin/jni_md.h found in a macOS JDK is byte for byte the unix one.
 */

#if defined(_WIN32) || defined(_WIN64)
  #include "jni_md_windows.h"
#else
  #include "jni_md_unix.h"
#endif
