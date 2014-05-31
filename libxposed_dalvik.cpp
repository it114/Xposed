/**
 * This file includes functions specific to the Dalvik runtime.
 */

#define LOG_TAG "Xposed"

#include "libxposed_dalvik.h"
#include "xposed_offsets.h"

#include <dlfcn.h>

namespace xposed {

////////////////////////////////////////////////////////////
// Forward declarations
////////////////////////////////////////////////////////////

bool initMemberOffsets(JNIEnv* env);
void prepareSubclassReplacement(jclass clazz);
void hookedMethodCallback(const u4* args, JValue* pResult, const Method* method, ::Thread* self);
void XposedBridge_invokeOriginalMethodNative(const u4* args, JValue* pResult, const Method* method, ::Thread* self);


////////////////////////////////////////////////////////////
// Variables
////////////////////////////////////////////////////////////

static Method* xposedHandleHookedMethod = NULL;
static ClassObject* objectArrayClass = NULL;
static size_t arrayContentsOffset = 0;
static void* PTR_gDvmJit = NULL;


////////////////////////////////////////////////////////////
// Library initialization
////////////////////////////////////////////////////////////

/** Called by Xposed's app_process replacement. */
bool xposedInitLib(xposed::XposedShared* shared) {
    xposed = shared;

    xposed->onVmCreated = &onVmCreated;
    return true;
}

/** Called very early during VM startup. */
void onVmCreated(JNIEnv* env, const char* className) {
    startClassName = className;

    if (!initMemberOffsets(env))
        return;

    jclass miuiResourcesClass = env->FindClass(MIUI_RESOURCES_CLASS);
    if (miuiResourcesClass != NULL) {
        ClassObject* clazz = (ClassObject*)dvmDecodeIndirectRef(dvmThreadSelf(), miuiResourcesClass);
        if (dvmIsFinalClass(clazz)) {
            ALOGD("Removing final flag for class '%s'", MIUI_RESOURCES_CLASS);
            clazz->accessFlags &= ~ACC_FINAL;
        }
    }
    env->ExceptionClear();

    jclass xTypedArrayClass = env->FindClass(XTYPEDARRAY_CLASS);
    if (xTypedArrayClass == NULL) {
        ALOGE("Error while loading XTypedArray class '%s':", XTYPEDARRAY_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return;
    }
    prepareSubclassReplacement(xTypedArrayClass);

    xposedClass = env->FindClass(XPOSED_CLASS);
    xposedClass = reinterpret_cast<jclass>(env->NewGlobalRef(xposedClass));
    
    if (xposedClass == NULL) {
        ALOGE("Error while loading Xposed class '%s':", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return;
    }

    ALOGI("Found Xposed class '%s', now initializing", XPOSED_CLASS);
    if (register_natives_XposedBridge(env) != JNI_OK) {
        ALOGE("Could not register natives for '%s'", XPOSED_CLASS);
        env->ExceptionClear();
        return;
    }

    xposedLoadedSuccessfully = true;
}

bool initMemberOffsets(JNIEnv* env) {
    PTR_gDvmJit = dlsym(RTLD_DEFAULT, "gDvmJit");

    if (PTR_gDvmJit == NULL) {
        offsetMode = MEMBER_OFFSET_MODE_NO_JIT;
    } else {
        offsetMode = MEMBER_OFFSET_MODE_WITH_JIT;
    }
    ALOGD("Using structure member offsets for mode %s", xposedOffsetModesDesc[offsetMode]);

    MEMBER_OFFSET_COPY(DvmJitGlobals, codeCacheFull);

    int overrideCodeCacheFull = readIntConfig(XPOSED_OVERRIDE_JIT_RESET_OFFSET, -1);
    if (overrideCodeCacheFull > 0 && overrideCodeCacheFull < 0x400) {
        ALOGI("Offset for DvmJitGlobals.codeCacheFull is overridden, new value is 0x%x", overrideCodeCacheFull);
        MEMBER_OFFSET_VAR(DvmJitGlobals, codeCacheFull) = overrideCodeCacheFull;
    }

    // detect offset of ArrayObject->contents
    jintArray dummyArray = env->NewIntArray(1);
    if (dummyArray == NULL) {
        ALOGE("Could allocate int array for testing");
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    jint* dummyArrayElements = env->GetIntArrayElements(dummyArray, NULL);
    arrayContentsOffset = (size_t)dummyArrayElements - (size_t)dvmDecodeIndirectRef(dvmThreadSelf(), dummyArray);
    env->ReleaseIntArrayElements(dummyArray,dummyArrayElements, 0);
    env->DeleteLocalRef(dummyArray);

    if (arrayContentsOffset < 12 || arrayContentsOffset > 128) {
        ALOGE("Detected strange offset %d of ArrayObject->contents", arrayContentsOffset);
        return false;
    }

    return true;
}


////////////////////////////////////////////////////////////
// Utility methods
////////////////////////////////////////////////////////////

/** Portable clone of dvmSetObjectArrayElement() */
inline void setObjectArrayElement(const ArrayObject* obj, int index, Object* val) {
    uintptr_t arrayContents = (uintptr_t)obj + arrayContentsOffset;
    ((Object **)arrayContents)[index] = val;
    dvmWriteBarrierArray(obj, index, index + 1);
}

/** Lay the foundations for XposedBridge.setObjectClassNative() */
void prepareSubclassReplacement(jclass clazz) {
    // clazz is supposed to replace its superclass, so make sure enough memory is allocated
    ClassObject* sub = (ClassObject*) dvmDecodeIndirectRef(dvmThreadSelf(), clazz);
    ClassObject* super = sub->super;
    super->objectSize = sub->objectSize;
}

/** Wrapper used by the common part of the library. */
void logExceptionStackTrace() {
    dvmLogExceptionStackTrace();
}

/** Check whether a method is already hooked. */
inline bool isMethodHooked(const Method* method) {
    return (method->nativeFunc == &hookedMethodCallback);
}

////////////////////////////////////////////////////////////
// JNI methods
////////////////////////////////////////////////////////////

jboolean callback_XposedBridge_initNative(JNIEnv* env) {
    xposedHandleHookedMethod = (Method*) env->GetStaticMethodID(xposedClass, "handleHookedMethod",
        "(Ljava/lang/reflect/Member;ILjava/lang/Object;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (xposedHandleHookedMethod == NULL) {
        ALOGE("ERROR: could not find method %s.handleHookedMethod(Member, int, Object, Object, Object[])", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    Method* xposedInvokeOriginalMethodNative = (Method*) env->GetStaticMethodID(xposedClass, "invokeOriginalMethodNative",
        "(Ljava/lang/reflect/Member;I[Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (xposedInvokeOriginalMethodNative == NULL) {
        ALOGE("ERROR: could not find method %s.invokeOriginalMethodNative(Member, int, Class[], Class, Object, Object[])", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    dvmSetNativeFunc(xposedInvokeOriginalMethodNative, XposedBridge_invokeOriginalMethodNative, NULL);

    objectArrayClass = dvmFindArrayClass("[Ljava/lang/Object;", NULL);
    if (objectArrayClass == NULL) {
        ALOGE("Error while loading Object[] class");
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    return true;
}

/** This is called when a hooked method is executed. */
void hookedMethodCallback(const u4* args, JValue* pResult, const Method* method, ::Thread* self) {
    if (!isMethodHooked(method)) {
        dvmThrowNoSuchMethodError("Could not find Xposed original method - how did you even get here?");
        return;
    }

    XposedHookInfo* hookInfo = (XposedHookInfo*) method->insns;
    Method* original = (Method*) hookInfo;
    Object* originalReflected = hookInfo->reflectedMethod;
    Object* additionalInfo = hookInfo->additionalInfo;
  
    // convert/box arguments
    const char* desc = &method->shorty[1]; // [0] is the return type.
    Object* thisObject = NULL;
    size_t srcIndex = 0;
    size_t dstIndex = 0;

    // for non-static methods determine the "this" pointer
    if (!dvmIsStaticMethod(original)) {
        thisObject = (Object*) args[0];
        srcIndex++;
    }

    ArrayObject* argsArray = dvmAllocArrayByClass(objectArrayClass, strlen(method->shorty) - 1, ALLOC_DEFAULT);
    if (argsArray == NULL) {
        return;
    }

    while (*desc != '\0') {
        char descChar = *(desc++);
        JValue value;
        Object* obj;

        switch (descChar) {
        case 'Z':
        case 'C':
        case 'F':
        case 'B':
        case 'S':
        case 'I':
            value.i = args[srcIndex++];
            obj = (Object*) dvmBoxPrimitive(value, dvmFindPrimitiveClass(descChar));
            dvmReleaseTrackedAlloc(obj, self);
            break;
        case 'D':
        case 'J':
            value.j = dvmGetArgLong(args, srcIndex);
            srcIndex += 2;
            obj = (Object*) dvmBoxPrimitive(value, dvmFindPrimitiveClass(descChar));
            dvmReleaseTrackedAlloc(obj, self);
            break;
        case '[':
        case 'L':
            obj  = (Object*) args[srcIndex++];
            break;
        default:
            ALOGE("Unknown method signature description character: %c", descChar);
            obj = NULL;
            srcIndex++;
        }
        setObjectArrayElement(argsArray, dstIndex++, obj);
    }
    
    // call the Java handler function
    JValue result;
    dvmCallMethod(self, xposedHandleHookedMethod, NULL, &result,
        originalReflected, (int) original, additionalInfo, thisObject, argsArray);
        
    dvmReleaseTrackedAlloc(argsArray, self);

    // exceptions are thrown to the caller
    if (dvmCheckException(self)) {
        return;
    }

    // return result with proper type
    ClassObject* returnType = dvmGetBoxedReturnType(method);
    if (returnType->primitiveType == PRIM_VOID) {
        // ignored
    } else if (result.l == NULL) {
        if (dvmIsPrimitiveClass(returnType)) {
            dvmThrowNullPointerException("null result when primitive expected");
        }
        pResult->l = NULL;
    } else {
        if (!dvmUnboxPrimitive(result.l, returnType, pResult)) {
            dvmThrowClassCastException(result.l->clazz, returnType);
        }
    }
}


void XposedBridge_hookMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethodIndirect,
            jobject declaredClassIndirect, jint slot, jobject additionalInfoIndirect) {
    // Usage errors?
    if (declaredClassIndirect == NULL || reflectedMethodIndirect == NULL) {
        dvmThrowIllegalArgumentException("method and declaredClass must not be null");
        return;
    }
    
    // Find the internal representation of the method
    ClassObject* declaredClass = (ClassObject*) dvmDecodeIndirectRef(dvmThreadSelf(), declaredClassIndirect);
    Method* method = dvmSlotToMethod(declaredClass, slot);
    if (method == NULL) {
        dvmThrowNoSuchMethodError("Could not get internal representation for method");
        return;
    }
    
    if (isMethodHooked(method)) {
        // already hooked
        return;
    }
    
    // Save a copy of the original method and other hook info
    XposedHookInfo* hookInfo = (XposedHookInfo*) calloc(1, sizeof(XposedHookInfo));
    memcpy(hookInfo, method, sizeof(hookInfo->originalMethodStruct));
    hookInfo->reflectedMethod = dvmDecodeIndirectRef(dvmThreadSelf(), env->NewGlobalRef(reflectedMethodIndirect));
    hookInfo->additionalInfo = dvmDecodeIndirectRef(dvmThreadSelf(), env->NewGlobalRef(additionalInfoIndirect));

    // Replace method with our own code
    SET_METHOD_FLAG(method, ACC_NATIVE);
    method->nativeFunc = &hookedMethodCallback;
    method->insns = (const u2*) hookInfo;
    method->registersSize = method->insSize;
    method->outsSize = 0;

    if (PTR_gDvmJit != NULL) {
        // reset JIT cache
        char currentValue = *((char*)PTR_gDvmJit + MEMBER_OFFSET_VAR(DvmJitGlobals,codeCacheFull));
        if (currentValue == 0 || currentValue == 1) {
            MEMBER_VAL(PTR_gDvmJit, DvmJitGlobals, codeCacheFull) = true;
        } else {
            ALOGE("Unexpected current value for codeCacheFull: %d", currentValue);
        }
    }
}


/**
 * Simplified copy of Method.invokeNative(), but calls the original (non-hooked) method
 * and has no access checks. Used to call the real implementation of hooked methods.
 */
void XposedBridge_invokeOriginalMethodNative(const u4* args, JValue* pResult,
            const Method* method, ::Thread* self) {
    Method* meth = (Method*) args[1];
    if (meth == NULL) {
        meth = dvmGetMethodFromReflectObj((Object*) args[0]);
        if (isMethodHooked(meth)) {
            meth = (Method*) meth->insns;
        }
    }
    ArrayObject* params = (ArrayObject*) args[2];
    ClassObject* returnType = (ClassObject*) args[3];
    Object* thisObject = (Object*) args[4]; // null for static methods
    ArrayObject* argList = (ArrayObject*) args[5];

    // invoke the method
    pResult->l = dvmInvokeMethod(thisObject, meth, argList, params, returnType, true);
    return;
}

void XposedBridge_setObjectClassNative(JNIEnv* env, jclass clazz, jobject objIndirect, jclass clzIndirect) {
    Object* obj = (Object*) dvmDecodeIndirectRef(dvmThreadSelf(), objIndirect);
    ClassObject* clz = (ClassObject*) dvmDecodeIndirectRef(dvmThreadSelf(), clzIndirect);
    if (clz->status < CLASS_INITIALIZED && !dvmInitClass(clz)) {
        ALOGE("Could not initialize class %s", clz->descriptor);
        return;
    }
    obj->clazz = clz;
}

void XposedBridge_dumpObjectNative(JNIEnv* env, jclass clazz, jobject objIndirect) {
    Object* obj = (Object*) dvmDecodeIndirectRef(dvmThreadSelf(), objIndirect);
    dvmDumpObject(obj);
}

jobject XposedBridge_cloneToSubclassNative(JNIEnv* env, jclass clazz, jobject objIndirect, jclass clzIndirect) {
    Object* obj = (Object*) dvmDecodeIndirectRef(dvmThreadSelf(), objIndirect);
    ClassObject* clz = (ClassObject*) dvmDecodeIndirectRef(dvmThreadSelf(), clzIndirect);

    jobject copyIndirect = env->AllocObject(clzIndirect);
    if (copyIndirect == NULL)
        return NULL;

    Object* copy = (Object*) dvmDecodeIndirectRef(dvmThreadSelf(), copyIndirect);
    size_t size = obj->clazz->objectSize;
    size_t offset = sizeof(Object);
    memcpy((char*)copy + offset, (char*)obj + offset, size - offset);

    if (IS_CLASS_FLAG_SET(clz, CLASS_ISFINALIZABLE))
        dvmSetFinalizable(copy);

    return copyIndirect;
}

jint XposedBridge_getRuntime(JNIEnv* env, jclass clazz) {
    return 1; // RUNTIME_DALVIK
}


////////////////////////////////////////////////////////////
// JNI methods registrations
////////////////////////////////////////////////////////////

int register_natives_XposedBridge(JNIEnv* env) {
    const JNINativeMethod natives[] = {
        {"getStartClassName", "()Ljava/lang/String;", (void*)XposedBridge_getStartClassName},
        {"getRuntime", "()I", (void*)XposedBridge_getRuntime},
        {"initNative", "()Z", (void*)XposedBridge_initNative},
        {"hookMethodNative", "(Ljava/lang/reflect/Member;Ljava/lang/Class;ILjava/lang/Object;)V", (void*)XposedBridge_hookMethodNative},
        {"setObjectClassNative", "(Ljava/lang/Object;Ljava/lang/Class;)V", (void*)XposedBridge_setObjectClassNative},
        {"dumpObjectNative", "(Ljava/lang/Object;)V", (void*)XposedBridge_dumpObjectNative},
        {"cloneToSubclassNative", "(Ljava/lang/Object;Ljava/lang/Class;)Ljava/lang/Object;", (void*)XposedBridge_cloneToSubclassNative},
    };
    return env->RegisterNatives(xposedClass, natives, NELEM(natives));
}

int register_natives_XResources(JNIEnv* env) {
    const JNINativeMethod natives[] = {
        {"rewriteXmlReferencesNative", "(ILandroid/content/res/XResources;Landroid/content/res/Resources;)V",
            (void*)XResources_rewriteXmlReferencesNative},
    };
    return env->RegisterNatives(xresourcesClass, natives, NELEM(natives));
}

} // namespace android
