/* Fake JNI for running an Android game's arm64 code outside the JVM: every JNIEnv/JavaVM slot is a stub that
 * logs its name once and returns a harmless value. See gen_jnishim.py for the rules. */
#pragma once
#include "jni.h"
JNIEnv *jnishim_env(void);      /* for SDL_AndroidGetJNIEnv */
JavaVM *jnishim_vm(void);
jobject jnishim_activity(void); /* for SDL_AndroidGetActivity: a non-NULL dummy */
