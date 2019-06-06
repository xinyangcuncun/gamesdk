/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <jni.h>
#include <Log.h>
#include <map>
#include "SwappyDisplayManager.h"
#include "Settings.h"

#define LOG_TAG "SwappyDisplayManager"

namespace swappy {

SwappyDisplayManager::SwappyDisplayManager(JavaVM* vm, jobject mainActivity) : mJVM(vm) {
    JNIEnv *env;
    mJVM->AttachCurrentThread(&env, nullptr);

    // Since we may call this from ANativeActivity we cannot use env->FindClass as the classpath
    // will be empty. Instead we need to work a bit harder
    jclass activity = env->GetObjectClass(mainActivity);
    jclass classLoader = env->FindClass("java/lang/ClassLoader");
    jmethodID getClassLoader = env->GetMethodID(activity,
                                                "getClassLoader",
                                                "()Ljava/lang/ClassLoader;");
    jmethodID loadClass = env->GetMethodID(classLoader,
                                           "loadClass",
                                           "(Ljava/lang/String;)Ljava/lang/Class;");
    jobject classLoaderObj = env->CallObjectMethod(mainActivity, getClassLoader);
    jstring swappyDisplayManagerName =
            env->NewStringUTF("com/google/androidgamesdk/SwappyDisplayManager");

    jclass swappyDisplayManagerClass = static_cast<jclass>(
            env->CallObjectMethod(classLoaderObj, loadClass, swappyDisplayManagerName));
    env->DeleteLocalRef(swappyDisplayManagerName);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        ALOGE("Unable to find com.google.androidgamesdk.SwappyDisplayManager class");
        ALOGE("Did you integrate extras ?");
        return;
    }

    jmethodID constructor = env->GetMethodID(
            swappyDisplayManagerClass,
            "<init>",
            "(JLandroid/app/Activity;)V");
    mSetPreferredRefreshRate = env->GetMethodID(
            swappyDisplayManagerClass,
            "setPreferredRefreshRate",
            "(I)V");
    jobject swappyDisplayManager = env->NewObject(swappyDisplayManagerClass,
                                                  constructor,
                                                  (jlong)this,
                                                  mainActivity);
    mJthis = env->NewGlobalRef(swappyDisplayManager);

    mInitialized = true;
}

SwappyDisplayManager::~SwappyDisplayManager() {
    JNIEnv *env;
    mJVM->AttachCurrentThread(&env, nullptr);

    env->DeleteGlobalRef(mJthis);
}

std::shared_ptr<SwappyDisplayManager::RefreshRateMap>
SwappyDisplayManager::getSupportedRefreshRates() {
    std::unique_lock<std::mutex> lock(mMutex);

    mCondition.wait(lock, [&]() { return mSupportedRefreshRates.get() != nullptr; });
    return mSupportedRefreshRates;
}

void SwappyDisplayManager::setPreferredRefreshRate(int index) {
    JNIEnv *env;
    mJVM->AttachCurrentThread(&env, nullptr);

    env->CallVoidMethod(mJthis, mSetPreferredRefreshRate, index);
}

// Helper class to wrap JNI entry points to SwappyDisplayManager
class SwappyDisplayManagerJNI {
public:
    static void onSetSupportedRefreshRates(jlong,
                                           std::shared_ptr<SwappyDisplayManager::RefreshRateMap>);
    static void onRefreshRateChanged(jlong, long, long, long);
};

void SwappyDisplayManagerJNI::onSetSupportedRefreshRates(jlong cookie,
        std::shared_ptr<SwappyDisplayManager::RefreshRateMap> refreshRates) {
    auto *sDM = reinterpret_cast<SwappyDisplayManager*>(cookie);

    std::lock_guard<std::mutex> lock(sDM->mMutex);
    sDM->mSupportedRefreshRates = std::move(refreshRates);
    sDM->mCondition.notify_one();
}

void SwappyDisplayManagerJNI::onRefreshRateChanged(jlong /*cookie*/,
                                                   long refreshPeriod,
                                                   long appOffset,
                                                   long sfOffset) {
    using std::chrono::nanoseconds;
    Settings::DisplayTimings displayTimings;
    displayTimings.refreshPeriod = nanoseconds(refreshPeriod);
    displayTimings.appOffset = nanoseconds(appOffset);
    displayTimings.sfOffset = nanoseconds(sfOffset);
    Settings::getInstance()->setDisplayTimings(displayTimings);
}

extern "C" {

JNIEXPORT void JNICALL
Java_com_google_androidgamesdk_SwappyDisplayManager_nSetSupportedRefreshRates(
                                                                            JNIEnv *env,
                                                                            jobject /* this */,
                                                                            jlong cookie,
                                                                            jlongArray refreshRates,
                                                                            jintArray modeIds) {
    int length = env->GetArrayLength(refreshRates);
    auto refreshRatesMap =
            std::make_shared<SwappyDisplayManager::RefreshRateMap>();

    jlong *refreshRatesArr = env->GetLongArrayElements(refreshRates, 0);
    jint *modeIdsArr = env->GetIntArrayElements(modeIds, 0);
    for (int i = 0; i < length; i++) {
        (*refreshRatesMap)[std::chrono::nanoseconds(refreshRatesArr[i])] = modeIdsArr[i];
    }
    env->ReleaseLongArrayElements(refreshRates, refreshRatesArr, 0);
    env->ReleaseIntArrayElements(modeIds, modeIdsArr, 0);

    SwappyDisplayManagerJNI::onSetSupportedRefreshRates(cookie, refreshRatesMap);
}

JNIEXPORT void JNICALL
Java_com_google_androidgamesdk_SwappyDisplayManager_nOnRefreshRateChanged(JNIEnv *env,
                                                                          jobject /* this */,
                                                                          jlong cookie,
                                                                          jlong refreshPeriod,
                                                                          jlong appOffset,
                                                                          jlong sfOffset) {
    SwappyDisplayManagerJNI::onRefreshRateChanged(cookie, refreshPeriod, appOffset, sfOffset);
}

} // extern "C"

} // namespace swappy
