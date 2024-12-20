# @note About Android NDK
# Android NDK r25c supports API level as following:
# - From 19 (Kitkat, 4.4) to 33
# - https://developer.android.com/ndk/guides/stable_apis#a33
#
# @note About Application ABI
# If you want to generate a binary file for all architectures, please append additional architech name
# such as "arm64-v8a armeabi-v7a x86 x86_64" as following:
# APP_ABI = armeabi armeabi-v7a arm64-v8a x86 x86_64

APP_ABI           := arm64-v8a
LIBCXX_USE_GABIXX := true
APP_STL           := c++_shared
APP_PLATFORM      := android-24
APP_CPPFLAGS      := -std=c++17
