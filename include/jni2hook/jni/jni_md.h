/*
 * Platform dispatch for the machine dependent part of the JNI headers.
 *
 * jni.h reaches this file through #include "jni_md.h", which resolves next to
 * jni.h itself, so no include path setup is needed. The two files it selects
 * between are vendored from OpenJDK unchanged and both carry the original
 * _JAVASOFT_JNI_MD_H_ guard, which is why exactly one of them may be included.
 *
 * OpenJDK ships one variant for Windows and one for every other platform; the
 * darwin/jni_md.h found in a macOS JDK is byte for byte the unix one, so these
 * two cover all three targets.
 *
 * Everything in this directory comes from OpenJDK 21, the current LTS:
 *
 *   jni.h, jvmti.h, jvmticmlr.h  $JAVA_HOME/include/
 *   jni_md_unix.h                src/java.base/unix/native/include/jni_md.h
 *   jni_md_windows.h             src/java.base/windows/native/include/jni_md.h
 *
 * 21 rather than the newest release on purpose: JNINativeInterface_ and
 * jvmtiInterface_1_ only ever gain entries at the end, so building against an
 * older header keeps the result runnable on every newer JVM. To move to another
 * JDK, replace the files here; the build references them in one place, the
 * include directory list in CMakeLists.txt.
 *
 * The vendored files carry Oracle's GPLv2 with Classpath Exception header,
 * which has to stay intact. That exception is what allows linking them into a
 * differently licensed program.
 */

#if defined(_WIN32) || defined(_WIN64)
  #include "jni_md_windows.h"
#else
  #include "jni_md_unix.h"
#endif
