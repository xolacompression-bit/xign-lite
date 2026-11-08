# the native library registers its methods on this class by name (JNI_OnLoad): keep the class and method names
-keep class com.equinumcloud.lite.Native { native <methods>; }
